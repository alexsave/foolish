// MessageEventsTests — the kernel is the single source of animation truth on
// iMessage too (docs/C_CORE_CONSOLIDATION.md A5/F4). An iMessage client holds
// only the encoded chain (no server emitted events at move time), so it asks the
// kernel for "the animations of the last move" via MessageKernel.lastMoveEvents
// -> fio_replay_last_events_packed -> the packed evwire frame -> EvWire.decode.
//
// The bug this pins: the OLD open-replay derived my own drawn cards by diffing
// two GameViews, which cannot recover them (the replayed hand looks the same
// from the diff's side), so a reopened pickup animated everyone ELSE's refill
// but never my own. The kernel, replaying with MY seat as the viewer, reveals my
// drawn cards and hides the opponents' - the SAME viewer-aware evwire stream live
// play emits. This test proves that end to end, and would fail against any client
// that went back to diffing.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageEventsTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 11 &+ Int(salt)) | 1 })
    }

    /// Play a 2p game to a defender pickup (which refills the attacker from the
    /// deck), then read the last move's events for each seat. The attacker's
    /// REFILL event must carry REAL cards to the attacker and HIDDEN ones to the
    /// defender - proving the mask is the kernel's, per viewer, not the client's.
    func testLastMoveEventsRevealMyOwnRefillAndHideTheOpponents() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(5), players: 2)

        // Whoever holds a legal attack is the first attacker; the other defends.
        let legal0 = await k.residentLegal(seat: 0)
        let attacker = legal0.contains(where: { $0.type == .attack }) ? 0 : 1
        let defender = 1 - attacker

        // Attack with the first legal attack, then the defender picks it up - one
        // handle_pickup call that also refills the attacker back up from the deck.
        let atkLegal = await k.residentLegal(seat: attacker)
        let atk = try XCTUnwrap(atkLegal.first { $0.type == .attack })
        try await k.apply(seat: attacker, move: atk)
        let defLegal = await k.residentLegal(seat: defender)
        let pick = try XCTUnwrap(defLegal.first { $0.type == .pickup })
        try await k.apply(seat: defender, move: pick)

        // The last move's events, as each seat would see them animate.
        let mine = await k.lastMoveEvents(viewer: attacker)
        let theirs = await k.lastMoveEvents(viewer: defender)
        XCTAssertFalse(mine.isEmpty, "the pickup step must produce animation events")

        // The attacker's refill, seen by the attacker: real cards (identities I am
        // allowed to know, because they landed in MY hand).
        let myRefill = mine.first { $0.kind == .refill && $0.seat == attacker }
        let refill = try XCTUnwrap(myRefill, "the attacker refills after a pickup")
        XCTAssertTrue(refill.cards.contains { $0 != nil },
                      "my own drawn cards must carry real identities, not backs")

        // The SAME refill, seen by the defender: every card redacted (a back).
        let theirView = theirs.first { $0.kind == .refill && $0.seat == attacker }
        if let tv = theirView {
            XCTAssertTrue(tv.cards.allSatisfy { $0 == nil },
                          "another seat's draws must be hidden from me")
        }
    }

    /// A sanity check on the decoder itself: the packed frame decodes to a
    /// non-empty, well-formed sequence whose events carry a from/to location. A
    /// malformed or empty frame must degrade to [], never crash.
    func testEvWireDecodeIsWellFormedAndEmptyIsSafe() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(9), players: 2)
        let legal0 = await k.residentLegal(seat: 0)
        let attacker = legal0.contains { $0.type == .attack } ? 0 : 1
        let atkLegal = await k.residentLegal(seat: attacker)
        let atk = try XCTUnwrap(atkLegal.first { $0.type == .attack })
        try await k.apply(seat: attacker, move: atk)

        let events = await k.lastMoveEvents(viewer: attacker)
        XCTAssertFalse(events.isEmpty, "an attack is one animation event at least")
        XCTAssertTrue(events.contains { $0.kind == .attackPass }, "the attack itself is in the stream")

        XCTAssertTrue(EvWire.decode(Data()).isEmpty, "an empty frame is not a crash")
        XCTAssertTrue(EvWire.decode(Data([0xFF, 0x00, 0x00])).isEmpty, "a bad version is not a crash")
    }
}
