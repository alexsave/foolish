// Leaving a lobby (round 16), and the two-button row it introduced.
//
// A leave is a WAITING reseal without me, and the awkward part is not the
// removal - it is everything downstream that assumed a lobby only ever grows:
// seats must stay contiguous or Start deals a game the wire refuses,
// `lastActorSeat` has to name a seat that no longer holds anyone, and the
// leaver has to stop being resolved back into the table they walked out of.
// Each of those is a test here.
//
// What is NOT tested: that a leave beats a concurrent Start. It does not, by
// the owner's explicit call - Messages hands every device whichever bubble
// arrives last and there is no reading past it, so a collision drops one of
// the two. Asserting an ordering we deliberately did not build would be a test
// of a fiction.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageLobbyExitTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 13 &+ Int(salt)) | 1 })
    }

    /// Seal a WAITING lobby with `names` seated 0..<n at capacity 8.
    private func lobby(_ names: [String], seed: UInt8, gameId: UInt64)
        async throws -> (Data, MessageEnvelope) {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(seed), players: 8)
        let joins = names.enumerated().map { MessageJoin(seat: $0.offset, name: $0.element) }
        let payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gameId,
                                       parent8: Data(repeating: 0, count: 8), joins: joins)
        return (payload, try await MessageEnvelope.decode(payload: payload, viewer: -1))
    }

    /// The reseal a leave performs, in the shape MessagesRootView.leaveLobby
    /// builds it: everyone but `seat`, renumbered from 0, and `lastActorSeat`
    /// parked on the first free slot.
    private func leave(_ env: MessageEnvelope, payload: Data, seat: Int,
                       gameId: UInt64) async throws -> MessageEnvelope {
        let k = MessageKernel.shared
        _ = try await k.decode(payload: payload, viewer: -1)
        let remaining = env.joins.filter { $0.seat != seat }.sorted { $0.seat < $1.seat }
        let joins = remaining.enumerated().map { MessageJoin(seat: $0.offset, name: $0.element.name) }
        let out = try await k.seal(phase: 0, lastActorSeat: joins.count, gameId: gameId,
                                   parent8: MessageTurnController.firstEight(hex: env.digest),
                                   joins: joins)
        return try await MessageEnvelope.decode(payload: out, viewer: -1)
    }

    // MARK: the wire

    /// A leave from the middle renumbers everyone below it, and the CYCLE that
    /// survives is the one that was there. Seat numbers were never identity -
    /// the order is.
    func testLeavingCompactsSeatsAndKeepsTheOrder() async throws {
        let (p, env) = try await lobby(["Alex", "Bob", "Cindy", "Dina"], seed: 3, gameId: 7100)
        let after = try await leave(env, payload: p, seat: 1, gameId: 7100)   // Bob

        XCTAssertEqual(after.joins.map(\.name), ["Alex", "Cindy", "Dina"])
        XCTAssertEqual(after.joins.map(\.seat), [0, 1, 2],
                       "a hole would seal a seat >= the count Start deals at")
        XCTAssertEqual(after.phase, 0)
        XCTAssertEqual(after.turn, 0)
    }

    /// `lastActorSeat` must be a seat, but mine is gone - so it parks on the
    /// first free slot. Two things ride on that, and both are asserted: nobody
    /// still seated is read as the newest sender (which would withhold Start
    /// from them under M9), and "the actor is not seated" is how a receiver
    /// tells a leave from a join with no name to go on.
    func testLeaveParksTheActorOnAFreeSeat() async throws {
        let (p, env) = try await lobby(["Alex", "Bob", "Cindy"], seed: 5, gameId: 7101)
        let after = try await leave(env, payload: p, seat: 2, gameId: 7101)   // Cindy

        XCTAssertEqual(after.joins.count, 2)
        XCTAssertFalse(after.joins.contains { $0.seat == after.lastActorSeat },
                       "the leave's actor must not be a seated player")
        for j in after.joins {
            XCTAssertNotEqual(
                LobbyControls.offered(mySeat: j.seat, joined: after.joins.count,
                                      capacity: after.nPlayers,
                                      iSentTheInvite: after.lastActorSeat == j.seat),
                .waiting,
                "\(j.name) was withheld from Start by a leave they did not send")
        }
    }

    /// Start after a leave deals the SMALLER table, and deals it at all - the
    /// contiguity the compaction preserves is exactly what this needs.
    func testStartAfterALeaveDealsTheRemainingTable() async throws {
        let (p, env) = try await lobby(["Alex", "Bob", "Cindy"], seed: 7, gameId: 7102)
        let after = try await leave(env, payload: p, seat: 1, gameId: 7102)
        let afterPayload = try await MessageKernel.shared.seal(
            phase: 0, lastActorSeat: after.lastActorSeat, gameId: 7102,
            parent8: MessageTurnController.firstEight(hex: env.digest),
            joins: after.joins)

        let live = try await MessageKernel.shared.startFromLobby(
            lobbyPayload: afterPayload, gameId: 7102, actingSeat: 0,
            parent8: MessageTurnController.firstEight(hex: after.digest),
            joins: after.joins)
        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)

        XCTAssertEqual(liveEnv.phase, 2)
        XCTAssertEqual(liveEnv.nPlayers, 2, "Bob was still dealt in")
        let view = await MessageKernel.shared.residentView(viewer: -1)
        XCTAssertEqual(view?.players.count, 2)
    }

    /// A leave clears the fool's penalty: the table is not the table the carry
    /// was taken over, so the rematch rule stands down (Rule F's guard).
    func testLeavingCancelsAPendingFoolPenalty() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(9), players: 8)
        let joins = ["Alex", "Bob", "Cindy"].enumerated()
            .map { MessageJoin(seat: $0.offset, name: $0.element) }
        let armed = await k.armRematchCarry(joins: joins, foolSeat: 1)
        XCTAssertTrue(armed)
        let p = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 7103,
                                 parent8: Data(repeating: 0, count: 8), joins: joins)
        let env = try await MessageEnvelope.decode(payload: p, viewer: -1)
        XCTAssertTrue(env.carriesPenalty)

        let after = try await leave(env, payload: p, seat: 2, gameId: 7103)   // Cindy walks
        XCTAssertTrue(after.carriesPenalty, "the lobby still carries the question")
        let shown = await k.penaltyFoolSeat(joins: after.joins,
                                            carryKey: after.carryKey!,
                                            carryFool: after.carryFool!)
        XCTAssertNil(shown, "a table someone left still claimed a penalty")
    }

    // MARK: who is offered Exit

    /// The owner's rule: a seated player may leave once somebody else is seated
    /// too. Below that the wire itself refuses - a WAITING envelope must carry
    /// at least one join - so a lone creator's exit is New game, not this.
    func testExitIsOfferedToASeatedPlayerOnceTwoAreIn() {
        XCTAssertTrue(LobbyControls.canExit(mySeat: 0, joined: 2))
        XCTAssertTrue(LobbyControls.canExit(mySeat: 1, joined: 8))
        XCTAssertFalse(LobbyControls.canExit(mySeat: 0, joined: 1), "nothing to leave into")
        XCTAssertFalse(LobbyControls.canExit(mySeat: nil, joined: 3), "a spectator is not in")
    }

    /// THE CASE THE OWNER NAMED: the last player to join cannot Start (M9
    /// withholds it while the lobby has room), and before round 16 that left
    /// them with no action at all. They can now leave.
    func testTheLastJoinerCannotStartButCanLeave() {
        let offered = LobbyControls.offered(mySeat: 2, joined: 3, capacity: 8,
                                            iSentTheInvite: true)
        XCTAssertEqual(offered, .waiting, "M9 still withholds Start from the newest sender")
        XCTAssertTrue(LobbyControls.canExit(mySeat: 2, joined: 3),
                      "…and that player would otherwise have nothing to do")
    }

    /// …while anyone else in the same lobby gets BOTH, which is the side-by-side
    /// row.
    func testAnEarlierJoinerGetsStartAndExitTogether() {
        let offered = LobbyControls.offered(mySeat: 0, joined: 3, capacity: 8,
                                            iSentTheInvite: false)
        XCTAssertEqual(offered, .start)
        XCTAssertTrue(LobbyControls.canExit(mySeat: 0, joined: 3))
    }

    /// A player who has not joined is offered neither - Join is their action.
    func testASpectatorIsOfferedJoinNotExit() {
        XCTAssertEqual(LobbyControls.offered(mySeat: nil, joined: 2, capacity: 8), .join)
        XCTAssertFalse(LobbyControls.canExit(mySeat: nil, joined: 2))
    }

    // MARK: the seat cache

    /// Leaving forgets the seat. Without this the next open of the same game
    /// resolves me back into a lobby I walked out of (SeatIdentity consults the
    /// cache before anything else).
    func testLeavingForgetsTheCachedSeat() {
        let store = MessageGameStore(defaults: UserDefaults(suiteName: "exit.tests")!)
        store.setSeat(gameId: "7104", chatKey: "chat", seat: 2)
        XCTAssertEqual(store.seat(gameId: "7104", chatKey: "chat"), 2)
        store.forgetSeat(gameId: "7104")
        XCTAssertNil(store.seat(gameId: "7104", chatKey: "chat"))
        XCTAssertNil(store.seatForBubble(gameId: "7104"))
        // Another game's row is untouched.
        store.setSeat(gameId: "7105", chatKey: "chat", seat: 1)
        store.forgetSeat(gameId: "7104")
        XCTAssertEqual(store.seat(gameId: "7105", chatKey: "chat"), 1)
    }
}
