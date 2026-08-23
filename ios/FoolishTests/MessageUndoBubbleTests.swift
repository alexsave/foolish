// The bubble that carries NOTHING (round 16 defect report).
//
// The owner: "somehow, if you stage a move then undo, you can still send a
// message and it will look weird for the other players. Sometimes even play a
// weird undo animation I think? Not sure. Idk if there's a way to unstage this
// kind of message, but it should at least have zero animation when received by
// others."
//
// There is no way to unstage it: Messages offers no API to remove a bubble that
// has been inserted into the input field, so §10 cancels a staged move by
// OVERWRITING it with a re-seal of the board the chain was already in
// (MessageTableView.stageBaseNow). That bubble is sendable and it carries no
// move - but it used to claim a bubble delta of 1, so every recipient opened it
// and replayed the PREVIOUS player's move as though it had just arrived. That
// is the "weird undo animation": a cover flying onto a table it was already on.
//
// The fix is that the wire can now say "nothing" (msg_wire.h's MSG_NEW_NOTHING)
// as distinct from "I do not know" - which is the value that makes a reader
// GUESS, and the guess is the bug. These tests drive the real controller the
// extension runs, seal what it seals, and open the result as the other player.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageUndoBubbleTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 13 &+ Int(salt)) | 1 })
    }

    /// A 2p chain where seat 0 has attacked and seat 1 can cover: the shape the
    /// report comes from (a defender picks a card, changes their mind, sends).
    /// Returns the attacker's sent bubble and its envelope.
    private func attackedChain(gameId: UInt64, sentAt: Int) async throws
        -> (payload: Data, env: MessageEnvelope) {
        for salt in UInt8(1)...UInt8(80) {
            let a = MessageTurnController(genesisSeed: freshSeed(salt), players: 2,
                                          gameId: gameId, myNickname: "A")
            await a.begin()
            guard let atk = a.legal.first(where: { $0.type == .attack }) else { continue }
            await a.apply(atk)
            let p = try await a.stagedPayload(sentAt: sentAt)
            let e = try await MessageEnvelope.decode(payload: p, viewer: -1)
            // The defender must actually have a cover to stage and undo.
            let b = MessageTurnController(parentPayload: p, parent: e, mySeat: 1)
            await b.begin()
            if b.legal.contains(where: { $0.type == .cover }) { return (p, e) }
        }
        throw XCTSkip("no 2p deal in 80 tries opened with a coverable attack")
    }

    /// THE REPORT, end to end. Stage a cover, undo it, send the bubble that
    /// replaces it - and the player who opens that bubble animates nothing at
    /// all. The control in the same test is what makes it mean something: the
    /// identical chain WITHOUT the undo hands that same player a cover to
    /// animate, so an empty stream here is the undo's doing and not a harness
    /// that quietly produced no events.
    func testAStagedMoveThatWasUndoneAnimatesNothingForTheReceiver() async throws {
        let (p0, e0) = try await attackedChain(gameId: 1600, sentAt: MessageKernel.clockNow() - 30)

        // The control: cover and send for real.
        let kept = MessageTurnController(parentPayload: p0, parent: e0, mySeat: 1)
        await kept.begin()
        let cover = try XCTUnwrap(kept.legal.first { $0.type == .cover })
        await kept.apply(cover)
        let pKept = try await kept.stagedPayload(sentAt: MessageKernel.clockNow() - 30)
        let eKept = try await MessageEnvelope.decode(payload: pKept, viewer: -1)
        let sawCover = MessageTurnController(parentPayload: pKept, parent: eKept, mySeat: 0)
        await sawCover.begin()
        XCTAssertEqual(sawCover.openReplayEvents.filter { $0.kind == .cover }.count, 1,
                       "control: a cover that WAS sent must animate for the attacker")

        // …and the report: the same cover, staged and then undone.
        let undone = MessageTurnController(parentPayload: p0, parent: e0, mySeat: 1)
        await undone.begin()
        let same = try XCTUnwrap(undone.legal.first { $0.type == .cover })
        await undone.apply(same)
        await undone.undo()
        XCTAssertFalse(undone.canStage, "fixture: the undo left something staged")
        let pEmpty = try await undone.stagedPayload(sentAt: MessageKernel.clockNow())
        let eEmpty = try await MessageEnvelope.decode(payload: pEmpty, viewer: -1)

        let opened = MessageTurnController(parentPayload: pEmpty, parent: eEmpty, mySeat: 0)
        await opened.begin()
        XCTAssertTrue(opened.openReplayEvents.isEmpty,
                      "an undone move animated \(opened.openReplayEvents.count) events: "
                      + "\(opened.openReplayEvents.map { String(describing: $0.kind) })")
        XCTAssertFalse(opened.replayPending,
                       "the veil went up for a bubble with nothing behind it")
    }

    /// …and it says so ON THE WIRE, rather than the receiver working it out. The
    /// distinction the fix rests on: 0 is "this bubble does not say", whose
    /// fallback is the guess that replays the previous move, so "nothing" needs
    /// a value of its own. `atomsBefore` is where a reader starts, and for this
    /// bubble it is the whole chain - one past the last step, so the suffix is
    /// empty by construction rather than by a special case at read time.
    func testTheEmptyBubbleStatesItsEmptinessRatherThanLeavingItToBeGuessed() async throws {
        let (p0, e0) = try await attackedChain(gameId: 1601, sentAt: MessageKernel.clockNow() - 30)
        let b = MessageTurnController(parentPayload: p0, parent: e0, mySeat: 1)
        await b.begin()
        await b.apply(try XCTUnwrap(b.legal.first { $0.type == .cover }))
        await b.undo()
        let env = try await MessageEnvelope.decode(payload: try await b.stagedPayload(), viewer: -1)

        XCTAssertEqual(env.newAtoms, MessageEnvelope.newAtomsNothing,
                       "the empty bubble claimed a delta of \(env.newAtoms)")
        XCTAssertNotEqual(env.newAtoms, 0,
                          "0 is 'does not say', and the guess it triggers IS the bug")
        XCTAssertEqual(env.atomsBefore, env.turn,
                       "a bubble that added nothing starts where the chain ends")
        XCTAssertEqual(env.turn, e0.turn,
                       "the re-seal moved the chain, so it was not empty after all")
    }

    /// AN UNDO IS NOT AUTOMATICALLY AN EMPTY BUBBLE. Undoing one of two staged
    /// covers leaves a real move to send, and that one must still animate - this
    /// is the boundary a fix that keyed on "the human pressed Undo" would get
    /// wrong. What decides is whether anything is staged, which is why the fact
    /// is read off the resident game (msg_seal_base) and not off the gesture.
    func testAnUndoThatLeavesAMoveStagedStillAnimatesThatMove() async throws {
        for salt in UInt8(1)...UInt8(120) {
            var creator: MessageTurnController?
            for s2 in UInt8(0)...UInt8(40) {
                let c = MessageTurnController(genesisSeed: freshSeed(salt &+ s2), players: 2,
                                              gameId: 1602, myNickname: "A")
                await c.begin()
                if c.legal.contains(where: { $0.type == .attack }) { creator = c; break }
            }
            guard let a = creator, let atk1 = a.legal.first(where: { $0.type == .attack })
            else { continue }
            await a.apply(atk1)
            guard let atk2 = a.legal.first(where: { $0.type == .attack }) else { continue }
            await a.apply(atk2)
            let p0 = try await a.stagedPayload(sentAt: MessageKernel.clockNow() - 30)
            let e0 = try await MessageEnvelope.decode(payload: p0, viewer: -1)

            // Stage both covers, then take one back.
            let b = MessageTurnController(parentPayload: p0, parent: e0, mySeat: 1)
            await b.begin()
            guard let c1 = b.legal.first(where: { $0.type == .cover }) else { continue }
            let firstCover = try XCTUnwrap(c1.cards.first)
            await b.apply(c1)
            guard let c2 = b.legal.first(where: { $0.type == .cover }) else { continue }
            let secondCover = try XCTUnwrap(c2.cards.first)
            await b.apply(c2)
            await b.undo()
            XCTAssertTrue(b.canStage, "fixture: undoing one of two covers left nothing staged")

            // Sealed ONCE and reused: `decode` re-points the shared kernel at
            // whatever it reads, so a second seal after it would be describing a
            // different resident game.
            let pStaged = try await b.stagedPayload()
            let env = try await MessageEnvelope.decode(payload: pStaged, viewer: -1)
            XCTAssertNotEqual(env.newAtoms, MessageEnvelope.newAtomsNothing,
                              "a bubble that still carries a cover called itself empty")

            let opened = MessageTurnController(parentPayload: pStaged, parent: env, mySeat: 0)
            await opened.begin()
            let covers = opened.openReplayEvents.filter { $0.kind == .cover }
            let moved = Set(covers.flatMap { $0.cards.compactMap { $0?.identity } })
            XCTAssertEqual(covers.count, 1, "the surviving cover must animate, exactly once")
            XCTAssertTrue(moved.contains(firstCover.identity),
                          "the cover that was NOT undone is the one that animates")
            XCTAssertFalse(moved.contains(secondCover.identity),
                           "the undone cover animated anyway")
            return
        }
        XCTFail("no 2p deal in 120 tries produced a double attack the defender could double-cover")
    }

    /// THE CLOCK IS NOT RESTARTED EITHER. `sent_at` is not "when these bytes
    /// were made", it is when the move inside them was played - and the
    /// defender's 15-second pickup hold measures from it. A cancel bubble
    /// carries no move, so stamping it with the current time would hand every
    /// recipient a fresh hold on an attack that was sent long before: change
    /// your mind three times and the defender waits three times as long. The
    /// empty bubble repeats the clock of the chain it re-seals.
    func testTheEmptyBubbleDoesNotRestartThePickupHold() async throws {
        // An attack sent 12 seconds ago: 3 of the 15-second hold are left, which
        // a restart would visibly reset to the full 15.
        let elapsed = 12
        let (p0, e0) = try await attackedChain(gameId: 1603,
                                               sentAt: MessageKernel.clockNow() - elapsed)
        let b = MessageTurnController(parentPayload: p0, parent: e0, mySeat: 1)
        await b.begin()
        let heldBefore = b.pickupHold
        try XCTSkipIf(heldBefore == 0, "fixture: this deal put no hold on the defender")

        await b.apply(try XCTUnwrap(b.legal.first { $0.type == .cover }))
        await b.undo()
        let pEmpty = try await b.stagedPayload(sentAt: MessageKernel.clockNow())
        let eEmpty = try await MessageEnvelope.decode(payload: pEmpty, viewer: -1)

        XCTAssertEqual(eEmpty.sentAt, e0.sentAt,
                       "the cancel bubble stamped itself NOW and restarted the hold")
        let reopened = MessageTurnController(parentPayload: pEmpty, parent: eEmpty, mySeat: 1)
        await reopened.begin()
        XCTAssertLessThanOrEqual(reopened.pickupHold, heldBefore,
                                 "the hold grew from \(heldBefore)s to \(reopened.pickupHold)s "
                                 + "across a bubble that changed nothing")
    }

    /// A REAL move still stamps the real time - the control for the test above,
    /// so "carry the parent's clock" cannot quietly become "never stamp".
    func testARealMoveStillStampsItsOwnSendTime() async throws {
        let old = MessageKernel.clockNow() - 40
        let (p0, e0) = try await attackedChain(gameId: 1604, sentAt: old)
        let b = MessageTurnController(parentPayload: p0, parent: e0, mySeat: 1)
        await b.begin()
        await b.apply(try XCTUnwrap(b.legal.first { $0.type == .cover }))
        let now = MessageKernel.clockNow()
        let env = try await MessageEnvelope.decode(payload: try await b.stagedPayload(sentAt: now),
                                                   viewer: -1)
        XCTAssertEqual(env.sentAt, now, "a move that was really played must carry its own clock")
        XCTAssertNotEqual(env.sentAt, e0.sentAt, "fixture: the two clocks are indistinguishable")
    }
}
