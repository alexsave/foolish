// GameSwitchTests — tapping a DIFFERENT game's bubble (1.0(37)).
//
// Owner: "I notice (and this is rare) that if you have one game open, and you
// scroll up and hit a different game bubble, it should completely switch to that
// other game. Not rebase, completely switch. If a bubble was staged, make the
// bubble a noop."
//
// A thread holds many games and every bubble in it stays tappable forever, so
// this is an ordinary afternoon, not an edge case. Two things went wrong, and
// they are independent - each is reproduced separately below.
//
// 1. THE SWITCH DID NOT ALWAYS HAPPEN. `StagedBubbleRouting` pins the presented
//    URL when the selected bubble is one THIS device staged or just sent, so my
//    own bubble becoming the selection does not tear the live board down and
//    replay the move I just watched. The pin is right; outliving its board is
//    not. Send in game B, tap game A (that switches - A is neither marker), then
//    tap game B's bubble again: it IS `lastSentPayload`, the pin fires, and the
//    URL handed back is game A's. loadKey never moves, the surface never
//    reloads, and tapping game B leaves game A on screen.
//
// 2. THE SEND OF A LEFT-BEHIND DRAFT REBASED THE BOARD ON SCREEN. A staged
//    bubble is a DRAFT and Messages offers no call to remove one, so it survives
//    the tap. Press Send and `markSent` runs on the board you switched TO, with
//    bytes it never sealed - and the "not my bytes" refusal cannot catch that,
//    because it is "did I seal these" and a board that sealed nothing has no
//    opinion. The verdict was REBASE: `base` became the other game's chain,
//    decoded MASKED FOR THIS BOARD'S SEAT NUMBER, which over there is somebody
//    else. The rule is now the kernel's (MSG_TURN_SEND_OTHERGAME).
//
// WHAT THESE FAIL AGAINST (every mutation actually run):
//   Route.clearMarkers hard-coded false (the pin never expires) -> the routing test
//   markSent drops the same-game ask (always rebase)            -> the rebase test
//   markSent refuses but adopts the base anyway                 -> the rebase test
//   the kernel gate answers REBASE for another game             -> the gate test
//
// NO RIG SCREENSHOT PAIR FOR THIS ONE, and the reason is a rig defect worth
// knowing about rather than a shortcut. FoolishHarness models `lastPayloadURL`
// as `HarnessModel.presentedURL`, and `rememberPresented()` sets that to the
// RAW SELECTION - while the extension records the ROUTED url
// (`MessagesViewController.present`'s `lastPayloadURL = payloadURL`). With the
// pin's output thrown away on every tap, the harness cannot pose either half of
// this: it always presents the bubble that was tapped, so it looks correct
// whichever way `route` answers. Making it faithful is surgery across ~30
// scenarios and was left alone. These tests drive the shipped decisions
// directly instead - the pure router, and a real controller over the real
// kernel - and each is shown to fail against the behaviour that shipped.
import XCTest
@testable import FoolishKit

@MainActor
final class GameSwitchTests: XCTestCase {

    /// Seal a fresh LIVE 2p chain under `gameId`. Each call re-deals, so the two
    /// chains this file compares are genuinely different games and not one game
    /// twice.
    private func makeGame(_ gameId: UInt64, seed: UInt8) async throws -> Data {
        let k = MessageKernel.shared
        try await k.newGame(seed: Data(repeating: seed, count: 32), players: 2)
        return try await k.seal(phase: 2, lastActorSeat: 0, gameId: gameId,
                                parent8: Data(repeating: 0, count: 8),
                                joins: [MessageJoin(seat: 0, name: "Alex"),
                                        MessageJoin(seat: 1, name: "Vera")])
    }

    // MARK: - 1. the tap

    /// The routing decision, driven with real sealed chains and no simulator.
    func testTappingTheOtherGameSwitchesEvenAfterSendingInThisOne() async throws {
        let a = try await makeGame(0xA001, seed: 11)
        let b = try await makeGame(0xB002, seed: 22)
        XCTAssertNotEqual(a, b)
        let urlA = MessageEnvelope.link(payload: a)
        let urlB = MessageEnvelope.link(payload: b)

        // I sent in game B and I am standing on game B's board.
        var lastPayloadURL: URL? = urlB
        var lastSent: Data? = b

        // Scroll up, tap game A. Neither marker describes it, so it routes as
        // itself - and the markers are spent, because they pin a board that is
        // about to leave the screen.
        let toA = StagedBubbleRouting.route(selectedURL: urlA, startingNewGame: false,
                                            pendingStage: nil, lastPayloadURL: lastPayloadURL,
                                            lastSentPayload: lastSent)
        XCTAssertEqual(toA.url, urlA, "the tapped bubble is what gets presented")
        XCTAssertTrue(toA.clearMarkers,
                      "the presentation moved, so the just-sent pin has nothing left to protect")
        if toA.clearMarkers { lastSent = nil }
        lastPayloadURL = toA.url

        // …and now tap game B again. THIS is the reported bug: with the pin
        // still live, `isMine(b)` hands back `lastPayloadURL`, which is game A.
        let backToB = StagedBubbleRouting.route(selectedURL: urlB, startingNewGame: false,
                                                pendingStage: nil, lastPayloadURL: lastPayloadURL,
                                                lastSentPayload: lastSent)
        XCTAssertEqual(backToB.url, urlB,
                       "tapping game B's bubble must present GAME B - a pin that outlived its "
                       + "board would hand back game A's URL, loadKey would never move, and the "
                       + "surface would never reload")
    }

    /// …and the pin itself is untouched where it belongs: my own bubble becoming
    /// the selection while I am still standing on its board keeps the board.
    func testTheOwnBubblePinStillHoldsOnItsOwnBoard() async throws {
        let b = try await makeGame(0xB002, seed: 22)
        let parent = MessageEnvelope.link(payload: try await makeGame(0xB002, seed: 23))
        let r = StagedBubbleRouting.route(selectedURL: MessageEnvelope.link(payload: b),
                                          startingNewGame: false,
                                          pendingStage: (payload: b, mySeat: 0),
                                          lastPayloadURL: parent,
                                          lastSentPayload: nil)
        XCTAssertEqual(r.url, parent,
                       "my own just-staged bubble becoming the selection must NOT move the "
                       + "presented URL - that teardown replays the move I just watched")
        XCTAssertFalse(r.clearMarkers, "and nothing is spent, because nothing moved")
    }

    // MARK: - 2. the send

    /// A left-behind draft's Send, arriving at the board the human switched TO.
    func testSendingAnotherGamesDraftLeavesThisBoardAlone() async throws {
        let a = try await makeGame(0xA001, seed: 11)
        let b = try await makeGame(0xB002, seed: 22)

        let envA = try await MessageEnvelope.decode(payload: a, viewer: 0)
        let c = MessageTurnController(parentPayload: a, parent: envA, mySeat: 0)
        await c.begin()
        let boardBefore = c.view
        XCTAssertNotNil(boardBefore)
        XCTAssertEqual(c.gameIdString, envA.gameId)

        // The human presses Send on the bubble still sitting in the input field
        // - game B's, composed before they tapped away.
        await c.markSent(payload: b)

        XCTAssertEqual(c.gameIdString, envA.gameId,
                       "THE BOARD IS STILL GAME A. A rebase here adopts game B's chain as this "
                       + "controller's base and decodes it masked for seat 0 OF GAME B, which is "
                       + "a different player.")
        XCTAssertEqual(c.basePayload, a, "the base never moved off the chain this board opened")
        XCTAssertEqual(c.view?.me?.hand, boardBefore?.me?.hand,
                       "and my hand is my hand")
        XCTAssertFalse(c.sending, "the send window is closed either way - a refusal is a refusal "
                       + "to REBASE, never a board left stuck mid-send")
    }

    /// The kernel's own gate, through the bridge. `sameGame` is asked on the
    /// second call only, because it is not knowable before the decode.
    func testTheOtherGameGateIsTheKernels() {
        XCTAssertEqual(TurnWire.sendVerdict(staged: false, host: true, sealed: false,
                                            hostIsSealed: false, decoded: true, sameGame: true),
                       .rebase, "my own game's chain, adopted as ever")
        XCTAssertEqual(TurnWire.sendVerdict(staged: false, host: true, sealed: false,
                                            hostIsSealed: false, decoded: true, sameGame: false),
                       .otherGame, "same facts, different game - never this board's to adopt")
        XCTAssertEqual(TurnWire.sendVerdict(staged: false, host: true, sealed: false,
                                            hostIsSealed: false, decoded: nil, sameGame: nil),
                       .decode, "the game cannot be asked about before the decode")
        XCTAssertEqual(TurnWire.sendVerdict(staged: false, host: true, sealed: false,
                                            hostIsSealed: false, decoded: false, sameGame: false),
                       .unreadable,
                       "bytes that will not decode are unreadable whoever they belong to")
    }
}
