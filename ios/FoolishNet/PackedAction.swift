// PackedAction.swift — Milestone D scaffolding (§8.1, §16.D3). Byte-for-byte
// port of the web's packed action wire (server/impls/supabase/functions/_shared/
// packed_action.ts → wire/awire.ts). Every online move POSTs this binary body:
//   supabase.functions.invoke("action", body: <these bytes>)
// and the response is the 7-byte envelope decoded below.
//
// This is the ONE place binary crosses the language boundary on the wire (the
// other is the golden fixtures) — so it is pinned by golden vectors generated
// from the TS implementation (§16.D3, TODO: action_goldens.json). Until those
// land and the D milestone wires Net/ into an OnlineGame, this file is unused by
// the app; it is complete and unit-tested in isolation so D is a leaf addition.

import Foundation
import FoolishKit

public enum PackedAction {

    // Kind bytes (awire AWIRE_KIND). No `bump` exists in the wire.
    public enum Kind: UInt8 {
        case attack = 0, cover = 1, pass = 2, pickup = 3, good = 4
    }

    // Special card bytes (wire.h).
    public static let cardHidden: UInt8 = 0xFE   // {-1,-1}
    public static let cardNone: UInt8 = 0xFF
    public static let maxCards = 28              // AWIRE_MAX_CARDS

    // Request envelope format bytes.
    public static let reqFormatV2: UInt8 = 2     // current: carries intent version
    // Response envelope.
    public static let respFormat: UInt8 = 1

    public enum Status: UInt8 { case applied = 0, rejected = 1, moot = 2 }
    /// Edge-policy reject code that sits above the kernel's 0..21 (the stale-round
    /// guard from WEB_RACE_BUG_HANDOFF.md §5). Surfaced with its own localized copy.
    public static let rejectStaleRound: UInt8 = 100

    // MARK: card encoding

    /// suit*13 + (value-1) → 0..51, matching wire.h. Hidden cards → cardHidden.
    public static func encodeCard(_ c: Card) -> UInt8 {
        if c.isHidden { return cardHidden }
        return UInt8(c.s * 13 + (c.v - 1))
    }

    public static func decodeCard(_ b: UInt8) -> Card {
        if b == cardHidden { return .hidden }
        let v = Int(min(b, 51))
        return Card(s: v / 13, v: (v % 13) + 1)
    }

    // MARK: move → wire

    /// The move buffer: [kind][n][card×n], with cover appending [attackCard×n].
    /// Throws on structural violations the wire decoder would reject (§ awire
    /// decodeAction strictness): pickup/good must carry 0 cards; cover must have
    /// matching attackCards; n ≤ 28.
    public static func encode(_ move: Move) throws -> [UInt8] {
        let kind = try kind(for: move.type)
        let cards = move.cards
        if cards.count > maxCards { throw WireError.tooManyCards }

        switch kind {
        case .pickup, .good:
            guard cards.isEmpty else { throw WireError.mustBeZeroCard }
            return [kind.rawValue, 0]
        case .attack, .pass:
            var out: [UInt8] = [kind.rawValue, UInt8(cards.count)]
            out.append(contentsOf: cards.map(encodeCard))
            return out
        case .cover:
            let attacks = move.attackCards ?? []
            guard attacks.count == cards.count else { throw WireError.coverMismatch }
            var out: [UInt8] = [kind.rawValue, UInt8(cards.count)]
            out.append(contentsOf: cards.map(encodeCard))
            out.append(contentsOf: attacks.map(encodeCard))
            return out
        }
    }

    /// The full HTTP request body (format v2): [2][gid_len][gid][intentVersion:u32 LE][wire].
    /// `intentVersion` is the client's `games.version` intent — the stale-round
    /// guard compares it server-side (WEB_RACE_BUG_HANDOFF.md §5).
    public static func requestBody(gameId: String, intentVersion: UInt32, move: Move) throws -> Data {
        let gid = Array(gameId.utf8)
        guard gid.count <= 255 else { throw WireError.gameIdTooLong }
        var out: [UInt8] = [reqFormatV2, UInt8(gid.count)]
        out.append(contentsOf: gid)
        out.append(contentsOf: leU32(intentVersion))
        out.append(contentsOf: try encode(move))
        return Data(out)
    }

    // MARK: response envelope

    public struct Response: Equatable {
        public let status: Status
        public let rejectCode: UInt8
        public let version: UInt32
        public var isStaleRound: Bool { status == .rejected && rejectCode == PackedAction.rejectStaleRound }
    }

    /// Decode the 7-byte response [fmt=1][status][rejectCode][version:u32 LE].
    public static func decodeResponse(_ data: Data) throws -> Response {
        guard data.count >= 7 else { throw WireError.shortResponse }
        let b = [UInt8](data)
        guard b[0] == respFormat else { throw WireError.badResponseFormat }
        guard let status = Status(rawValue: b[1]) else { throw WireError.badStatus }
        let version = UInt32(b[3]) | (UInt32(b[4]) << 8) | (UInt32(b[5]) << 16) | (UInt32(b[6]) << 24)
        return Response(status: status, rejectCode: b[2], version: version)
    }

    // MARK: helpers

    private static func kind(for t: MoveType) throws -> Kind {
        switch t {
        case .attack: return .attack
        case .cover: return .cover
        case .pass: return .pass
        case .pickup: return .pickup
        case .good: return .good
        case .wait, .unknown: throw WireError.unsupportedMove
        }
    }

    private static func leU32(_ v: UInt32) -> [UInt8] {
        [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF), UInt8((v >> 16) & 0xFF), UInt8((v >> 24) & 0xFF)]
    }

    public enum WireError: Error, Equatable {
        case tooManyCards, mustBeZeroCard, coverMismatch, gameIdTooLong
        case unsupportedMove, shortResponse, badResponseFormat, badStatus
    }
}
