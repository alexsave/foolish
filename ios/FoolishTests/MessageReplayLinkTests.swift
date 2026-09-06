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
        "f7020002efcdab89674523010800000200020000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e310800f72719e90cb7ee031bd6af74a3a23a"

    private func bytes(_ hex: String) -> Data {
        var d = Data(); var i = hex.startIndex
        while i < hex.endIndex {
            let j = hex.index(i, offsetBy: 2)
            d.append(UInt8(hex[i..<j], radix: 16)!); i = j
        }
        return d
    }

    /// Drive the fixture to game over the way a real table does - lowest
    /// eligible seat, first legal move - SEAL the finished chain, and hand back
    /// a controller that opened it. Two seats have to move to finish a game and
    /// only one of them is mine, so the far end is played on the kernel and the
    /// result reaches the controller the way it reaches a real device: as a
    /// bubble.
    ///
    /// ROUND 22: it used to drive the kernel and then call `c.refresh()`,
    /// leaning on the controller mirroring whatever game was resident. That is
    /// the assumption this whole file is about (see the header) and it is now
    /// gone - a board is rebuilt from its own base and its own staged moves
    /// before it is read, so moves made behind its back are not its moves. The
    /// seal is what makes them its moves.
    private func finishedController(seats: Int = 2) async throws -> MessageTurnController {
        let k = MessageKernel.shared
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)
        var last = parent.lastActorSeat
        for _ in 0..<2000 {
            if let v = await k.residentView(viewer: -1), v.isOver { break }
            var applied = false
            for seat in 0..<seats {
                let legal = await k.residentLegal(seat: seat)
                if let m = legal.first(where: { $0.type != .wait }) {
                    try await k.apply(seat: seat, move: m)
                    applied = true
                    last = seat
                    break
                }
            }
            if !applied { break }
        }
        let payload = try await k.seal(phase: 3, lastActorSeat: last,
                                       gameId: UInt64(parent.gameId) ?? 0,
                                       parent8: Data(repeating: 0, count: 8),
                                       joins: parent.joins)
        let env = try await MessageEnvelope.decode(payload: payload, viewer: 0)
        let c = MessageTurnController(parentPayload: payload, parent: env, mySeat: 0)
        await c.begin()
        return c
    }

    /// A finished game offers a link, and it is the FUNNEL shape - a bare code
    /// on the domain, which the site classifies as a self-contained replay
    /// payload. Not the `/m/1<base32>` bubble link: that one is a move to be
    /// adopted, and it lands on the play page, not the replay page.
    ///
    /// The code the kernel gives is the MOVES HALF of that path segment. Since
    /// 1.0(38) the nicknames ride behind it after a dash (see the next test), so
    /// what is pinned here is that the kernel's half arrives untouched - the
    /// game a link resolves to must not depend on who was sitting at the table.
    func testAFinishedGameOffersTheBareFunnelLink() async throws {
        let c = try await finishedController()
        XCTAssertEqual(c.isOver, true, "the driven game reached game over")
        let code = try XCTUnwrap(c.replayCode, "a finished game published no replay code")
        XCTAssertFalse(code.isEmpty)

        let url = try XCTUnwrap(c.replayURL)
        XCTAssertTrue(url.absoluteString.hasPrefix("https://foolish.cards/" + code),
                      "the link is not this game's code: \(url.absoluteString)")
        XCTAssertFalse(url.path.hasPrefix("/m/"),
                       "that is the bubble's payload link, which opens the game, not the replay")
    }

    /// THE NICKNAMES TRAVEL WITH THE LINK (owner, 1.0(38): "Replay code in
    /// iMessage does not save nicknames! It should").
    ///
    /// The site has read a names channel off the end of a replay code since the
    /// channel existed - `<moves>-<extras>`, decoded by ReplayScreen through
    /// server/api/common/replay/extras.ts - and the website's own finished games
    /// have written it all along. The phone was the one producer that did not,
    /// so a game between friends replayed as "P1" beating "P2".
    ///
    /// Asserted against the WIRE BYTES rather than by decoding with a Swift
    /// decoder, because there is no Swift decoder and writing one here would
    /// only prove this file agrees with itself. The layout is
    /// c/src/replay_extras.h's: version 2, flags bit0 = names, then exactly
    /// one NUL-terminated UTF-8 name per seat.
    ///
    /// THIS IS THE LIVE PATH, end to end: a finished chain, the controller
    /// the extension really runs, its own `replayURL`, the bytes a friend's
    /// browser will read. The format's other half - that these bytes are the
    /// bytes every earlier build wrote, so old links still decode - is
    /// e2e/replay_extras_kernel.test.ts.
    func testTheLinkCarriesTheTableNicknames() async throws {
        let c = try await finishedController()
        let code = try XCTUnwrap(c.replayCode)
        let url = try XCTUnwrap(c.replayURL)

        let segment = String(url.absoluteString.dropFirst("https://foolish.cards/".count))
        let halves = segment.split(separator: "-", maxSplits: 1, omittingEmptySubsequences: false)
        // A `guard` and not an assert: everything below indexes into it, and a
        // test that CRASHES the runner on failure takes the rest of the suite
        // with it instead of reporting one red line.
        guard halves.count == 2 else {
            return XCTFail("the link carries no names segment: \(segment)")
        }
        XCTAssertEqual(String(halves[0]), code, "the names segment disturbed the moves half")

        let blob = try XCTUnwrap(Base32.decode(String(halves[1])), "the names segment is not base32")
        // The fixture's chain names seat 0 "Ann0" and seat 1 "Ann1" (its joins).
        let expected: [UInt8] = [2, 1]
            + Array("Ann0".utf8) + [0]
            + Array("Ann1".utf8) + [0]
        XCTAssertEqual([UInt8](blob), expected)
    }

    /// EVERY SEAT GETS A SLOT, named or not. The reader takes the player count
    /// from the decoded MOVES and then reads that many NUL-terminated strings,
    /// so a roster short by one seat does not just lose a name - it walks the
    /// parse off the end of the blob and throws away the whole thing. A seat
    /// nobody introduced is "", which the frame builder renders as "P<n>".
    func testAnUnnamedSeatStillGetsItsSlot() {
        XCTAssertEqual(ReplayExtras.seatNames([0: "Ann"], count: 3), ["Ann", "", ""])
        XCTAssertEqual(ReplayExtras.seatNames([1: "Bo", 0: "Al"], count: 2), ["Al", "Bo"])
        // No view means no seat count (see `replayURL`): nothing to pad to.
        XCTAssertEqual(ReplayExtras.seatNames([0: "Ann"], count: 0), [])
    }

    /// A TABLE WHERE NOBODY IS NAMED EMITS THE OLD BARE CODE. An all-empty
    /// roster decodes to the same "P1"/"P2" the reader already falls back to, so
    /// the segment would be bytes for nothing - and a link byte-identical to what
    /// earlier builds produced is one less thing that can behave differently.
    func testAnAnonymousTableEmitsTheCodeUnchanged() {
        let bare = "https://foolish.cards/MOVES"
        XCTAssertEqual(ReplayExtras.link(moves: "MOVES", names: []), bare)
        XCTAssertEqual(ReplayExtras.link(moves: "MOVES", names: ["", ""]), bare)
        XCTAssertEqual(ReplayExtras.bare("MOVES"), bare)
        XCTAssertEqual(MessageEnvelope.replayLink(code: "MOVES").absoluteString, bare,
                       "the no-names link is the same string it always was")
        XCTAssertNotEqual(ReplayExtras.link(moves: "MOVES", names: ["", "Bo"]), bare,
                          "one named seat is still worth carrying")
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

    /// A ROSTER OF REAL NAMES, through the real bridge. The seats a chat fills
    /// are not ASCII: Cyrillic, kana, an emoji with a skin tone, and a name that
    /// is over the 48-byte budget. This is the case a byte-wise trim gets wrong
    /// - a thumbs-up with a skin tone is one grapheme and two code points, and
    /// the budget line falls inside one here - so it is asserted on the WIRE
    /// BYTES the browser will read, not on a Swift round trip.
    func testAUnicodeRosterCrossesTheBridgeIntact() {
        let over = "A" + String(repeating: "\u{1F44D}\u{1F3FD}", count: 7)  // 57 bytes
        let names = ["Владимир", "さくら", over, ""]
        let code = ReplayExtras.link(moves: "MOVES", names: names)
        let halves = code.split(separator: "-", maxSplits: 1, omittingEmptySubsequences: false)
        guard halves.count == 2 else { return XCTFail("no names segment: \(code)") }
        XCTAssertEqual(String(halves[0]), "https://foolish.cards/MOVES",
                       "the roster disturbed the link in front of it")
        let blob = try? XCTUnwrap(Base32.decode(String(halves[1])))
        guard let blob else { return XCTFail("the segment is not base32") }

        var want: [UInt8] = [2, 1]
        for name in names {
            // 48 UTF-8 bytes, cut on a code-point boundary. Expressed here as
            // the rule, not as a hand-copied byte string: a fixture would agree
            // with a wrong trim as readily as a right one.
            var kept = Array(name.utf8)
            if kept.count > 48 {
                var k = 48
                while k > 0 && (kept[k] & 0xC0) == 0x80 { k -= 1 }
                kept = Array(kept[0..<k])
            }
            want += kept + [0]
        }
        XCTAssertEqual([UInt8](blob), want)
        // …and the over-budget seat really was trimmed, or this proves nothing.
        XCTAssertLessThan(Array(over.utf8).count, 64)
        XCTAssertGreaterThan(Array(over.utf8).count, 48)
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

