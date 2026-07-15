// EngineC.swift — the ONLY Swift file that touches the C API (CFoolish /
// ios_api.h). Everything else in the app goes through this. Modeled as an
// `actor` so access to the kernel's single static Game is serialized without a
// manual queue, and heavy bot deliberation (cordite/octogen) runs off the main
// thread by construction. No Durak rule lives here — this is pure marshaling
// (§3, §7.1).

import Foundation
import CFoolish

/// A failure returned by the C bridge. Codes mirror ios_api.h (FIO_E*).
public enum EngineError: Error, Equatable, Sendable {
    case noGame
    case badArg
    case capacity
    case parse
    case reject(code: Int)      // ENGINE_REJECT_* from game.h
    case noStrategy
    case replay(code: Int)      // REPLAY_E* from replay.h
    case unknown(Int)
}

public actor EngineC {
    public init() {}

    // ios_api.h error codes, restated so we never depend on how C macros import.
    private static let eOK: Int32 = 0
    private static let eBadArg: Int32 = -1
    private static let eNoGame: Int32 = -2
    private static let eCap: Int32 = -3
    private static let eParse: Int32 = -4
    private static let eReject: Int32 = -5
    private static let eNoStrat: Int32 = -6
    private static let eReplay: Int32 = -7

    // MARK: lifecycle

    /// Deal a fresh game from `seed`. 32+ bytes ⇒ the wide ChaCha deal (whole
    /// deal space, reproducible). All seats start human; assign bots after.
    public func newGame(seed: Data, players: Int) throws {
        let rc = seed.withUnsafeBytes { raw -> Int32 in
            fio_new_game(raw.bindMemory(to: UInt8.self).baseAddress, Int32(seed.count), Int32(players))
        }
        try Self.check(rc)
    }

    public func setSeatStrategy(seat: Int, strategyId: Int) throws {
        try Self.check(fio_set_seat_strategy(Int32(seat), Int32(strategyId)))
    }

    public func hasGame() -> Bool { fio_has_game() != 0 }

    // MARK: observation

    public func stateData(viewer: Int) throws -> Data {
        try json { fio_state_json(Int32(viewer), $0, $1) }
    }
    public func publicStateData() throws -> Data {
        try json { fio_public_state_json($0, $1) }
    }
    public func legalMovesData(seat: Int) throws -> Data {
        try json { fio_legal_moves_json(Int32(seat), $0, $1) }
    }
    /// Bitmask of seats with a pending legal action, or a thrown error.
    public func actorMask() throws -> Int {
        let m = fio_actor_mask()
        if m < 0 { try Self.check(m) }
        return Int(m)
    }
    /// Fool seat once the game is over, else -1.
    public func gameOver() throws -> Int {
        let r = fio_game_over()
        if r < -1 { try Self.check(r) }   // -1 is "still running", not an error
        return Int(r)
    }

    // MARK: intents

    public func apply(seat: Int, move: Move) throws {
        let rc = move.jsonString().withCString { fio_apply_json(Int32(seat), $0) }
        if rc == Self.eReject { throw EngineError.reject(code: Int(fio_last_reject())) }
        try Self.check(rc)
    }

    /// Drive one eligible bot seat (any seat but `humanSeat`). Returns the
    /// move it made, or nil when it is the human's turn / the game is over.
    ///
    /// Deprecated for the app's bot loop: it drives the FIRST eligible seat and
    /// cannot bundle silent actions. `botDrive` is the cycle the website runs.
    public func botStep(humanSeat: Int) throws -> Move? {
        var data: Data?
        let rc = try jsonAllowingEmpty({ fio_bot_step_json(Int32(humanSeat), $0, $1) }, into: &data)
        if rc == 0 { return nil }          // no bot acted
        guard let d = data, !d.isEmpty else { return nil }
        return try JSONDecoder().decode(Move.self, from: d)
    }

    /// Run one bot cycle (docs/C_CORE_CONSOLIDATION.md F2/F3): the kernel picks
    /// fairly among simultaneously-eligible bots, applies 0..n actions, bundles
    /// the silent ones, and hands back how long to wait. `humanSeats` are the
    /// seats the kernel must not drive — note that a human being able to act is
    /// NOT a stop condition, so bots throw in while the player deliberates,
    /// exactly as they do online.
    public func botDrive(humanSeats: [Int]) throws -> BotDrive {
        var mask: Int32 = 0
        for s in humanSeats where s >= 0 { mask |= (1 << Int32(s)) }
        let d = try json { fio_bot_drive_json(mask, $0, $1) }
        return try JSONDecoder().decode(BotDrive.self, from: d)
    }

    // MARK: strategies

    public func strategyCount() -> Int { Int(fio_strategy_count()) }

    /// The offline bot roster (id + name). The strategy table is static C data
    /// with no game state, so this is safe to read synchronously off the actor —
    /// the Home/bot-picker needs it before any game exists (§6, §7.2).
    public nonisolated static func roster() -> [(id: Int, name: String)] {
        var out: [(id: Int, name: String)] = []
        let n = Int(fio_strategy_count())
        var buf = [CChar](repeating: 0, count: 64)
        for i in 0..<n {
            let w = fio_strategy_name(Int32(i), &buf, 64)
            let name = w > 0 ? String(decoding: buf.prefix(Int(w)).map { UInt8(bitPattern: $0) }, as: UTF8.self) : "bot \(i)"
            out.append((id: i, name: name))
        }
        return out
    }

    public func strategyName(_ id: Int) throws -> String {
        let d = try json { fio_strategy_name(Int32(id), $0, $1) }
        return String(decoding: d, as: UTF8.self)
    }

    // MARK: online packed-view decode (§16.D4)

    /// Decode a server packed masked-view blob (player_views / spectator_views
    /// wire) into a GameView through the shared kernel — the same decode offline
    /// play uses, never a reimplemented wire. `viewer` is the local seat, or
    /// -1 for the spectator feed.
    public func viewFromPacked(_ bytes: Data, viewer: Int) throws -> GameView {
        let data = try bytes.withUnsafeBytes { raw -> Data in
            let base = raw.bindMemory(to: UInt8.self).baseAddress
            return try json { fio_view_from_packed_json(base, Int32(bytes.count), Int32(viewer), $0, $1) }
        }
        return try JSONDecoder().decode(GameView.self, from: data)
    }

    /// Legal moves for `seat` computed from a server packed masked-view blob —
    /// online enable-states, kernel-computed (§3).
    public func legalFromPacked(_ bytes: Data, seat: Int) throws -> [Move] {
        let data = try bytes.withUnsafeBytes { raw -> Data in
            let base = raw.bindMemory(to: UInt8.self).baseAddress
            return try json { fio_legal_from_packed_json(base, Int32(bytes.count), Int32(seat), $0, $1) }
        }
        return try JSONDecoder().decode([Move].self, from: data)
    }

    // MARK: replays (§7.3)

    /// Encode the CURRENT game's history to a shareable base32 code.
    public func replayEncodeCode() throws -> String {
        let d = try json { fio_replay_encode_b32($0, $1) }
        return String(decoding: d, as: UTF8.self)
    }

    /// Decode a shareable code to the decoded step list (does not touch the
    /// current game). Byte-parity with the server (shared replay.c).
    public func replayDecode(code: String) throws -> DecodedReplay {
        let data = try code.withCString { cstr -> Data in
            try json { fio_replay_decode_json(cstr, $0, $1) }
        }
        return try JSONDecoder().decode(DecodedReplay.self, from: data)
    }

    // MARK: typed decoders (convenience)

    public func state(viewer: Int) throws -> GameView {
        try JSONDecoder().decode(GameView.self, from: try stateData(viewer: viewer))
    }
    public func publicState() throws -> GameView {
        try JSONDecoder().decode(GameView.self, from: try publicStateData())
    }
    public func legalMoves(seat: Int) throws -> [Move] {
        try JSONDecoder().decode([Move].self, from: try legalMovesData(seat: seat))
    }

    // MARK: - plumbing

    /// Call a JSON-emitting C function into a growing buffer. On FIO_ECAP the
    /// buffer doubles and retries; any other negative code throws.
    private func json(_ call: (UnsafeMutablePointer<CChar>, Int32) -> Int32) throws -> Data {
        var cap = 16 * 1024
        while true {
            var buf = [CChar](repeating: 0, count: cap)
            let n = call(&buf, Int32(cap))
            if n >= 0 { return Data(bytes: buf, count: Int(n)) }
            if n == Self.eCap {
                cap *= 2
                if cap > (1 << 23) { throw EngineError.capacity }   // 8MB ceiling
                continue
            }
            try Self.check(n)
        }
    }

    /// Like `json`, but a return of 0 (no output) is a valid "nothing happened"
    /// result rather than an error. Writes the bytes into `out` and returns the
    /// raw code (0 = empty, >0 = wrote bytes).
    private func jsonAllowingEmpty(_ call: (UnsafeMutablePointer<CChar>, Int32) -> Int32,
                                   into out: inout Data?) throws -> Int32 {
        var cap = 16 * 1024
        while true {
            var buf = [CChar](repeating: 0, count: cap)
            let n = call(&buf, Int32(cap))
            if n == 0 { out = nil; return 0 }
            if n > 0 { out = Data(bytes: buf, count: Int(n)); return n }
            if n == Self.eCap {
                cap *= 2
                if cap > (1 << 23) { throw EngineError.capacity }
                continue
            }
            try Self.check(n)
        }
    }

    private static func check(_ code: Int32) throws {
        switch code {
        case let c where c >= 0: return
        case eBadArg:  throw EngineError.badArg
        case eNoGame:  throw EngineError.noGame
        case eCap:     throw EngineError.capacity
        case eParse:   throw EngineError.parse
        case eReject:  throw EngineError.reject(code: Int(fio_last_reject()))
        case eNoStrat: throw EngineError.noStrategy
        case eReplay:  throw EngineError.replay(code: Int(fio_last_replay_error()))
        default:       throw EngineError.unknown(Int(code))
        }
    }
}
