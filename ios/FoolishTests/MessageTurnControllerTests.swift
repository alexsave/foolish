// The LOCAL turn loop, end to end through the kernel — no Messages harness (that
// is the live-only part). Adopt a real mid-game bubble, let a seat play a legal
// move, and prove the sealed chain is one the same rules accept as newer than its
// parent (Rule P), then that undo rebuilds the parent exactly. If this passes,
// the only thing left untested is Apple's insert/send plumbing.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageTurnControllerTests: XCTestCase {

    // 2p, turn 7, round 1 — sealed by the native kernel (the §8.2 gate fixture).
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

    func testApplyingALegalMoveSealsAChainThatBeatsItsParent() async throws {
        let k = MessageKernel.shared
        let parentBytes = bytes(fixtureHex)

        // Adopt the parent, then find a seat with a legal move to make.
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)
        XCTAssertEqual(parent.nPlayers, 2)

        var chosen: (seat: Int, move: Move)?
        for seat in 0..<parent.nPlayers {
            let menu = await k.residentLegal(seat: seat)
            if let atk = menu.first(where: { $0.type == .attack }) { chosen = (seat, atk); break }
            if chosen == nil, let any = menu.first(where: { $0.type != .wait }) { chosen = (seat, any) }
        }
        guard let (seat, move) = chosen else {
            return XCTFail("no seat had a legal move in the fixture")
        }

        let c = MessageTurnController(parentPayload: parentBytes, parent: parent, mySeat: seat)
        await c.refresh()
        let beforeHand = c.view?.me?.handCount ?? -1
        XCTAssertGreaterThanOrEqual(beforeHand, 0, "my seat's hand is visible")
        XCTAssertFalse(c.canSend, "nothing staged yet")

        // Play it.
        await c.apply(move)
        XCTAssertEqual(c.pending.count, 1, "one action staged")
        XCTAssertTrue(c.canSend, "Send move enables after one action")
        if move.type == .attack {
            XCTAssertEqual(c.view?.me?.handCount, beforeHand - move.cards.count,
                           "an attack leaves my hand for the table")
        }

        // Seal, and prove the sealed chain is strictly newer than its parent
        // under the SAME rule that resolves races between devices (§7.2).
        let sealed = try await c.stagedPayload()
        let pref = try await k.preferred(parentBytes, sealed)
        XCTAssertGreaterThan(pref, 0, "Rule P ranks the chain-with-my-move above the parent")

        let env = try await MessageEnvelope.decode(payload: sealed, viewer: -1)
        XCTAssertEqual(env.nPlayers, parent.nPlayers)
        XCTAssertEqual(env.joins, parent.joins, "identities survive the seal")
        XCTAssertEqual(env.lastActorSeat, seat, "the bubble records who moved")

        // Undo rebuilds the parent — state is derived, so there is nothing to
        // unwind but the replay (§10).
        await c.undo()
        XCTAssertTrue(c.pending.isEmpty)
        XCTAssertFalse(c.canSend)
        await c.refresh()
        XCTAssertEqual(c.view?.me?.handCount, beforeHand, "undo restored my hand")
    }

    /// parent8 is the first 8 bytes of the parent digest, zero-padded — the exact
    /// tag the next chain points back with (§7.4).
    func testParent8IsFirstEightDigestBytes() {
        let d = MessageTurnController.firstEight(hex: "aabbccddeeff00112233")
        XCTAssertEqual([UInt8](d), [0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11])
        // Short/odd digests never overrun — they zero-pad to 8.
        XCTAssertEqual([UInt8](MessageTurnController.firstEight(hex: "aabb")),
                       [0xaa, 0xbb, 0, 0, 0, 0, 0, 0])
    }
}
