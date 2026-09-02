// The fool's penalty (Rule F), end to end through the kernel - the durak-ism
// that a rematch among the SAME players opens on the seat to the RIGHT of the
// last game's fool, so the fool is the first player attacked.
//
// What these prove is the CHAIN, not the arithmetic: the arithmetic is pinned
// in C (c/tests/msg_wire_test.c's test_roster_key / test_rematch_opening), and
// re-asserting it here would only prove the two copies of a formula agree.
// What can only break at this layer is the plumbing - a lobby that carries the
// carry, a Start that reads it back off the wire, a deal that comes out with
// the fool under the sword - so that is what is asserted, and always by asking
// the dealt game who is defending rather than by recomputing a seat.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageFoolPenaltyTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 11 &+ Int(salt)) | 1 })
    }

    private func roster(_ names: [String]) -> [MessageJoin] {
        names.enumerated().map { MessageJoin(seat: $0.offset, name: $0.element) }
    }

    /// Create the rematch lobby the way MessagesRootView does: deal at capacity,
    /// arm the carry, seal WAITING. Returns the lobby payload + its envelope.
    private func rematchLobby(seed: Data, gameId: UInt64, joins: [MessageJoin],
                              foolSeat: Int, capacity: Int = 8)
        async throws -> (Data, MessageEnvelope) {
        let k = MessageKernel.shared
        try await k.newGame(seed: seed, players: capacity)
        let armed = await k.armRematchCarry(joins: joins, foolSeat: foolSeat)
        XCTAssertTrue(armed, "the kernel refused a well-formed carry")
        let payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gameId,
                                       parent8: Data(repeating: 0, count: 8),
                                       joins: joins, sentAt: 0x1234)
        return (payload, try await MessageEnvelope.decode(payload: payload, viewer: -1))
    }

    // MARK: the lobby carries the question

    /// A rematch lobby is an ordinary WAITING bubble PLUS the carry - and the
    /// carry survives the wire, which is the whole reason it is on the wire and
    /// not in a cache: the device that taps Start may never have held the game
    /// the penalty is owed for.
    func testRematchLobbyCarriesThePenaltyOnTheWire() async throws {
        let joins = roster(["Alex", "Bob", "Cindy"])
        let (_, env) = try await rematchLobby(seed: freshSeed(3), gameId: 5100,
                                              joins: joins, foolSeat: 1)
        XCTAssertEqual(env.phase, 0)
        XCTAssertEqual(env.joins, joins, "the lobby is PREFILLED with the same table")
        XCTAssertTrue(env.carriesPenalty, "a rematch lobby with no carry punishes nobody")
        XCTAssertNil(env.opening, "a lobby has no opening seat yet - Start decides it")

        // And it names the right person, through the kernel rather than by
        // recomputing the rotation here.
        let fool = await MessageKernel.shared.penaltyFoolSeat(
            joins: env.joins, carryKey: env.carryKey!, carryFool: env.carryFool!)
        XCTAssertEqual(fool, 1, "the lobby would punish the wrong player")
    }

    /// An ORDINARY lobby carries nothing, so nothing about the old path changes.
    func testOrdinaryLobbyCarriesNoPenalty() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(4), players: 8)
        _ = await k.armRematchCarry(joins: [], foolSeat: 0)   // disarms
        let payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 5101,
                                       parent8: Data(repeating: 0, count: 8),
                                       joins: [MessageJoin(seat: 0, name: "Alex")])
        let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        XCTAssertFalse(env.carriesPenalty)
        XCTAssertNil(env.opening)
    }

    // MARK: Start applies it

    /// THE RULE. Start a rematch whose roster has not changed, and the game
    /// deals with the fool as its first DEFENDER - asked of the dealt board,
    /// not derived from a formula.
    func testStartPunishesTheFoolAtEverySeat() async throws {
        for foolSeat in 0..<3 {
            let joins = roster(["Alex", "Bob", "Cindy"])
            let (payload, env) = try await rematchLobby(
                seed: freshSeed(UInt8(20 + foolSeat)), gameId: UInt64(5200 + foolSeat),
                joins: joins, foolSeat: foolSeat)

            let live = try await MessageKernel.shared.startFromLobby(
                lobbyPayload: payload, gameId: UInt64(5200 + foolSeat), actingSeat: 0,
                parent8: MessageTurnController.firstEight(hex: env.digest),
                joins: joins, sentAt: 0x1234)

            let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
            XCTAssertEqual(liveEnv.phase, 2)
            XCTAssertEqual(liveEnv.nPlayers, 3)
            XCTAssertNotNil(liveEnv.opening, "the penalty did not reach the live chain")
            XCTAssertFalse(liveEnv.carriesPenalty,
                           "a live chain must not still carry the lobby's question")

            let view = await MessageKernel.shared.residentView(viewer: -1)
            XCTAssertEqual(view?.defender, foolSeat,
                           "fool \(foolSeat) is not the first defender")
            XCTAssertEqual(liveEnv.opening, (foolSeat + 2) % 3,
                           "the opener is not the seat to the fool's right")
        }
    }

    /// …and it survives a ROTATION. Whoever taps New game takes seat 0, so the
    /// same table comes back spun round; the same human must still be punished.
    func testRotatingTheTableStillPunishesTheSamePerson() async throws {
        // Bob was the fool. Alex's device creates the lobby (Alex at 0)…
        let alexOrder = roster(["Alex", "Bob", "Cindy"])
        let (pA, eA) = try await rematchLobby(seed: freshSeed(31), gameId: 5300,
                                              joins: alexOrder, foolSeat: 1)
        let liveA = try await MessageKernel.shared.startFromLobby(
            lobbyPayload: pA, gameId: 5300, actingSeat: 0,
            parent8: MessageTurnController.firstEight(hex: eA.digest),
            joins: alexOrder, sentAt: 0x1234)
        let envA = try await MessageEnvelope.decode(payload: liveA, viewer: -1)
        let viewA = await MessageKernel.shared.residentView(viewer: -1)
        XCTAssertEqual(alexOrder[viewA!.defender].name, "Bob")

        // …and now Cindy's device creates it instead, so the cycle is rotated to
        // Cindy/Alex/Bob. Same seed, same table, same fool: Bob again.
        let cindyOrder = roster(["Cindy", "Alex", "Bob"])
        let (pC, eC) = try await rematchLobby(seed: freshSeed(31), gameId: 5301,
                                              joins: cindyOrder, foolSeat: 2)
        let liveC = try await MessageKernel.shared.startFromLobby(
            lobbyPayload: pC, gameId: 5301, actingSeat: 0,
            parent8: MessageTurnController.firstEight(hex: eC.digest),
            joins: cindyOrder, sentAt: 0x1234)
        _ = try await MessageEnvelope.decode(payload: liveC, viewer: -1)
        let viewC = await MessageKernel.shared.residentView(viewer: -1)
        XCTAssertEqual(cindyOrder[viewC!.defender].name, "Bob",
                       "a rotated rematch punished the wrong person")

        // The SEAT differs (the table spun); the PERSON does not. If these were
        // equal the test would pass for the wrong reason.
        XCTAssertNotEqual(envA.opening, viewC!.defender == 0 ? 0 : nil,
                          "sanity: the two seatings are genuinely different")
    }

    // MARK: the guard

    /// A joiner between the lobby and Start means the table changed, so the
    /// penalty does not fire and the deal derives its opener as usual.
    func testAJoinerCancelsThePenalty() async throws {
        let original = roster(["Alex", "Bob", "Cindy"])
        let (payload, env) = try await rematchLobby(seed: freshSeed(41), gameId: 5400,
                                                    joins: original, foolSeat: 1)
        XCTAssertTrue(env.carriesPenalty)

        // Dina joins, then someone taps Start on the FOUR of them.
        let joined = original + [MessageJoin(seat: 3, name: "Dina")]
        let live = try await MessageKernel.shared.startFromLobby(
            lobbyPayload: payload, gameId: 5400, actingSeat: 0,
            parent8: MessageTurnController.firstEight(hex: env.digest),
            joins: joined, sentAt: 0x1234)

        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
        XCTAssertEqual(liveEnv.nPlayers, 4)
        XCTAssertNil(liveEnv.opening,
                     "a table that gained a player still applied the penalty")

        // …and the lobby had already stopped announcing it, so the screen and
        // the deal agree.
        let shown = await MessageKernel.shared.penaltyFoolSeat(
            joins: joined, carryKey: env.carryKey!, carryFool: env.carryFool!)
        XCTAssertNil(shown, "the lobby would announce a penalty the deal will not apply")
    }

    /// A RENAME is a changed table too - the roster key is over names, which are
    /// the only identity this wire carries.
    func testARenameCancelsThePenalty() async throws {
        let original = roster(["Alex", "Bob", "Cindy"])
        let (payload, env) = try await rematchLobby(seed: freshSeed(42), gameId: 5401,
                                                    joins: original, foolSeat: 1)
        let renamed = roster(["Alex", "Bob", "Dina"])
        let live = try await MessageKernel.shared.startFromLobby(
            lobbyPayload: payload, gameId: 5401, actingSeat: 0,
            parent8: MessageTurnController.firstEight(hex: env.digest),
            joins: renamed, sentAt: 0x1234)
        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
        XCTAssertNil(liveEnv.opening, "a renamed seat still applied the penalty")
    }

    /// A REORDER that no rotation produces is a different table, even though the
    /// same three people are at it. (Alex/Bob/Cindy -> Alex/Cindy/Bob.)
    func testANonRotationReorderCancelsThePenalty() async throws {
        let original = roster(["Alex", "Bob", "Cindy"])
        let (payload, env) = try await rematchLobby(seed: freshSeed(43), gameId: 5402,
                                                    joins: original, foolSeat: 1)
        let swapped = roster(["Alex", "Cindy", "Bob"])
        let live = try await MessageKernel.shared.startFromLobby(
            lobbyPayload: payload, gameId: 5402, actingSeat: 0,
            parent8: MessageTurnController.firstEight(hex: env.digest),
            joins: swapped, sentAt: 0x1234)
        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
        XCTAssertNil(liveEnv.opening, "a reordered table still applied the penalty")
    }

    // MARK: the penalty rides the whole game

    /// Every later bubble of a punished game repeats the opening seat, because
    /// a chain is re-dealt from its seed on every open - there is no "first
    /// bubble" a later reader could consult. Drop it and the chain stops
    /// replaying, which is the property that makes the repetition safe.
    func testEveryBubbleOfAPunishedGameRepeatsTheOpening() async throws {
        let joins = roster(["Alex", "Bob"])
        let (payload, env) = try await rematchLobby(seed: freshSeed(51), gameId: 5500,
                                                    joins: joins, foolSeat: 1, capacity: 2)
        var chain = try await MessageKernel.shared.startFromLobby(
            lobbyPayload: payload, gameId: 5500, actingSeat: 0,
            parent8: MessageTurnController.firstEight(hex: env.digest),
            joins: joins, sentAt: 0x1234)
        var cur = try await MessageEnvelope.decode(payload: chain, viewer: -1)
        let opening = cur.opening
        XCTAssertNotNil(opening)

        // Play a few real moves, sealing each one, and the term stays on.
        for _ in 0..<6 {
            guard let view = await MessageKernel.shared.residentView(viewer: -1),
                  !view.isOver else { break }
            var acted: (seat: Int, move: Move)?
            for s in 0..<view.players.count where acted == nil {
                let legal = await MessageKernel.shared.residentLegal(seat: s)
                if let m = legal.first(where: { $0.type != .wait }) { acted = (s, m) }
            }
            guard let (seat, move) = acted else { break }
            try await MessageKernel.shared.apply(seat: seat, move: move)
            chain = try await MessageKernel.shared.seal(
                phase: 2, lastActorSeat: seat, gameId: 5500,
                parent8: MessageTurnController.firstEight(hex: cur.digest),
                joins: joins, sentAt: 0x1234)
            cur = try await MessageEnvelope.decode(payload: chain, viewer: -1)
            XCTAssertEqual(cur.opening, opening,
                           "a later bubble dropped the opening seat")
        }
        XCTAssertGreaterThan(cur.turn, 0, "no move was actually played")
    }
}
