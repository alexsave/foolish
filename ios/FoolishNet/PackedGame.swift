// PackedGame.swift — decodes the server's enveloped packed-game buffer (the
// `create` response body and the `player_views.view` hex column) into a GameView
// plus the game id, the local seat, and the version. This is the native mirror
// of the web's decodePackedGame (sdk/ts/wire/view.ts).
//
// The envelope wraps the masked-state blob with a small header; we parse that
// header in Swift (trivial) and decode the INNER masked state with
// MaskedView.decode (the same packed state_put wire the offline view reads),
// then merge the roster's real player names in. No wire is reimplemented in
// Swift beyond this fixed-layout header (§3).
//
// Envelope layout (GAME_RESP_FORMAT = 1):
//   [0]      magic = 1
//   [1]      flags: bit0 = isPlayer, bit1 = a PACKED roster trailer follows
//   [2]      seat (when isPlayer), else -1
//   [3..6]   u32 LE version
//   [7..8]   u16 LE roster_len, always 0 (see below)
//   [q]      u16 LE view length      (q = 9 + rosterLen)
//   [q+2]    VIEW_FORMAT_VERSION = 1
//   [q+3]    viewer seat
//   [q+4..]  masked state (view.c state_put layout) — handed to the kernel
//   [q+viewLen..] packed roster (EnvelopeRoster), REQUIRED
//
// WHY roster_len IS ALWAYS ZERO. It used to be the length of a JSON roster
// island sitting at byte 9, and that island moved to bytes without a
// coordinated deploy: the server appended the packed roster AFTER the view blob
// and announced it in a flag bit, so build 1.0(43) - which reads bit0, ignores
// the rest of the flags byte, and bounds the view blob without ever looking
// past it - kept seeing exactly the payload it always saw while the field caught
// up. The island is no longer written, so the field is now a zero that keeps
// every later offset where it was. An envelope with no trailer does not decode.
// See docs/KERNEL_LIFT_BRIEF.md item 4.

import Foundation
import FoolishKit

public struct DecodedGame: Sendable {
    public let view: GameView
    public let gameId: String
    public let seat: Int
    public let version: Int
    /// The inner masked-state bytes (for kernel legal-move computation).
    public let stateBytes: Data
}

public enum PackedGame {
    private static let magic: UInt8 = 1            // GAME_RESP_FORMAT
    private static let viewFormatVersion: UInt8 = 1 // VIEW_FORMAT_VERSION

    /// flags bit1 — the packed roster trailer is present.
    private static let flagPackedRoster: UInt8 = 2

    // (A JSON roster island used to sit at byte 9 and be decoded here with a
    // Codable struct, for envelopes written before the packed trailer existed.
    // The server does not write it any more - roster_len is 0 - so an envelope
    // without a trailer is unreadable rather than JSON-parsed, exactly as the
    // web's decodePackedGame now treats it.)

    /// Decode an enveloped packed-game buffer. Returns nil on a malformed/short
    /// payload (the caller treats it as unreadable, like the web).
    public static func decode(_ buf: Data, engine: EngineC) async -> DecodedGame? {
        let b = [UInt8](buf)
        guard b.count >= 11, b[0] == magic else { return nil }
        let isPlayer = (b[1] & 1) != 0
        let seat = isPlayer ? Int(b[2]) : -1
        let version = Int(UInt32(b[3]) | (UInt32(b[4]) << 8) | (UInt32(b[5]) << 16) | (UInt32(b[6]) << 24))
        let rosterLen = Int(b[7]) | (Int(b[8]) << 8)
        guard 9 + rosterLen + 2 <= b.count else { return nil }

        var q = 9 + rosterLen
        let viewLen = Int(b[q]) | (Int(b[q + 1]) << 8)
        q += 2
        guard q + viewLen <= b.count, viewLen >= 2, b[q] == viewFormatVersion else { return nil }
        // masked state proper starts after the [fmt | viewer] header (2 bytes)
        let stateStart = q + 2
        let stateBytes = Data(b[stateStart..<(q + viewLen)])

        // THE ROSTER, IN BYTES: the trailer past the view blob, and nothing
        // else. An envelope that announces no trailer was written before the
        // trailer existed and is unreadable - a table with no names is worse
        // than a load error, because the caller cannot tell it went wrong.
        guard (b[1] & flagPackedRoster) != 0,
              let packed = EnvelopeRoster.decode(b, at: q + viewLen) else { return nil }
        let roster = packed.roster

        // Decode the masked state directly in Swift - client and server are
        // kernel-to-kernel and the bytes are the contract.
        guard let raw = MaskedView.decode(stateBytes, viewer: seat) else { return nil }

        // Merge the roster's real names / is_ai (the masked state carries neither).
        let named = raw.players.map { p -> PlayerView in
            let name = roster.players.indices.contains(p.seat) ? roster.players[p.seat].name : p.name
            return PlayerView(seat: p.seat, name: name, status: p.status, handCount: p.handCount,
                              awaitingAttack: p.awaitingAttack, strategyKey: p.strategyKey, hand: p.hand)
        }
        // games.status (carried in the roster) is column-authoritative over the
        // blob's copy — the same rule the web applies (view.ts decodePackedGame).
        // Without it a WAITING lobby's blob decodes as a playing board.
        let view = GameView(
            status: roster.status, numPlayers: raw.numPlayers, powerSuit: raw.powerSuit,
            deckCount: raw.deckCount, discardCount: raw.discardCount, hasFlipped: raw.hasFlipped,
            firstAttacker: raw.firstAttacker, defender: raw.defender, viewer: raw.viewer,
            goodMask: raw.goodMask, gameOver: raw.gameOver, flipped: raw.flipped,
            battles: raw.battles, eliminationOrder: raw.eliminationOrder, players: named
        )
        return DecodedGame(view: view, gameId: roster.id, seat: seat, version: version, stateBytes: stateBytes)
    }

    /// Decode from the bare-hex `player_views.view` column string.
    public static func decodeHex(_ hex: String, engine: EngineC) async -> DecodedGame? {
        guard let data = hexToData(hex) else { return nil }
        return await decode(data, engine: engine)
    }

    /// Bare-hex (optional `\x` / `0x` prefix) → bytes. Nonisolated so the async
    /// decoders can call it off the main actor.
    public static func hexToData(_ hex: String) -> Data? {
        var s = hex
        if s.hasPrefix("\\x") { s = String(s.dropFirst(2)) }
        if s.hasPrefix("0x") { s = String(s.dropFirst(2)) }
        guard s.count % 2 == 0 else { return nil }
        var out = Data(capacity: s.count / 2)
        var idx = s.startIndex
        while idx < s.endIndex {
            let next = s.index(idx, offsetBy: 2)
            guard let b = UInt8(s[idx..<next], radix: 16) else { return nil }
            out.append(b)
            idx = next
        }
        return out
    }
}
