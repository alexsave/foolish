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

    /// Round-3 "double animations": a move animates when you PLAY it, and then
    /// again a moment later, slowly. Sending re-selects your own just-inserted
    /// bubble, which reloads the board from those bytes — and the last move on
    /// that chain is the move you just watched yourself play, so the open-replay
    /// played it a second time.
    ///
    /// The rule that closes it, pinned here: a chain's last move is replayed to
    /// everyone EXCEPT the seat that sealed it. Same bytes, two seats, two
    /// answers — so this fails against any build that decides from the chain
    /// alone (as it did before) and against one that suppresses the replay for
    /// everybody.
    func testAChainsLastMoveIsNotReplayedToTheSeatThatSealedIt() async throws {
        let k = MessageKernel.shared

        // Seat 0 deals and opens; seat 1 replies. `p1` is therefore a chain
        // whose last actor is seat 1. The creator is not always the first
        // attacker (whoever holds the lowest trump is), so deal until seat 0 is.
        var found: MessageTurnController?
        for salt in UInt8(20)...UInt8(60) {
            let c = MessageTurnController(genesisSeed: freshSeed(salt), players: 2,
                                          gameId: 77, myNickname: "A")
            await c.begin()
            if c.legal.contains(where: { $0.type != .wait }) { found = c; break }
        }
        let creator = try XCTUnwrap(found, "no 2p deal made seat 0 the first attacker")
        let open = try XCTUnwrap(creator.legal.first { $0.type != .wait },
                                 "the creator must have an opening move")
        await creator.apply(open)
        let p0 = try await creator.stagedPayload()
        let e0 = try await MessageEnvelope.decode(payload: p0, viewer: -1)

        let joiner = MessageTurnController(parentPayload: p0, parent: e0, mySeat: 1)
        await joiner.begin()
        let reply = try XCTUnwrap(joiner.legal.first { $0.type != .wait },
                                  "the joiner must have a reply")
        await joiner.apply(reply)
        let p1 = try await joiner.stagedPayload()
        let e1 = try await MessageEnvelope.decode(payload: p1, viewer: -1)
        XCTAssertEqual(e1.lastActorSeat, 1, "fixture: seat 1 sealed this chain")

        // Seat 0 opens it — they were not there for seat 1's move, so it plays.
        let receiver = MessageTurnController(parentPayload: p1, parent: e1, mySeat: 0)
        await receiver.begin()
        XCTAssertFalse(receiver.openReplayEvents.isEmpty,
                       "the seat that did NOT move must be shown what happened")

        // Seat 1 reopens their OWN bubble — nothing to catch up on.
        let sender = MessageTurnController(parentPayload: p1, parent: e1, mySeat: 1)
        await sender.begin()
        XCTAssertTrue(sender.openReplayEvents.isEmpty,
                      "a seat must never be shown a replay of the move it just played")
        _ = k
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
