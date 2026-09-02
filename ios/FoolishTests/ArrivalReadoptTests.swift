// ArrivalReadoptTests — ROUND 12: a move arriving on an open board must not
// rebuild the board.
//
// THE BUG. Every adopt built a NEW MessageTurnController, and `GameSurface`
// keys the board on the controller's identity (`.id(ObjectIdentifier(controller))`).
// A new controller therefore meant SwiftUI threw the whole board away and built
// another: fresh `@State`, unmeasured geometry, a first paint at defaults. That
// teardown is what the owner reports as "still flashes if move comes in during
// expanded screen" — earlier rounds fixed the `Color.clear` gap between the two
// boards, but the rebuild itself was still there.
//
// The fix folds the arriving chain into the LIVE controller instead. What has to
// be true for that to be safe is a rule about IDENTITY, and getting it subtly
// wrong is worse than the flash — reusing a controller across a different game
// would put one game's chain on another game's measured board. So the rule is a
// pure function and these assert it directly, both ways.

import XCTest
@testable import FoolishKit

@MainActor
final class ArrivalReadoptTests: XCTestCase {

    private func controller(seat: Int, gameId: UInt64) async throws -> MessageTurnController {
        let k = MessageKernel.shared
        try await k.newGame(seed: Data(repeating: 7, count: 32), players: 2)
        let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Vera")]
        let payload = try await k.seal(phase: 2, lastActorSeat: 0, gameId: gameId,
                                       parent8: Data(repeating: 0, count: 8), joins: joins)
        let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        let c = MessageTurnController(parentPayload: payload, parent: env, mySeat: seat)
        await c.begin()
        return c
    }

    func testTheSameGameAndSeatIsFoldedIn() async throws {
        let c = try await controller(seat: 1, gameId: 0xF001)
        XCTAssertTrue(c.canAdopt(seat: 1, gameId: c.gameIdString),
                      "an arrival on the open board keeps the board")
    }

    func testADifferentGameIsNotFoldedIn() async throws {
        let c = try await controller(seat: 1, gameId: 0xF001)
        XCTAssertFalse(c.canAdopt(seat: 1, gameId: "999999"),
                       "a different game is a different board — rebuild it")
    }

    func testADifferentSeatIsNotFoldedIn() async throws {
        let c = try await controller(seat: 1, gameId: 0xF001)
        XCTAssertFalse(c.canAdopt(seat: 0, gameId: c.gameIdString),
                       "a different seat sees a different hand — rebuild it")
    }

    /// A controller that has not finished `begin()` has no board to preserve
    /// and no resident chain to fold onto.
    func testAnUnstartedControllerIsNotFoldedIn() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: Data(repeating: 7, count: 32), players: 2)
        let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Vera")]
        let payload = try await k.seal(phase: 2, lastActorSeat: 0, gameId: 0xF001,
                                       parent8: Data(repeating: 0, count: 8), joins: joins)
        let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        let fresh = MessageTurnController(parentPayload: payload, parent: env, mySeat: 1)
        XCTAssertFalse(fresh.canAdopt(seat: 1, gameId: fresh.gameIdString),
                       "not ready yet — there is nothing to keep")
    }

    /// And the fold itself: the SAME controller instance moves on to the new
    /// chain, which is the property the board's `.id` depends on. `arrivalTick`
    /// moves so the board knows to play the arrival as a cold open would.
    func testAdoptKeepsTheInstanceAndMarksTheArrival() async throws {
        let k = MessageKernel.shared
        let c = try await controller(seat: 1, gameId: 0xF001)
        let before = ObjectIdentifier(c)
        let tickBefore = c.arrivalTick

        // Whichever seat opens this deal attacks; that chain is what "arrives".
        // Asked, not assumed — the first attacker is whoever holds the lowest
        // trump, so hard-coding a seat makes the test SKIP on half of all deals,
        // and a test that skips is a test that is not run.
        var attacker = -1
        var opening: Move?
        for seat in 0..<2 {
            if let m = (await k.residentLegal(seat: seat)).first(where: { $0.type == .attack }) {
                attacker = seat; opening = m; break
            }
        }
        let atk = try XCTUnwrap(opening, "some seat must be able to open the bout")
        try await k.apply(seat: attacker, move: atk)
        let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Vera")]
        let next = try await k.seal(phase: 2, lastActorSeat: attacker, gameId: 0xF001,
                                    parent8: Data(repeating: 0, count: 8), joins: joins)
        let nextEnv = try await MessageEnvelope.decode(payload: next, viewer: -1)

        await c.adopt(payload: next, parent: nextEnv)

        XCTAssertEqual(ObjectIdentifier(c), before, "the board's identity must survive an arrival")
        XCTAssertGreaterThan(c.arrivalTick, tickBefore, "the board must be told this was an arrival")
        XCTAssertEqual(c.basePayload, next, "…and the controller is now on the arrived chain")
        XCTAssertFalse(c.view?.battles.isEmpty ?? true, "…whose attack is on the table")
    }

    /// ROUND 18, the vanishing first attack (two devices, 1.0(19)). Messages can
    /// deliver the same bubble twice - a re-delivered didReceive, or two racing
    /// maybeAdoptIncoming tasks whose "same chain" pre-check both read the base
    /// before the first adopt wrote it. The FIRST adopt publishes a changed view;
    /// the board's `.onChange(of: controller.view)` consumes the veil and
    /// animates the move, which lands. The SECOND adopt of the same chain then
    /// published an UNCHANGED view - so no onChange ever fired again - while
    /// re-arming `replayPending`. That veil hid every card in
    /// `openReplayTouchedCardIds` at rest, forever: the attack card sat in its
    /// laid-out slot at opacity 0 ("flies, lands, then disappears"; a vanished
    /// cover leaves the attack under it reading as un-tilted; a veiled refill is
    /// a hand of 5 against a full deck). Reproduced end to end with
    /// HARNESS_SCENARIO=arrival HARNESS_ARRIVE_WARMUP=0 HARNESS_ARRIVE_DUP=0
    /// (ORACLE stuckVeil=true). This pins the controller half: a duplicate adopt
    /// must never leave a veil standing that no view change will take down.
    func testADuplicateArrivalCannotStrandTheVeil() async throws {
        let k = MessageKernel.shared
        let c = try await controller(seat: 1, gameId: 0xF001)

        // The first attack of the game arrives on the open board.
        var attacker = -1
        var opening: Move?
        for seat in 0..<2 {
            if let m = (await k.residentLegal(seat: seat)).first(where: { $0.type == .attack }) {
                attacker = seat; opening = m; break
            }
        }
        let atk = try XCTUnwrap(opening, "some seat must be able to open the bout")
        try await k.apply(seat: attacker, move: atk)
        let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Vera")]
        let next = try await k.seal(phase: 2, lastActorSeat: attacker, gameId: 0xF001,
                                    parent8: Data(repeating: 0, count: 8), joins: joins)
        let nextEnv = try await MessageEnvelope.decode(payload: next, viewer: -1)

        await c.adopt(payload: next, parent: nextEnv)
        XCTAssertTrue(c.replayPending,
                      "a real arrival changes the view, so the veil goes up for the board to consume")
        // The board's `.onChange(of: controller.view)` fires for that change and
        // takes the veil over (flyBoutEndToDiscard's defer). The move animates
        // and lands.
        c.consumeReplayPending()
        let shown = c.view
        let tick = c.arrivalTick

        // The SAME bubble is delivered again.
        await c.adopt(payload: next, parent: nextEnv)

        XCTAssertEqual(c.view, shown, "a duplicate moves nothing - no onChange will fire")
        XCTAssertFalse(c.replayPending,
                       "a veil armed against an unchanged view is a veil nothing consumes: every card "
                       + "the replay touches stays laid out at opacity 0 until the board is rebuilt")
        // …and the board must not be TOLD it was an arrival either: a moved
        // `arrivalTick` makes the next view change - my own next move - play as
        // a cold open (`prior = nil`) instead of as the placement it is. The
        // identical chain is already resident; the whole adopt is a no-op.
        XCTAssertEqual(c.arrivalTick, tick,
                       "an identical chain is not an arrival - nothing changed hands")
    }

    /// The DEEPER half of the same defect, which the identical-payload skip
    /// above cannot catch: an arriving chain whose BYTES differ but whose BOARD
    /// is the one already on screen. Real shape: the same turn re-sealed at a
    /// new send clock (`sentAt` is in the envelope, so a re-staged/re-sent
    /// bubble is new bytes carrying the same game). The adopt proceeds - the
    /// base really does move to the new chain - but the published view is EQUAL
    /// to the shown one, so `.onChange(of: controller.view)` will not fire and
    /// nothing can consume a veil. `publish` must not arm one.
    func testAnEqualBoardArrivalCannotArmTheVeil() async throws {
        let k = MessageKernel.shared
        let c = try await controller(seat: 1, gameId: 0xF001)

        var attacker = -1
        var opening: Move?
        for seat in 0..<2 {
            if let m = (await k.residentLegal(seat: seat)).first(where: { $0.type == .attack }) {
                attacker = seat; opening = m; break
            }
        }
        let atk = try XCTUnwrap(opening, "some seat must be able to open the bout")
        try await k.apply(seat: attacker, move: atk)
        let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Vera")]
        // The same turn, sealed twice at different send clocks: two chains,
        // different bytes, one board.
        // Sealed back to back from the same resident, so BOTH bubbles carry the
        // attack as their own move (the same bubble delta) - a decode in between
        // would advance the staged mark and turn s2 into an empty re-seal, whose
        // empty event stream never arms the veil regardless of any guard.
        let s1 = try await k.seal(phase: 2, lastActorSeat: attacker, gameId: 0xF001,
                                  parent8: Data(repeating: 0, count: 8), joins: joins,
                                  sentAt: 100)
        let s2 = try await k.seal(phase: 2, lastActorSeat: attacker, gameId: 0xF001,
                                  parent8: Data(repeating: 0, count: 8), joins: joins,
                                  sentAt: 200)
        XCTAssertNotEqual(s1, s2, "the send clock is in the envelope - these are two chains")

        let env1 = try await MessageEnvelope.decode(payload: s1, viewer: -1)
        await c.adopt(payload: s1, parent: env1)
        c.consumeReplayPending()   // the board consumed the real arrival's veil
        let shown = c.view

        let env2 = try await MessageEnvelope.decode(payload: s2, viewer: -1)
        await c.adopt(payload: s2, parent: env2)

        XCTAssertEqual(c.basePayload, s2, "the re-sent chain IS adopted - the base moves")
        XCTAssertEqual(c.view, shown, "…but the board it carries is the one already shown")
        XCTAssertFalse(c.openReplayEvents.isEmpty,
                       "sanity: the chain does carry a last move - the guard, not vacuity, "
                       + "must be what keeps the veil down")
        XCTAssertFalse(c.replayPending,
                       "no view change will fire onChange, so an armed veil would stand forever")
    }
}
