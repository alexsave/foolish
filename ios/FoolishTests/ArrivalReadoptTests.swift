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
}
