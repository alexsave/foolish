// The iMessage concurrency model, wired — the Swift twin of
// e2e/msg_concurrency.test.ts. That suite proves Rule P / Rule R in the kernel
// through wasm; this one proves the PHONE's glue over the same primitives:
// Rule P's stale verdict, driven exactly the way MessagesRootView.load()
// drives it. (ROUND 9: the durable pending ledger, the Swift rebase binding
// and the pre-staged replay are removed - owner call - so their legs went
// too.) If the wasm suite and this one disagree, a phone and a browser would
// resolve a race differently — the one thing §7 forbids.
//
// No Messages harness here (that is B4, live-only): every leg is the kernel and
// the store, driven exactly the way MessagesRootView.load() drives them.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageConcurrencyTests: XCTestCase {

    // The §14 fixtures, shared with the C and wasm suites (a diff is a blocker).
    private let p2 = "f7020002efcdab89674523010800000200020000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e310800f72719e90cb7ee031bd6af74a3a23a"
    private let p4 = "f7020002efcdab89674523010700000400010000000000000000449bbad52d5dfb1bdb68d87a09fe591b9419f9f39b0ec35e9f2b75c5a359a138040004416e6e300104416e6e310204416e6e320304416e6e33070001c32dd6c13bd1e53963f945fef906649a"

    private func bytes(_ hex: String) -> Data {
        var d = Data(); var i = hex.startIndex
        while i < hex.endIndex { let j = hex.index(i, offsetBy: 2); d.append(UInt8(hex[i..<j], radix: 16)!); i = j }
        return d
    }

    private func freshStore() -> MessageGameStore {
        let d = UserDefaults(suiteName: "fmsg.test.\(UUID().uuidString)")!
        return MessageGameStore(defaults: d)
    }

    /// Adopt `parent`, find a seat that can play `type`, and return (seat, move).
    private func seatWith(_ type: MoveType, on parent: Data) async throws -> (Int, Move)? {
        let env = try await MessageEnvelope.decode(payload: parent, viewer: -1)
        let k = MessageKernel.shared
        for s in 0..<env.nPlayers {
            if let m = await k.residentLegal(seat: s).first(where: { $0.type == type }) { return (s, m) }
        }
        return nil
    }

    // ROUND 9 (owner): the two Rule-R-over-awire tests that lived here went
    // with the Swift rebase binding (the iOS pending ledger is removed, so no
    // Swift code calls fio_msg_rebase_awire any more). The kernel entry's
    // verdicts stay covered by the C smoke and the wasm e2e suite.

    // MARK: - Rule P: the stale verdict MessagesRootView.load() gates on

    /// §7.5 pickup ∥ throw-in: the defender's pickup closes the bout, so its chain
    /// has the higher round and WINS Rule P everywhere — in both delivery orders.
    /// This is the exact comparison load() runs to decide "adopt" vs "stale".
    func testPickupBeatsThrowInInBothOrders() async throws {
        let k = MessageKernel.shared
        let parent = bytes(p4)
        guard let (pickSeat, pickMove) = try await seatWith(.pickup, on: parent),
              let (atkSeat, atkMove) = try await seatWith(.attack, on: parent) else {
            throw XCTSkip("the 4p fixture cannot pose the pickup∥throw-in race")
        }
        let parentEnv = try await MessageEnvelope.decode(payload: parent, viewer: -1)

        let D = try await playOn(parent, env: parentEnv, seat: pickSeat, move: pickMove)  // pickup → round+1
        let A = try await playOn(parent, env: parentEnv, seat: atkSeat, move: atkMove)    // throw-in → same round

        let dEnv = try await MessageEnvelope.decode(payload: D, viewer: -1)
        XCTAssertGreaterThan(dEnv.round, parentEnv.round, "pickup closes the bout")

        let da = try await k.preferred(D, A)
        let ad = try await k.preferred(A, D)
        XCTAssertLessThan(da, 0, "D (pickup) must win")
        XCTAssertGreaterThan(ad, 0, "D must win with the arguments swapped — delivery order is not an input")
    }

    /// Adopt `parent`, play one move, seal the child chain — the send path.
    private func playOn(_ parent: Data, env: MessageEnvelope, seat: Int, move: Move) async throws -> Data {
        let k = MessageKernel.shared
        _ = try await k.decode(payload: parent, viewer: -1)
        try await k.apply(seat: seat, move: move)
        return try await k.seal(phase: 2, lastActorSeat: seat, gameId: UInt64(env.gameId)!,
                                parent8: MessageTurnController.firstEight(hex: env.digest),
                                joins: env.joins)
    }

    // ROUND 9 (owner): the durable pending ledger and its Rule R rebase are
    // removed - the two ledger tests that lived here went with them. Staged
    // moves exist only in the controller's memory now; the concurrency rules
    // (Rule P and the kernel-side Rule R) remain covered by the FMSG e2e suite
    // (e2e/msg_concurrency.test.ts).
}
