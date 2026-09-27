// MessageChainSummaryTests - the sentence a SEEDED chain bubble displays.
//
// WHY THIS FILE EXISTS, and why it is not covered by MessageCaptionActorTests
// next door. That file walks a chain the Swift side PLAYS
// (`MessageTurnController.stagedPayload`), and it passes. The rig photographs a
// chain the C side SEALS (`msg_wire_test --chain`), and a four-bubble shoot of
// one came out reading
//
//     SEATONE attacks with 8 of C
//     SEATZERO covers 10 of C with 8 of S
//     SEATONE attacks with 8 of C
//
// over entries whose real moves were `cover 8S`, `attack 9C`, `cover 7S`. Two
// bubbles of a transcript cannot display the same sentence - no two consecutive
// moves read alike - so something between those bytes and that line is wrong,
// and no screenshot can say which. A C probe over the same payloads matched,
// which narrows it to the Swift read; this file IS that read, with the moves
// the generator printed written down beside them.
//
// The payloads are the literal output of
//     FOOLISH_NAMES="SEATZERO,SEATONE" c/build/msg_wire_test --chain 2 4 14
// which is deterministic (a fixed seed sweep). They are pinned here rather than
// regenerated because the point is a FIXED artifact the product must describe
// correctly - the same reason a replay fixture is pinned. If the generator's
// search ever moves, the expectations below move with it and the mismatch is
// the failure, which is what you want.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageChainSummaryTests: XCTestCase {

    /// `msg_wire_test --chain 2 4 14`, verbatim.
    private static let wire = [
        "f7050002efcdab89674523010f000002010200000000000000006d45451e3267773ccd7868f6437bf9865c4c2013c141014b1dcd16093d1848f0058501020008534541545a45524f0107534541544f4e450f002dfca18097b90625b23522c0d4e976e6955a",
        "f7050002efcdab896745230110000102010200000000000000006d45451e3267773ccd7868f6437bf9865c4c2013c141014b1dcd16093d1848f0058501020008534541545a45524f0107534541544f4e4510003ce660f1d3f846db8cecf42e87c976e7055a",
        "f7050002efcdab896745230111000002010200000000000000006d45451e3267773ccd7868f6437bf9865c4c2013c141014b1dcd16093d1848f0058501020008534541545a45524f0107534541544f4e4511000317a4059e5c13a9aa6a1c0a2dc4a976e7755a",
        "f7050002efcdab896745230112000102010200000000000000006d45451e3267773ccd7868f6437bf9865c4c2013c141014b1dcd16093d1848f0058501020008534541545a45524f0107534541544f4e4512000441e6fa771104b7df807866bfbe2976e7e55a",
    ]

    /// What the generator said each entry IS - `chain[i]: actor=seat N <move>`.
    private struct Expected { let seat: Int; let verb: String; let card: String }
    /// Copied from the generator's own stderr, NOT typed from a card table:
    ///
    ///     chain[0]: actor=seat 0 cover 9S
    ///     chain[1]: actor=seat 1 attack 10C
    ///     chain[2]: actor=seat 0 cover 8S
    ///     chain[3]: actor=seat 1 attack 8C
    ///
    /// The first version of this list was typed from a hand-rolled rank table
    /// that put the ace at 1, so every card came out one rank low and all four
    /// correct summaries failed. Value 1 is a TWO and the ace is 13 - the table
    /// in c/src/main_analyse.c - and the way not to get it wrong again is to
    /// read the generator rather than the deck.
    private static let truth = [
        Expected(seat: 0, verb: "covers", card: "9 of \u{2660}"),
        Expected(seat: 1, verb: "attacks", card: "10 of \u{2663}"),
        Expected(seat: 0, verb: "covers", card: "8 of \u{2660}"),
        Expected(seat: 1, verb: "attacks", card: "8 of \u{2663}"),
    ]

    private func bytes(_ s: String) -> Data {
        var out = Data(); var i = s.startIndex
        while i < s.endIndex {
            let j = s.index(i, offsetBy: 2)
            out.append(UInt8(s[i..<j], radix: 16)!); i = j
        }
        return out
    }

    /// THE ENVELOPE agrees with the generator about who acted. If this fails the
    /// bytes are not what the generator printed and nothing below means anything.
    func testEachEntryNamesItsOwnActor() async throws {
        for (i, hex) in Self.wire.enumerated() {
            let env = try await MessageEnvelope.decode(payload: bytes(hex), viewer: -1)
            XCTAssertEqual(env.lastActorSeat, Self.truth[i].seat,
                           "entry \(i): the envelope disagrees with the generator")
        }
    }

    /// THE REPORT: the staged bubble's summary, read exactly as
    /// `MessagesViewController.stage` reads it, one fresh appex per entry - which
    /// is what the rig arranges by killing the extension between sends.
    func testStagedSummaryDescribesItsOwnMove() async throws {
        var wrong: [String] = []
        for (i, hex) in Self.wire.enumerated() {
            let (_, _, summary) = await MessageSummary.forStagedBubble(payload: bytes(hex))
            let want = Self.truth[i]
            let who = want.seat == 0 ? "SEATZERO" : "SEATONE"
            let other = want.seat == 0 ? "SEATONE" : "SEATZERO"
            if !summary.contains(who) || summary.contains(other) {
                wrong.append("entry \(i): \"\(summary)\" should name \(who)")
            }
            if !summary.contains(want.verb) || !summary.contains(want.card) {
                wrong.append("entry \(i): \"\(summary)\" should say \(want.verb) … \(want.card)")
            }
        }
        XCTAssertEqual(wrong, [], "staged summaries that describe the wrong move")
    }

    /// AND NO TWO ARE ALIKE. The tell that started this: consecutive moves
    /// cannot read the same, so a transcript that shows one sentence twice is
    /// reporting a state it was not given, whatever the sentences say.
    func testNoTwoBubblesReadAlike() async throws {
        var seen: [String] = []
        for hex in Self.wire {
            let (_, _, summary) = await MessageSummary.forStagedBubble(payload: bytes(hex))
            seen.append(summary)
        }
        XCTAssertEqual(Set(seen).count, seen.count,
                       "two bubbles of one chain read alike: \(seen)")
    }
}
