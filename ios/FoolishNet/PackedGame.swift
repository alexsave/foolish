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
//   [7..8]   u16 LE legacy roster JSON length (0 once the island is gone)
//   [9..]    legacy roster JSON
//   [q]      u16 LE view length      (q = 9 + rosterLen)
//   [q+2]    VIEW_FORMAT_VERSION = 1
//   [q+3]    viewer seat
//   [q+4..]  masked state (view.c state_put layout) — handed to the kernel
//   [q+viewLen..] packed roster (EnvelopeRoster), present iff flags bit1
//
// THE ROSTER IS PACKED NOW. It was the last JSON on any path that mattered, and
// it moved to bytes without a coordinated deploy: the server appends the packed
// roster AFTER the view blob and announces it in a flag bit, so build 1.0(43) -
// which reads bit0, ignores the rest of the flags byte, and bounds the view blob
// without ever looking past it - still sees exactly the payload it always saw.
// The JSON island below is the fallback for a stored player_views row written
// before that deploy; it dies in one commit with the server's LEGACY_ROSTER_JSON
// (sdk/ts/wire/view.ts). See docs/KERNEL_LIFT_BRIEF.md item 4.

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

    // ---- the legacy JSON island, and nothing else, lives below --------------
    // Only reachable for an envelope written before the trailer existed (a
    // stored player_views row for an idle game). Delete this, and the branch
    // that calls it, when the server stops writing the island.
    struct Roster: Decodable {
        let id: String
        let name: String
        let players: [RosterPlayer]
        let status: String
        let good_players: [String]?
        let good_timestamp: Double?
    }
    struct RosterPlayer: Decodable { let player_id: String; let name: String; let is_ai: Bool }

    /// The legacy island as an EnvelopeRoster, so the decoder below has one
    /// shape to read whichever way the bytes arrived.
    private static func legacyRoster(_ bytes: Data) -> EnvelopeRoster? {
        guard let r = try? JSONDecoder().decode(Roster.self, from: bytes) else { return nil }
        let status: Int
        switch r.status {
        case "waiting":   status = GameStatus.waiting.rawValue
        case "game_over": status = GameStatus.gameOver.rawValue
        default:          status = GameStatus.playing.rawValue
        }
        return EnvelopeRoster(
            id: r.id, name: r.name, status: status,
            players: r.players.map { EnvelopeRoster.Player(playerId: $0.player_id, name: $0.name, isAI: $0.is_ai) },
            goodPlayers: r.good_players ?? [], goodTimestamp: r.good_timestamp)
    }

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

        // THE ROSTER, IN BYTES. The trailer sits past the view blob; the JSON
        // island is read only when no trailer was announced.
        let roster: EnvelopeRoster
        if (b[1] & flagPackedRoster) != 0, let packed = EnvelopeRoster.decode(b, at: q + viewLen) {
            roster = packed.roster
        } else if let legacy = legacyRoster(Data(b[9..<(9 + rosterLen)])) {
            roster = legacy
        } else {
            return nil
        }

        // Decode the masked state directly in Swift — no kernel JSON round-trip
        // (owner: wipe the JSON; client↔server is packed kernel wire).
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
