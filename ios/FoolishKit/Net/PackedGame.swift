// PackedGame.swift — decodes the server's enveloped packed-game buffer (the
// `create` response body and the `player_views.view` hex column) into a GameView
// plus the game id, the local seat, and the version. This is the native mirror
// of the web's decodePackedGame (supabase/functions/_shared/wire/view.ts).
//
// The envelope wraps the masked-state blob with a small header; we parse that
// header in Swift (trivial) and hand the INNER masked state to the kernel
// (EngineC.viewFromPacked → view.c state_get, tested by `make ios-view-test`),
// then merge the roster's real player names in. No wire is reimplemented in
// Swift beyond this fixed-layout header (§3).
//
// Envelope layout (GAME_RESP_FORMAT = 1):
//   [0]      magic = 1
//   [1]      flags: bit0 = isPlayer
//   [2]      seat (when isPlayer), else -1
//   [3..6]   u32 LE version
//   [7..8]   u16 LE roster JSON length
//   [9..]    roster JSON (PackedGameRoster)
//   [q]      u16 LE view length      (q = 9 + rosterLen)
//   [q+2]    VIEW_FORMAT_VERSION = 1
//   [q+3]    viewer seat
//   [q+4..]  masked state (view.c state_put layout) — handed to the kernel

import Foundation

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

    struct Roster: Decodable {
        let id: String
        let name: String
        let players: [RosterPlayer]
        let status: String
        let good_players: [String]?
        let good_timestamp: Double?
    }
    struct RosterPlayer: Decodable { let player_id: String; let name: String; let is_ai: Bool }

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

        guard let roster = try? JSONDecoder().decode(Roster.self, from: Data(b[9..<(9 + rosterLen)])) else { return nil }
        var q = 9 + rosterLen
        let viewLen = Int(b[q]) | (Int(b[q + 1]) << 8)
        q += 2
        guard q + viewLen <= b.count, viewLen >= 2, b[q] == viewFormatVersion else { return nil }
        // masked state proper starts after the [fmt | viewer] header (2 bytes)
        let stateStart = q + 2
        let stateBytes = Data(b[stateStart..<(q + viewLen)])

        // Decode the masked state through the kernel (viewer = the local seat).
        guard let raw = try? await engine.viewFromPacked(stateBytes, viewer: seat) else { return nil }

        // Merge the roster's real names / is_ai (the masked state carries neither).
        let named = raw.players.map { p -> PlayerView in
            let name = roster.players.indices.contains(p.seat) ? roster.players[p.seat].name : p.name
            return PlayerView(seat: p.seat, name: name, status: p.status, handCount: p.handCount,
                              awaitingAttack: p.awaitingAttack, strategyKey: p.strategyKey, hand: p.hand)
        }
        let view = GameView(
            status: raw.status, numPlayers: raw.numPlayers, powerSuit: raw.powerSuit,
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
