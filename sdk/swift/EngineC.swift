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
    @_spi(FoolishBots) public static let eOK: Int32 = 0
    @_spi(FoolishBots) public static let eBadArg: Int32 = -1
    @_spi(FoolishBots) public static let eNoGame: Int32 = -2
    @_spi(FoolishBots) public static let eCap: Int32 = -3
    @_spi(FoolishBots) public static let eParse: Int32 = -4
    @_spi(FoolishBots) public static let eReject: Int32 = -5
    @_spi(FoolishBots) public static let eNoStrat: Int32 = -6
    @_spi(FoolishBots) public static let eReplay: Int32 = -7

    // MARK: lifecycle

    /// Deal a fresh game from `seed`. 32+ bytes ⇒ the wide ChaCha deal (whole
    /// deal space, reproducible). All seats start human; assign bots after.
    public func newGame(seed: Data, players: Int) throws {
        let rc = seed.withUnsafeBytes { raw -> Int32 in
            fio_new_game(raw.bindMemory(to: UInt8.self).baseAddress, Int32(seed.count), Int32(players))
        }
        try Self.check(rc)
    }

    public func hasGame() -> Bool { fio_has_game() != 0 }

    // MARK: observation

    /// Raw PACKED masked-state bytes for `viewer` (view.c state_put) — the wire
    /// itself, undecoded. Used where the exact bytes matter (golden hashing),
    /// so nothing rides the JSON surface (§16.0 packed-wire rule).
    public func statePackedData(viewer: Int) throws -> Data {
        try json { fio_state_packed(Int32(viewer), $0, $1) }
    }
    /// Raw PACKED legal-move wire for `seat` (u32 count, then per move
    /// type/n/cards/attacks). Undecoded — for golden hashing.
    public func legalPackedData(seat: Int) throws -> Data {
        try json { fio_legal_packed(Int32(seat), $0, $1) }
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
        // Packed awire action, no JSON move (owner: wipe the JSON).
        let awire = MoveWire.encodeAction(move)
        let rc = awire.withUnsafeBytes { raw -> Int32 in
            fio_apply_awire(Int32(seat), raw.bindMemory(to: UInt8.self).baseAddress, Int32(awire.count))
        }
        if rc == Self.eReject { throw EngineError.reject(code: Int(fio_last_reject())) }
        try Self.check(rc)
    }

    /// The animation events of the LAST apply / bot drive. Not carried on the packed
    /// wire yet — the app doesn't consume them until the B4 animation lands, at
    /// which point they return as packed evwire. Empty for now (owner: wipe JSON).
    public func lastEvents(viewer: Int) throws -> [GameEvent] {
        []
    }

    // MARK: bots
    //
    // setSeatStrategy / botDrive / strategyCount / strategyName / roster are NOT
    // here: they are in the FoolishBots module (sdk/swift/bots/), because a
    // public method on this actor is an exported symbol of a dynamic framework,
    // and exporting one of those links the whole strategy ladder into anything
    // that links FoolishKit - the iMessage extension included. See
    // sdk/swift/bots/EngineC+Bots.swift.

    // MARK: online packed-view decode (§16.D4)

    // A server masked-view blob decodes to a GameView in pure Swift via
    // MaskedView.decode (see PackedGame) — the packed→JSON bridge that used to
    // live here (fio_view_from_packed_json) is gone with the JSON surface.

    /// Legal moves for `seat` computed from a server packed masked-view blob —
    /// online enable-states, kernel-computed (§3). The PACKED form is what the
    /// board actually passes on (the play rules take the menu as bytes, see
    /// PlayWire), so the decode is the caller's second step, not this one's.
    public func legalFromPackedData(_ bytes: Data, seat: Int) throws -> Data {
        try bytes.withUnsafeBytes { raw -> Data in
            let base = raw.bindMemory(to: UInt8.self).baseAddress
            return try json { fio_legal_from_packed(base, Int32(bytes.count), Int32(seat), $0, $1) }
        }
    }
    public func legalFromPacked(_ bytes: Data, seat: Int) throws -> [Move] {
        MoveWire.decode(try legalFromPackedData(bytes, seat: seat))
    }

    // MARK: replays (§7.3)

    /// Encode the CURRENT game's history to a shareable base32 code.
    ///
    /// The kernel picks the format: v6 (carrying every hidden card, so a viewer
    /// sees the real hands instead of a retrodiction) when the game's deal can
    /// be re-derived from its seed, else v5. That choice is deliberately not
    /// made here — the server makes the same one, and app code that duplicated
    /// it would be one more thing the watch and iMessage extension had to
    /// reimplement (docs/C_CORE_CONSOLIDATION.md F5/A4).
    public func replayEncodeCode() throws -> String {
        let d = try json { fio_replay_share_code_b32($0, $1) }
        return String(decoding: d, as: UTF8.self)
    }

    /// Decode a shareable code to the decoded step list (does not touch the
    /// current game). Byte-parity with the server (shared replay.c).
    /// Decode a shareable code to the step list, through the PACKED wire: the
    /// kernel hands back the raw replay DECODE binary and Swift parses it
    /// (DecodedReplay.decode) — no JSON crosses the boundary. Does not touch the
    /// current game. Byte-parity with the server (shared replay.c).
    public func replayDecode(code: String) throws -> DecodedReplay {
        let data = try code.withCString { cstr -> Data in
            try json { buf, cap in
                buf.withMemoryRebound(to: UInt8.self, capacity: Int(cap)) {
                    fio_replay_decode_packed(cstr, $0, cap)
                }
            }
        }
        guard let r = DecodedReplay.decode(packed: data) else { throw EngineError.unknown(-1) }
        return r
    }

    // MARK: typed decoders (convenience)

    public func state(viewer: Int) throws -> GameView {
        let data = try json { fio_state_packed(Int32(viewer), $0, $1) }
        guard let v = MaskedView.decode(data, viewer: viewer) else { throw EngineError.unknown(-1) }
        return v
    }
    public func publicState() throws -> GameView {
        // Spectator view: VIEW_SPECTATOR sentinel (-1) unmasks nothing.
        let data = try json { fio_state_packed(Int32(-1), $0, $1) }
        guard let v = MaskedView.decode(data, viewer: -1) else { throw EngineError.unknown(-1) }
        return v
    }
    public func legalMoves(seat: Int) throws -> [Move] {
        MoveWire.decode(try json { fio_legal_packed(Int32(seat), $0, $1) })
    }

    // MARK: - plumbing

    /// Call a buffer-filling C function into a growing buffer. On FIO_ECAP the
    /// buffer doubles and retries; any other negative code throws.
    ///
    /// @_spi rather than private because the BOT half of the bridge lives in a
    /// different module (FoolishBots, sdk/swift/bots/) so that naming a bot
    /// entry point does not link the strategy ladder into the iMessage
    /// extension - see c/ios/include-bots/ios_bots_api.h. An extension in
    /// another module cannot reach a private member, and this is plumbing
    /// rather than API, so it is exposed to that module and to nothing else.
    @_spi(FoolishBots) public func json(_ call: (UnsafeMutablePointer<CChar>, Int32) -> Int32) throws -> Data {
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

    @_spi(FoolishBots) public static func check(_ code: Int32) throws {
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
