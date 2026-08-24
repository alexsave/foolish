// PODKIDNOY, the throw-in game with no transfer - the phone's half.
//
// The RULE is C's and is pinned there (c/tests/tests.c test_podkidnoy for the
// engine, c/tests/msg_wire_test.c test_podkidnoy_wire for the codec and the
// wire). What can only break HERE is the plumbing between a checkbox and a
// dealt game: a lobby that chooses the variant, a bubble that says so, a Start
// that keeps it across the re-deal, a board whose buttons and drags follow, and
// a rulebook that stops teaching a move nobody can make.
//
// Every assertion about what a player may do asks the KERNEL's legal menu
// rather than a Swift copy of the rule, because that menu is the only thing the
// buttons and the drag are built from - which is exactly why this variant needs
// no per-control gate.
import XCTest
@testable import FoolishKit

@MainActor
final class PodkidnoyTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 13 &+ Int(salt)) | 1 })
    }

    private func roster(_ names: [String]) -> [MessageJoin] {
        names.enumerated().map { MessageJoin(seat: $0.offset, name: $0.element) }
    }

    /// A lobby sealed the way MessagesRootView seals one, with the checkbox in
    /// the given position.
    private func lobby(seed: Data, gameId: UInt64, joins: [MessageJoin],
                       passing: Bool, capacity: Int = 8)
        async throws -> (Data, MessageEnvelope) {
        let k = MessageKernel.shared
        try await k.newGame(seed: seed, players: capacity)
        await k.setPassing(passing)
        let payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gameId,
                                       parent8: Data(repeating: 0, count: 8),
                                       joins: joins, sentAt: 0x1234)
        return (payload, try await MessageEnvelope.decode(payload: payload, viewer: -1))
    }

    // MARK: the lobby chooses, the wire carries

    /// A WAITING lobby has no body at all - the deal alone is its state - so if
    /// the rules did not ride its HEADER there would be nowhere for them to be
    /// until the game started, and the other players would learn the variant
    /// from a board that refuses their transfer.
    func testTheLobbySaysWhichGameItIs() async throws {
        let (_, on) = try await lobby(seed: freshSeed(1), gameId: 6001,
                                      joins: roster(["Alex"]), passing: true)
        XCTAssertTrue(on.passingAllowed, "the default lobby is the classic game")

        let (_, off) = try await lobby(seed: freshSeed(2), gameId: 6002,
                                       joins: roster(["Alex"]), passing: false)
        XCTAssertFalse(off.passingAllowed, "the checkbox did not reach the wire")
    }

    /// …and a FRESH game is the classic one whatever the last lobby chose. The
    /// kernel is a process-wide resident game; without this, one podkidnoy
    /// lobby would quietly make every later game on the device podkidnoy too.
    func testAFreshGameIsTheClassicGame() async throws {
        _ = try await lobby(seed: freshSeed(3), gameId: 6003,
                            joins: roster(["Alex"]), passing: false)
        let k = MessageKernel.shared
        let afterLobby = await k.passingAllowed()
        XCTAssertFalse(afterLobby)
        try await k.newGame(seed: freshSeed(4), players: 2)
        let afterNewGame = await k.passingAllowed()
        XCTAssertTrue(afterNewGame, "a new deal inherited the last lobby's rules")
    }

    /// START re-derives the deal from the locked seed at the joined count, which
    /// is a whole new Game - and the rules have to cross that, because the lobby
    /// that chose them is now several bubbles back.
    func testStartKeepsTheRulesAcrossTheRedeal() async throws {
        let joins = roster(["Alex", "Dima"])
        let (payload, env) = try await lobby(seed: freshSeed(5), gameId: 6004,
                                             joins: joins, passing: false, capacity: 2)
        let live = try await MessageKernel.shared.startFromLobby(
            lobbyPayload: payload, gameId: 6004, actingSeat: 1,
            parent8: MessageTurnController.firstEight(hex: env.digest),
            joins: joins, sentAt: 0x1234)
        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
        XCTAssertEqual(liveEnv.phase, 2)
        XCTAssertFalse(liveEnv.passingAllowed, "Start dealt the classic game instead")
    }

    // MARK: the board follows, without being told

    /// The defender's menu in a podkidnoy game has no transfer in it - and the
    /// SAME deal played the classic way does, which is what makes this a test
    /// of the variant rather than of a position with no transfer in it anyway.
    ///
    /// This is the assertion the Pass button and the drag rest on: both are
    /// built by CardPlay from this menu (canPass / resolve), so a rule enforced
    /// here cannot be forgotten by a control.
    func testADefenderIsNeverOfferedATransfer() async throws {
        var sawTransferInTheClassicGame = false

        for salt in UInt8(10)...UInt8(40) {
            let joins = roster(["Alex", "Dima"])
            // The classic control first: play the opening attack and look at
            // the defender's menu.
            let (p1, e1) = try await lobby(seed: freshSeed(salt), gameId: 6100,
                                           joins: joins, passing: true, capacity: 2)
            let classicMenu = try await defenderMenuAfterOpening(
                lobbyPayload: p1, env: e1, joins: joins, gameId: 6100)
            let classicHasPass = classicMenu.contains { $0.type == .pass }

            // …the same seed, podkidnoy.
            let (p2, e2) = try await lobby(seed: freshSeed(salt), gameId: 6101,
                                           joins: joins, passing: false, capacity: 2)
            let podkidnoyMenu = try await defenderMenuAfterOpening(
                lobbyPayload: p2, env: e2, joins: joins, gameId: 6101)
            XCTAssertFalse(podkidnoyMenu.contains { $0.type == .pass },
                           "seed \(salt): a podkidnoy defender was offered a transfer")

            if classicHasPass {
                sawTransferInTheClassicGame = true
                // The rest of the menu is untouched: this variant removes ONE
                // move, it does not narrow the defence.
                let cover = { (m: Move) in m.type == .cover || m.type == .pickup }
                XCTAssertEqual(podkidnoyMenu.filter(cover).count,
                               classicMenu.filter(cover).count,
                               "seed \(salt): podkidnoy changed the covers too")
                // And the buttons: the same selection that could be passed in
                // the classic game cannot be here.
                let passCards = classicMenu.first { $0.type == .pass }!.cards
                XCTAssertTrue(CardPlay.canPass(passCards, legal: classicMenu))
                XCTAssertFalse(CardPlay.canPass(passCards, legal: podkidnoyMenu),
                               "seed \(salt): the Pass button would still be offered")
                // …and dropping those cards on open table space is no longer a
                // transfer (it resolves to a cover, or to nothing).
                let dropped = CardPlay.resolve(cards: passCards, target: .table,
                                               isDefender: true, battles: [],
                                               legal: podkidnoyMenu)
                XCTAssertNotEqual(dropped?.type, .pass,
                                  "seed \(salt): the drag would still transfer")
            }
        }

        XCTAssertTrue(sawTransferInTheClassicGame,
                      "no deal in 31 offered a transfer under the classic rules - "
                      + "the podkidnoy assertions above proved nothing")
    }

    /// Deal, start, play the opening attack, and hand back the DEFENDER's legal
    /// menu as the kernel computes it.
    private func defenderMenuAfterOpening(lobbyPayload: Data, env: MessageEnvelope,
                                          joins: [MessageJoin], gameId: UInt64)
        async throws -> [Move] {
        let k = MessageKernel.shared
        let live = try await k.startFromLobby(
            lobbyPayload: lobbyPayload, gameId: gameId, actingSeat: 1,
            parent8: MessageTurnController.firstEight(hex: env.digest),
            joins: joins, sentAt: 0x1234)
        _ = try await k.decode(payload: live, viewer: -1)
        guard let view = await k.residentView(viewer: -1) else { return [] }
        let opener = view.firstAttacker
        let menu = await k.residentLegal(seat: opener)
        // Attack with a single card, so the defender faces exactly one attack -
        // the shape a transfer needs.
        guard let attack = menu.filter({ $0.type == .attack && $0.cards.count == 1 })
                .min(by: { $0.cards[0].v < $1.cards[0].v }) else { return [] }
        try await k.apply(seat: opener, move: attack)
        guard let after = await k.residentView(viewer: -1) else { return [] }
        return await k.residentLegal(seat: after.defender)
    }

    // MARK: whoever changes the rules cannot start

    /// The owner's rule, as the pure decision behind it. Note the FULL lobby:
    /// M9's authorship gate exempts a full one (nobody else could join, so
    /// withholding Start would strand it), and a rules change deliberately does
    /// not take that exemption - in a two-player DM it is the only case that
    /// matters, and letting the changer start there is exactly the thing the
    /// rule forbids.
    func testTheOneWhoChangedTheRulesIsNotOfferedStart() {
        XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: 2, capacity: 8,
                                             iSentTheInvite: false, iChangedTheRules: true),
                       .waiting)
        XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: 2, capacity: 2,
                                             iSentTheInvite: true, iChangedTheRules: true),
                       .waiting, "a full lobby's exemption must not cover a rules change")
        // …and everyone else still can, which is what stops it being a deadlock:
        // the change is a bubble, and whoever opens it may start.
        XCTAssertEqual(LobbyControls.offered(mySeat: 1, joined: 2, capacity: 2,
                                             iSentTheInvite: false, iChangedTheRules: false),
                       .start)
    }

    /// WHO changed them, answered from the chain rather than from a memory of a
    /// tap - so it survives the extension being closed, and so that changing
    /// the box back cancels itself.
    func testRulesChangedReadsTheChainNotATap() {
        XCTAssertTrue(LobbyControls.rulesChanged(baseline: true, current: false, mine: true))
        XCTAssertFalse(LobbyControls.rulesChanged(baseline: true, current: false, mine: false),
                       "somebody else's change is not mine to be punished for")
        XCTAssertFalse(LobbyControls.rulesChanged(baseline: true, current: true, mine: true),
                       "ticking it back is not a change")
        XCTAssertFalse(LobbyControls.rulesChanged(baseline: nil, current: false, mine: true),
                       "a lobby nobody else has touched has no agreement to break")
    }

    // MARK: the rulebook

    /// A podkidnoy table's rulebook does not mention passing ANYWHERE - the
    /// owner's instruction, and the reason the defending section has a second
    /// text rather than an "(if allowed)" aside.
    func testTheNoPassRulesTextNeverMentionsPassing() {
        let words = ["pass", "перевод", "넘기", "페레보"]
        let was = FStrings.override
        defer { FStrings.override = was }
        for lang in [AppLanguage.en, .ru, .ko] {
            FStrings.override = lang
            let body = FStrings.t("ios.rules.defend.b.nopass").lowercased()
            XCTAssertFalse(body.isEmpty, "\(lang): the no-pass defending text is missing")
            XCTAssertNotEqual(body, "ios.rules.defend.b.nopass",
                              "\(lang): the key is not translated")
            for w in words {
                XCTAssertFalse(body.contains(w), "\(lang): the no-pass text mentions \(w)")
            }
            // …and the ordinary text DOES, or the pair proves nothing.
            XCTAssertTrue(words.contains { FStrings.t("ios.rules.defend.b").lowercased().contains($0) },
                          "\(lang): the classic defending text lost its transfer")
        }
    }
}
