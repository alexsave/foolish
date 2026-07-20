// The lobby (design §5.2, rewritten for lobby v2 — docs/IMESSAGE_LOBBY_V2.md,
// notes 19/20/25), end to end through the kernel — the send path a creator and
// each joiner take, with no Messages harness. Proves that a phase-0 bubble
// seals and decodes (seed + joins, zero actions), that a joiner appends the
// lowest free seat and the lobby NEVER auto-starts (no matter how many have
// joined, or that the wire's 8-seat cap is reached), and that Start — a
// separate, explicit action any joined player may take once 2+ have joined —
// re-derives the LOCKED seed at the real player count and seals a LIVE game
// the first attacker can then play. If this holds, the lobby is a UI over
// primitives that already agree with the C and wasm suites.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageLobbyTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ Int(salt)) | 1 })
    }

    /// A creator locks the seed in and opens an OPEN lobby: the kernel is dealt
    /// at the wire's MAX capacity (8) — not a chosen count, because lobby v2
    /// asks for none up front — and the sealed bubble is WAITING, carrying only
    /// the creator's own join. n_players==8 here is the open-lobby convention
    /// (see docs/IMESSAGE_LOBBY_V2.md), never a real 8-player game.
    func testCreatorSealsAnOpenWaitingBubble() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(3), players: 8)   // fixes seed + open capacity
        let payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 900,
                                              parent8: Data(repeating: 0, count: 8),
                                              joins: [MessageJoin(seat: 0, name: "Alex")])
        XCTAssertFalse(payload.isEmpty)

        let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        XCTAssertEqual(env.phase, 0, "WAITING")
        XCTAssertEqual(env.nPlayers, 8, "the open-lobby convention: max capacity, not a chosen count")
        XCTAssertEqual(env.turn, 0, "a lobby carries no actions")
        XCTAssertEqual(env.round, 0)
        XCTAssertEqual(env.joins, [MessageJoin(seat: 0, name: "Alex")])
    }

    /// A joiner adopts the WAITING bubble, claims the lowest free seat, and
    /// reseals WAITING — the joins list grows, nothing else does. Unchanged
    /// mechanics from the old N>=3 lobby.
    func testJoinerAppendsLowestFreeSeatAndStaysWaiting() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(5), players: 8)
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
        XCTAssertEqual(env2.phase, 0, "still WAITING with room to spare")
        XCTAssertEqual(env2.nPlayers, 8, "the open capacity never shrinks on a join")
        XCTAssertEqual(env2.joins.count, 2)
        XCTAssertEqual(env2.joins.map(\.name), ["Alex", "Sveta"])
        XCTAssertEqual(env2.lastActorSeat, 1, "the bubble records who just joined")
    }

    /// The old lobby auto-started the moment joins filled n_players. Lobby v2
    /// removes that entirely (notes 19/20/25: "unspecified player count until
    /// someone hits start") — joining NEVER flips the phase, no matter how many
    /// have joined, including reaching the wire's 8-seat cap.
    func testJoiningNeverAutoStartsEvenAtCapacity() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(11), players: 8)
        var joins = [MessageJoin(seat: 0, name: "P0")]
        var payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 950,
                                              parent8: Data(repeating: 0, count: 8), joins: joins)

        for seat in 1..<8 {
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            XCTAssertEqual(env.phase, 0, "still WAITING before seat \(seat) joins")
            joins = (env.joins + [MessageJoin(seat: seat, name: "P\(seat)")]).sorted { $0.seat < $1.seat }
            payload = try await k.seal(phase: 0, lastActorSeat: seat, gameId: 950,
                                              parent8: MessageTurnController.firstEight(hex: env.digest),
                                              joins: joins)
        }

        // All 8 seats claimed — the cap is reached, and the lobby is STILL
        // WAITING: filling every seat is no longer special.
        let full = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        XCTAssertEqual(full.phase, 0, "reaching the 8-seat cap does not start the game")
        XCTAssertEqual(full.joins.count, 8)
    }

    /// Start (lobby v2's new, explicit action): any joined player, once 2+ have
    /// joined, re-derives the game from the seed LOCKED at create at the ACTUAL
    /// joined count — here 3, though the lobby's own capacity was 8 — and seals
    /// a LIVE game the first attacker can play. Proves the reseat mechanism
    /// (MessageKernel.reseatResidentGame) end to end.
    func testStartRedealsAtTheActualJoinCountAndSealsLive() async throws {
        let k = MessageKernel.shared
        let gid: UInt64 = 902
        try await k.newGame(seed: freshSeed(9), players: 8)   // open lobby, cap 8
        var joins = [MessageJoin(seat: 0, name: "Alex")]
        var payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gid,
                                              parent8: Data(repeating: 0, count: 8), joins: joins)

        // Two more join (seats 1, 2) — still WAITING/8 throughout (see the test
        // above), so only the THIRD join's payload is threaded into Start below.
        for (seat, name) in [(1, "Sveta"), (2, "Boris")] {
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            joins = (env.joins + [MessageJoin(seat: seat, name: name)]).sorted { $0.seat < $1.seat }
            payload = try await k.seal(phase: 0, lastActorSeat: seat, gameId: gid,
                                              parent8: MessageTurnController.firstEight(hex: env.digest),
                                              joins: joins)
        }

        let lobbyEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        XCTAssertEqual(lobbyEnv.phase, 0, "3 joined, still an open lobby — nobody hit Start yet")
        XCTAssertEqual(lobbyEnv.joins.count, 3)

        // Start, as seat 1 (Sveta) — ANY joined player, not just the creator or
        // the last joiner: re-adopt the lobby, reseat at joins.count (3), seal LIVE.
        _ = try await k.decode(payload: payload, viewer: -1)
        try await k.reseatResidentGame(players: lobbyEnv.joins.count)
        let live = try await k.seal(phase: 2, lastActorSeat: 1, gameId: gid,
                                          parent8: MessageTurnController.firstEight(hex: lobbyEnv.digest),
                                          joins: lobbyEnv.joins)

        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
        XCTAssertEqual(liveEnv.phase, 2, "Start seals a LIVE handoff")
        XCTAssertEqual(liveEnv.nPlayers, 3, "the REAL count (3), not the lobby's open capacity (8)")
        XCTAssertEqual(liveEnv.turn, 0, "no move yet — just the deal")
        XCTAssertEqual(liveEnv.joins.count, 3, "everyone who joined is named")

        // Somebody can actually act on the freshly-dealt 3p game (first attacker).
        var someoneCanPlay = false
        for s in 0..<liveEnv.nPlayers where await k.residentLegal(seat: s).contains(where: { $0.type != .wait }) {
            someoneCanPlay = true; break
        }
        XCTAssertTrue(someoneCanPlay, "a live game has a first attacker with a legal move")
    }
}
