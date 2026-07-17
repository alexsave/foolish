// MessageEnvelope — the Swift face of FMSG (c/src/msg_wire.h).
//
// PURE MARSHALLING. Every question this type looks like it answers — which chain
// wins, whether a pending move survives, what is legal now — is answered in C
// and merely relayed here. That is not tidiness: Rule P decides which game every
// player SEES, so a phone disagreeing with a browser forks the game mid-bout.
// There is exactly one implementation, in msg_wire.c, and the same fixtures pin
// it through wasm (e2e/msg_wire.test.ts) and through libfoolish.a
// (FoolishTests/MessageEnvelopeTests). The M3 plan's Swift port of the
// concurrency model is cancelled — there is nothing to port.
//
// Base32 + the '1' text-version prefix are the URL layer (§4.3), and they live
// in Swift only because a URL is a Swift thing; the bytes underneath are the
// kernel's.
import Foundation
import CFoolish

public struct MessageJoin: Codable, Sendable, Equatable {
    public let seat: Int
    public let name: String
}

/// A decoded, VALIDATED payload. Holding one means the chain replayed cleanly
/// through the kernel and the game it describes is now the resident one.
public struct MessageEnvelope: Codable, Sendable, Equatable {
    public let phase: Int              // 0 WAITING · 1 ACCEPT · 2 LIVE · 3 FINISHED
    public let turn: Int               // atoms applied — Rule P's second key
    public let round: Int              // completed bouts — Rule P's first key
    public let nPlayers: Int
    public let lastActorSeat: Int
    public let gameId: String          // a u64: a String because JSON numbers are doubles
    public let parent8: String         // hex
    public let digest: String          // hex — SHA-256 of the payload; Rule P's tiebreak
    public let joins: [MessageJoin]

    enum CodingKeys: String, CodingKey {
        case phase, turn, round, joins, digest, parent8
        case nPlayers = "n_players"
        case lastActorSeat = "last_actor_seat"
        case gameId = "game_id"
    }

    public enum Failure: Error, Equatable {
        case notAFoolishLink        // wrong scheme/format version — not ours
        case damaged(code: Int)     // MSG_E* — the payload does not replay
    }

    /// The URL a bubble carries: https://foolish.cards/m/1<base32>. The leading
    /// '1' is the TEXT-level format version, so a link can be rejected before a
    /// single binary byte is decoded (§4.3).
    public static func payloadBytes(url: URL) throws -> Data {
        let seg = url.pathComponents.last ?? ""
        guard seg.first == "1" else { throw Failure.notAFoolishLink }
        guard let bytes = Base32.decode(String(seg.dropFirst())), !bytes.isEmpty else {
            throw Failure.notAFoolishLink
        }
        return bytes
    }

    /// The bubble's link for a payload: https://foolish.cards/m/1<base32>. The
    /// inverse of `payloadBytes` — the '1' is the §4.3 text-format version. Kept
    /// pure (no MSMessage) so the URL layer is testable without Apple's framework.
    public static func link(payload: Data) -> URL {
        URL(string: "https://foolish.cards/m/1" + Base32.encode(payload))!
    }

    /// The FINISHED bubble's link (§12): the shareable REPLAY code, not `/m/`, so
    /// a tap lands on the web replay page (Infinite Oracle) on any platform — the
    /// ecosystem funnel. `code` is `fio_replay_share_code_b32`'s output.
    public static func replayLink(code: String) -> URL {
        URL(string: "https://foolish.cards/" + code)!
    }

    /// Decode + validate + ADOPT: the chain is replayed through the kernel, so
    /// afterwards the engine's resident game IS this payload's game.
    public static func decode(url: URL, viewer: Int) async throws -> MessageEnvelope {
        try await decode(payload: try payloadBytes(url: url), viewer: viewer)
    }

    public static func decode(payload: Data, viewer: Int) async throws -> MessageEnvelope {
        try await MessageKernel.shared.decode(payload: payload, viewer: viewer)
    }

    /// Parse the kernel's packed envelope-metadata blob (fio_msg_decode_packed).
    /// Fixed layout: phase(1) n_players(1) last_actor_seat(1) round(1) turn(u16
    /// LE) game_id(u64 LE) parent8(8) digest(32) n_joins(1) then joins of
    /// {seat(1) name_len(1) name[]}. Returns nil if a field runs past the end.
    static func decode(packed d: Data) -> MessageEnvelope? {
        let b = [UInt8](d)
        let HDR = 55
        guard b.count >= HDR else { return nil }
        let phase = Int(b[0]); let nPlayers = Int(b[1]); let last = Int(b[2]); let round = Int(b[3])
        let turn = Int(b[4]) | (Int(b[5]) << 8)
        var gid: UInt64 = 0
        for i in 0..<8 { gid |= UInt64(b[6 + i]) << (8 * i) }
        let hex = { (r: Range<Int>) in b[r].map { String(format: "%02x", $0) }.joined() }
        let parent8 = hex(14..<22)
        let digest = hex(22..<54)
        let nJoins = Int(b[54])
        var joins: [MessageJoin] = []
        var q = HDR
        for _ in 0..<nJoins {
            guard q + 2 <= b.count else { return nil }
            let seat = Int(b[q]); let nl = Int(b[q + 1]); q += 2
            guard q + nl <= b.count else { return nil }
            let name = String(decoding: b[q..<q + nl], as: UTF8.self); q += nl
            joins.append(MessageJoin(seat: seat, name: name))
        }
        return MessageEnvelope(phase: phase, turn: turn, round: round, nPlayers: nPlayers,
                               lastActorSeat: last, gameId: String(gid),
                               parent8: parent8, digest: digest, joins: joins)
    }
}

/// Serialized access to the kernel's single static Game, same discipline as
/// EngineC (ios_api.h: not reentrant).
public actor MessageKernel {
    public static let shared = MessageKernel()
    private init() {}

    /// Decode + validate + ADOPT through the PACKED envelope wire — the metadata
    /// crosses as a fixed-layout blob, no JSON. The view is read separately
    /// (residentView) in this same actor. `viewer` no longer rides the decode
    /// (metadata is viewer-independent); it stays in the signature for the
    /// call sites that pass a seat, and is applied on the residentView read.
    public func decode(payload: Data, viewer: Int) throws -> MessageEnvelope {
        var out = [UInt8](repeating: 0, count: 4 * 1024)
        let n: Int32 = payload.withUnsafeBytes { raw in
            fio_msg_decode_packed(raw.bindMemory(to: UInt8.self).baseAddress,
                                  Int32(payload.count), &out, Int32(out.count))
        }
        guard n > 0 else { throw MessageEnvelope.Failure.damaged(code: Int(fio_last_msg_error())) }
        guard let env = MessageEnvelope.decode(packed: Data(out.prefix(Int(n)))) else {
            throw MessageEnvelope.Failure.damaged(code: -1)
        }
        return env
    }


    /// The masked board the last `decode` left resident, for `viewer` (or -1 for
    /// the public/spectator view the bubble snapshot needs). Same packed wire the
    /// app reads — decoded in this actor so it never races EngineC on the shared
    /// static Game. Returns nil if no game is resident or the buffer won't fit.
    public func residentView(viewer: Int) -> GameView? {
        guard let data = packedCall({ fio_state_packed(Int32(viewer), $0, $1) }) else { return nil }
        return MaskedView.decode(data, viewer: viewer)
    }

    /// The legal moves for `seat` on the resident game (kernel-computed). Empty
    /// on no game or none legal.
    public func residentLegal(seat: Int) -> [Move] {
        guard let data = packedCall({ fio_legal_packed(Int32(seat), $0, $1) }) else { return [] }
        return MoveWire.decode(data)
    }

    /// Call a packed-bytes-emitting C function into a growing buffer (mirrors
    /// EngineC's helper; FIO_ECAP == -3 doubles and retries).
    private func packedCall(_ call: (UnsafeMutablePointer<CChar>, Int32) -> Int32) -> Data? {
        var cap = 8 * 1024
        while true {
            var buf = [CChar](repeating: 0, count: cap)
            let n = call(&buf, Int32(cap))
            if n >= 0 { return Data(bytes: buf, count: Int(n)) }
            if n == -3 { cap *= 2; if cap > (1 << 20) { return nil }; continue }
            return nil
        }
    }

    /// Deal a fresh game as the resident one — the start of the send path (a new
    /// invite). `seed` MUST be 32 bytes (the wide ChaCha deal both devices
    /// reproduce); `seal` then reads that seed into the envelope header, so the
    /// recipient re-deals the identical stock. Throws on a bad seed / seat count.
    public func newGame(seed: Data, players: Int) throws {
        let rc = seed.withUnsafeBytes { raw -> Int32 in
            fio_new_game(raw.bindMemory(to: UInt8.self).baseAddress, Int32(seed.count), Int32(players))
        }
        guard rc == 0 else { throw MessageEnvelope.Failure.damaged(code: Int(rc)) }
    }

    /// Apply one action by `seat` to the resident (adopted) game — the LOCAL half
    /// of a turn, before `seal`. Same packed awire frame the app and server apply
    /// through, and the kernel is the only judge of legality: an illegal move
    /// throws and the resident game is untouched. Kept in THIS actor so a message
    /// turn never races EngineC on the shared static Game.
    public func apply(seat: Int, move: Move) throws {
        let awire = MoveWire.encodeAction(move)
        guard !awire.isEmpty else { throw MessageEnvelope.Failure.damaged(code: -1) }
        let rc = awire.withUnsafeBytes { raw in
            fio_apply_awire(Int32(seat), raw.bindMemory(to: UInt8.self).baseAddress, Int32(awire.count))
        }
        guard rc == 0 else { throw MessageEnvelope.Failure.damaged(code: Int(rc)) }
    }

    /// Seal the resident game — the send path, after the local player moved.
    /// The kernel derives turn/round from the body it writes, so a device cannot
    /// emit a payload it would itself reject.
    public func seal(phase: Int, lastActorSeat: Int, gameId: UInt64,
                    parent8: Data, joins: [MessageJoin]) throws -> Data {
        let joinsJSON = String(data: try JSONEncoder().encode(joins), encoding: .utf8) ?? "[]"
        var parent = [UInt8](repeating: 0, count: 8)
        parent.replaceSubrange(0..<min(8, parent8.count), with: parent8.prefix(8))
        var out = [UInt8](repeating: 0, count: 8 * 1024)
        let n = joinsJSON.withCString { jp in
            fio_msg_encode(Int32(phase), Int32(lastActorSeat), gameId, parent, jp,
                           &out, Int32(out.count))
        }
        guard n > 0 else { throw MessageEnvelope.Failure.damaged(code: Int(fio_last_msg_error())) }
        return Data(bytes: out, count: Int(n))
    }

    /// The best shareable REPLAY code for the resident (finished) game — the §12
    /// funnel code behind `replayLink`. v6 when the deal is re-derivable, else v5;
    /// the kernel chooses, not app code. nil if no game or it cannot encode.
    public func residentReplayCode() -> String? {
        guard let data = packedCall({ fio_replay_share_code_b32($0, $1) }), !data.isEmpty
        else { return nil }
        return String(decoding: data, as: UTF8.self)
    }

    /// Rule P (§7.2). <0 `a` wins, >0 `b`, 0 the same chain. Delivery order is
    /// never an input — two devices can transiently disagree about "newest".
    public func preferred(_ a: Data, _ b: Data) throws -> Int {
        let r: Int32 = a.withUnsafeBytes { ap in
            b.withUnsafeBytes { bp in
                fio_msg_rule_p(ap.bindMemory(to: UInt8.self).baseAddress, Int32(a.count),
                               bp.bindMemory(to: UInt8.self).baseAddress, Int32(b.count))
            }
        }
        if r < -1 { throw MessageEnvelope.Failure.damaged(code: Int(fio_last_msg_error())) }
        return Int(r)
    }

    public enum Rebase: Int, Sendable {
        case reapplied = 0          // applied to the resident game — that IS the rebase
        case discardedRoundEnded = 1
        case discardedIllegal = 2
    }

    /// Rule R (§7.4), one pending move, in ledger order.
    public func rebase(pendingRound: Int, seat: Int, moveJSON: String) throws -> Rebase {
        let r = moveJSON.withCString { fio_msg_rebase(Int32(pendingRound), Int32(seat), $0) }
        guard r >= 0, let v = Rebase(rawValue: Int(r)) else {
            throw MessageEnvelope.Failure.damaged(code: Int(r))
        }
        return v
    }
}

/// RFC 4648 base32, uppercase, no padding — the same alphabet the replay codec
/// and the /m/ route use (codec.ts). QR-alphanumeric-safe and URL-safe, which is
/// why the payload is base32 and not base64.
public enum Base32 {
    private static let A = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567")

    public static func encode(_ data: Data) -> String {
        var out = "", bits = 0, value = 0
        for b in data {
            value = (value << 8) | Int(b); bits += 8
            while bits >= 5 { out.append(A[(value >> (bits - 5)) & 31]); bits -= 5 }
        }
        if bits > 0 { out.append(A[(value << (5 - bits)) & 31]) }
        return out
    }

    public static func decode(_ s: String) -> Data? {
        var bits = 0, value = 0
        var out = Data()
        for ch in s.uppercased() {
            guard let idx = A.firstIndex(of: ch) else { continue }  // ignore stray chars
            value = (value << 5) | idx; bits += 5
            if bits >= 8 { out.append(UInt8((value >> (bits - 8)) & 0xff)); bits -= 8 }
        }
        return out
    }
}
