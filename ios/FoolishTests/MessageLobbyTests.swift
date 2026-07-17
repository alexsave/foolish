// The N≥3 WAITING lobby (design §5.2), end to end through the kernel — the send
// path a creator and each joiner take, with no Messages harness. Proves that a
// phase-0 bubble seals and decodes (seed + joins, zero actions), that a joiner
// appends the lowest free seat, and that the joiner who FILLS the last seat can
// seal a LIVE game the first attacker then plays. If this holds, the lobby is a
// UI over primitives that already agree with the C and wasm suites.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageLobbyTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ Int(salt)) | 1 })
    }

    /// A creator picks 4 players and seats only themselves: the sealed bubble is
    /// WAITING, carries the one join, and decodes back to the same lobby anywhere.
    func testCreatorSealsAWaitingBubble() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(3), players: 4)   // fixes n_players + seed resident
        let payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 900,
                                              parent8: Data(repeating: 0, count: 8),
                                              joins: [MessageJoin(seat: 0, name: "Alex")])
        XCTAssertFalse(payload.isEmpty)

        let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        XCTAssertEqual(env.phase, 0, "WAITING")
        XCTAssertEqual(env.nPlayers, 4)
        XCTAssertEqual(env.turn, 0, "a lobby carries no actions")
        XCTAssertEqual(env.round, 0)
        XCTAssertEqual(env.joins, [MessageJoin(seat: 0, name: "Alex")])
    }

    /// A joiner adopts the WAITING bubble, claims the lowest free seat, and reseals
    /// WAITING while seats remain — the joins list grows, nothing else does.
    func testJoinerAppendsLowestFreeSeatAndStaysWaiting() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(5), players: 4)
        let created = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 901,
                                              parent8: Data(repeating: 0, count: 8),
                                              joins: [MessageJoin(seat: 0, name: "Alex")])

        // Joiner adopts, then claims the lowest free seat (1).
        let env = try await MessageEnvelope.decode(payload: created, viewer: -1)
        let free = (0..<env.nPlayers).first { s in !env.joins.contains { $0.seat == s } }
        XCTAssertEqual(free, 1, "seat 0 is taken; the next free seat is 1")
        let joins2 = (env.joins + [MessageJoin(seat: free!, name: "Sveta")]).sorted { $0.seat < $1.seat }
        let payload2 = try await k.seal(phase: 0, lastActorSeat: free!, gameId: 901,
                                               parent8: MessageTurnController.firstEight(hex: env.digest),
                                               joins: joins2)

        let env2 = try await MessageEnvelope.decode(payload: payload2, viewer: -1)
        XCTAssertEqual(env2.phase, 0, "still WAITING with a seat open")
        XCTAssertEqual(env2.joins.count, 2)
        XCTAssertEqual(env2.joins.map(\.name), ["Alex", "Sveta"])
        XCTAssertEqual(env2.lastActorSeat, 1, "the bubble records who just joined")
    }

    /// The joiner who fills the LAST seat seals a LIVE game (§5.2 "last joiner
    /// deals"): phase flips to 2, the deal is on, and the first attacker has a
    /// legal move to make on it.
    func testLastJoinerSealsALiveGame() async throws {
        let k = MessageKernel.shared
        let gid: UInt64 = 902
        try await k.newGame(seed: freshSeed(9), players: 3)
        var joins = [MessageJoin(seat: 0, name: "Alex")]
        var payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gid,
                                              parent8: Data(repeating: 0, count: 8), joins: joins)

        // Two joiners fill seats 1 and 2; the second flips the game LIVE.
        for (seat, name) in [(1, "Sveta"), (2, "Boris")] {
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            joins = (env.joins + [MessageJoin(seat: seat, name: name)]).sorted { $0.seat < $1.seat }
            let parent = MessageTurnController.firstEight(hex: env.digest)
            if joins.count == env.nPlayers {
                payload = try await k.seal(phase: 2, lastActorSeat: seat, gameId: gid, parent8: parent, joins: joins)
            } else {
                payload = try await k.seal(phase: 0, lastActorSeat: seat, gameId: gid,
                                                  parent8: parent, joins: joins)
            }
        }

        let live = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        XCTAssertEqual(live.phase, 2, "the last claim starts the game")
        XCTAssertEqual(live.joins.count, 3, "everyone is named")

        // Somebody can actually act on the freshly live game (first attacker).
        var someoneCanPlay = false
        for s in 0..<live.nPlayers where await k.residentLegal(seat: s).contains(where: { $0.type != .wait }) {
            someoneCanPlay = true; break
        }
        XCTAssertTrue(someoneCanPlay, "a live game has a first attacker with a legal move")
    }
}
