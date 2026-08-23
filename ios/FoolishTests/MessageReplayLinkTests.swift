// The result screen's Replay Link (owner: "in the end screen, have a link that
// when pressed goes to the replay screen in foolish cards website on safari").
//
// The link's whole job is to still work LATER - after the extension has been
// torn down, in a browser, with no chat around it. So what is worth pinning is
// not that a URL appears, but that the URL names THIS finished game and cannot
// be quietly re-pointed at another one. `MessageKernel` holds a single resident
// game that every decode overwrites (a bubble snapshot, a Rule-P comparison,
// the next chain that arrives), so a code read at the moment of the tap is a
// code for whatever the engine happened to be holding by then. It is captured
// when the game ends instead, and the last test here is the one that says so.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageReplayLinkTests: XCTestCase {

    // The §8.2 gate fixture: 2p, turn 7, round 1 - a game still in progress.
    private let fixtureHex =
        "f7020002efcdab89674523010700000200010000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e31070003a9cc795118a16a9edd28d516"

    private func bytes(_ hex: String) -> Data {
        var d = Data(); var i = hex.startIndex
        while i < hex.endIndex {
            let j = hex.index(i, offsetBy: 2)
            d.append(UInt8(hex[i..<j], radix: 16)!); i = j
        }
        return d
    }

    /// Drive the resident game to game over the way a real table does - lowest
    /// eligible seat, first legal move - and hand back the controller watching
    /// it. The controller reads the resident game, so a finish reached through
    /// the kernel and a finish reached through `c.apply` publish identically;
    /// this just gets there without needing both seats to be mine.
    private func finishedController(seats: Int = 2) async throws -> MessageTurnController {
        let k = MessageKernel.shared
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)
        let c = MessageTurnController(parentPayload: parentBytes, parent: parent, mySeat: 0)

        for _ in 0..<2000 {
            if let v = await k.residentView(viewer: -1), v.isOver { break }
            var applied = false
            for seat in 0..<seats {
                let legal = await k.residentLegal(seat: seat)
                if let m = legal.first(where: { $0.type != .wait }) {
                    try await k.apply(seat: seat, move: m)
                    applied = true
                    break
                }
            }
            if !applied { break }
        }
        await c.refresh()
        return c
    }

    /// A finished game offers a link, and it is the FUNNEL shape - a bare code
    /// on the domain, which the site classifies as a self-contained replay
    /// payload. Not the `/m/1<base32>` bubble link: that one is a move to be
    /// adopted, and it lands on the play page, not the replay page.
    func testAFinishedGameOffersTheBareFunnelLink() async throws {
        let c = try await finishedController()
        XCTAssertEqual(c.isOver, true, "the driven game reached game over")
        let code = try XCTUnwrap(c.replayCode, "a finished game published no replay code")
        XCTAssertFalse(code.isEmpty)

        let url = try XCTUnwrap(c.replayURL)
        XCTAssertEqual(url.absoluteString, "https://foolish.cards/" + code)
        XCTAssertFalse(url.path.hasPrefix("/m/"),
                       "that is the bubble's payload link, which opens the game, not the replay")
    }

    /// …and the link really is this game: the code decodes to a COMPLETE replay
    /// of the right size. A link that resolves to a half-game, or to a game with
    /// the wrong player count, is a broken page nobody would report as a bug in
    /// the extension.
    func testTheLinkResolvesToThisFinishedGame() async throws {
        let c = try await finishedController()
        let code = try XCTUnwrap(c.replayCode)
        let decoded = try await EngineC().replayDecode(code: code)
        XCTAssertEqual(decoded.nPlayers, 2)
        XCTAssertTrue(decoded.isComplete, "the replay behind the link does not know its fool")
    }

    /// NO LINK WHILE THE GAME IS RUNNING. There is nothing to watch back yet,
    /// and the result screen it lives on does not exist - but the code is read
    /// on every publish, so this is what keeps that read off every move as well
    /// as keeping a half-game's link off the screen.
    func testAGameInProgressPublishesNoLink() async throws {
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)
        let c = MessageTurnController(parentPayload: parentBytes, parent: parent, mySeat: 0)
        await c.refresh()
        XCTAssertEqual(c.isOver, false, "the fixture is a game in progress")
        XCTAssertNil(c.replayCode)
        XCTAssertNil(c.replayURL)
    }

    /// THE ONE THAT MATTERS. Once the link is on screen the engine moves on -
    /// this test re-points it at an unrelated, unfinished chain, exactly as a
    /// bubble snapshot or an arriving message does. The controller's link must
    /// not follow it.
    ///
    /// The assertion is made against the engine's own live answer rather than
    /// against a remembered string, so it cannot pass by coincidence: after the
    /// re-point the kernel's `residentReplayCode()` describes a DIFFERENT,
    /// unfinished game, and the two answers visibly disagree. Read the code on
    /// tap instead of at game over and the link would be that second one.
    func testTheLinkSurvivesTheEngineMovingOn() async throws {
        let c = try await finishedController()
        let before = try XCTUnwrap(c.replayURL)

        // Somebody else's chain arrives and is adopted - the resident game is
        // now a 2p board at turn 7 with no fool.
        _ = try await MessageEnvelope.decode(payload: bytes(fixtureHex), viewer: 0)
        let live = await MessageKernel.shared.residentReplayCode()

        XCTAssertEqual(c.replayURL, before, "the link followed the engine to another game")
        if let live {
            XCTAssertNotEqual("https://foolish.cards/" + live, before.absoluteString,
                              "the re-point did not actually change the engine's answer, so this test proves nothing")
            let nowDecodes = try? await EngineC().replayDecode(code: live)
            XCTAssertEqual(nowDecodes?.isComplete, false,
                           "the engine is holding an unfinished game, which is the point")
        }
    }

    /// The link is named in every language the extension speaks. A missing key
    /// renders as the key itself, which on the results screen would read as
    /// "ios.msg.replaylink" under the ranking.
    func testTheLinkIsNamedInEveryLanguage() {
        defer { FStrings.override = .en }
        for lang in AppLanguage.allCases {
            FStrings.override = lang
            let s = FStrings.t("ios.msg.replaylink")
            XCTAssertFalse(s.contains("ios.msg"), "\(lang) has no name for the replay link")
            XCTAssertFalse(s.isEmpty)
        }
    }
}

