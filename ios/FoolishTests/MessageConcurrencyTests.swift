// The iMessage concurrency model, wired — the Swift twin of
// e2e/msg_concurrency.test.ts. That suite proves Rule P / Rule R in the kernel
// through wasm; this one proves the PHONE's glue over the same primitives:
// the awire rebase entry (fio_msg_rebase_awire), the durable pending ledger
// (MessageGameStore), the controller's pre-staged replay, and Rule P's stale
// verdict. If the wasm suite and this one disagree, a phone and a browser would
// resolve a race differently — the one thing §7 forbids.
//
// No Messages harness here (that is B4, live-only): every leg is the kernel and
// the store, driven exactly the way MessagesRootView.load() drives them.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageConcurrencyTests: XCTestCase {

    // The §14 fixtures, shared with the C and wasm suites (a diff is a blocker).
    private let p2 = "f7020002efcdab89674523010700000200010000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e31070003a9cc795118a16a9edd28d516"
    private let p4 = "f7020002efcdab89674523010500000400010000000000000000449bbad52d5dfb1bdb68d87a09fe591b9419f9f39b0ec35e9f2b75c5a359a138040004416e6e300104416e6e310204416e6e320304416e6e33050003b7ddc3ef88a264acb5183fbe413a46"

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

    // MARK: - Rule R over the awire entry (the JSON-free path Swift actually calls)

    /// §7.4 round-boundary guard: a move composed against a round the chain has
    /// since closed is DISCARDED, never re-applied as an opening of the next
    /// round. Same verdict the C smoke and wasm suite reach on the same action.
    func testRebaseAwireRoundGuardDiscards() async throws {
        let k = MessageKernel.shared
        let env = try await MessageEnvelope.decode(payload: bytes(p4), viewer: -1)
        XCTAssertGreaterThan(env.round, 0, "the fixture must sit past round 0 to pose the guard")
        // "good" as awire, composed against a round strictly older than adopted.
        let verdict = try await k.rebase(pendingRound: env.round - 1, seat: 0,
                                         awire: MoveWire.encodeAction(Move(type: .good)))
        XCTAssertEqual(verdict, .discardedRoundEnded, "the round guard must fire")
    }

    /// A move that IS legal on the adopted state, composed against that same
    /// round, RE-APPLIES — the "your move was re-applied, send to confirm" path.
    func testRebaseAwireReappliesALegalMove() async throws {
        let k = MessageKernel.shared
        // Re-adopt cleanly first so g_msg_round is the fixture's round.
        let env = try await MessageEnvelope.decode(payload: bytes(p4), viewer: -1)
        guard let (seat, move) = try await seatWith(.attack, on: bytes(p4)) else {
            throw XCTSkip("the 4p fixture offered no attack to rebase")
        }
        // seatWith re-adopted the fixture; rebase against the adopted round.
        let verdict = try await k.rebase(pendingRound: env.round, seat: seat,
                                         awire: MoveWire.encodeAction(move))
        XCTAssertEqual(verdict, .reapplied, "a currently-legal move must re-apply")
    }

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

    // MARK: - The durable pending ledger

    /// A staged move is mirrored into the ledger tagged with the bout it was
    /// composed against; undo shrinks it; a cleared game vanishes. This is what a
    /// bubble-arriving-mid-staging reads back to rebase (§17.15).
    func testStagingWritesTheLedgerAndUndoClearsIt() async throws {
        let store = freshStore()
        let seed = Data((0..<32).map { UInt8(($0 &* 7 &+ 3) | 1) })
        let c = MessageTurnController(genesisSeed: seed, players: 2, gameId: 555, myNickname: "A", store: store)
        await c.begin()
        XCTAssertTrue(store.pending(gameId: "555").isEmpty, "nothing staged yet")

        guard let atk = c.legal.first(where: { $0.type != .wait }) else {
            return XCTFail("the first attacker has no legal move")
        }
        await c.apply(atk)
        let led = store.pending(gameId: "555")
        XCTAssertEqual(led.count, 1, "the staged move is in the ledger")
        XCTAssertEqual(led.first?.seat, 0)
        XCTAssertEqual(led.first?.round, 0, "a genesis game is round 0 — the guard key")

        await c.undo()
        XCTAssertTrue(store.pending(gameId: "555").isEmpty, "undo removed it from the ledger too")
    }

    /// Rule R survivors are handed to the next controller as `preStaged`: it
    /// replays them on top of the adopted chain, so they show as pending (Send is
    /// live) and land back in the ledger. This is the "re-applied — send to
    /// confirm" state a rebase leaves behind.
    func testPreStagedMovesAreReplayedAndReLedgered() async throws {
        let store = freshStore()
        let parent = bytes(p4)
        guard let (seat, move) = try await seatWith(.attack, on: parent) else {
            throw XCTSkip("no attack available to pre-stage")
        }
        let env = try await MessageEnvelope.decode(payload: parent, viewer: -1)
        let c = MessageTurnController(parentPayload: parent, parent: env, mySeat: seat,
                                      preStaged: [move], store: store)
        await c.begin()
        XCTAssertEqual(c.pending.count, 1, "the survivor was replayed as a staged move")
        XCTAssertTrue(c.canSend, "a rebased survivor leaves Send enabled")
        XCTAssertEqual(store.pending(gameId: env.gameId).count, 1, "and it is durable in the ledger")
        XCTAssertEqual(store.pending(gameId: env.gameId).first?.round, env.round,
                       "re-tagged to the adopted round")
    }
}
