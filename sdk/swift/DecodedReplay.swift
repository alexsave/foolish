// DecodedReplay.swift — the decoded replay: header + the log/event stream,
// matching the web's DecodedReplay (@shared/replay/core.ts). Card values are the
// {s,v} form; hidden cards decode to {s:-1,v:-1}, and `target` is null for
// single-card pairs. `decode(packed:)` parses the kernel's raw replay DECODE
// binary (fio_replay_decode_packed) — no JSON crosses the boundary (#17).

import Foundation

public struct DecodedReplay: Codable, Equatable, Sendable {
    public let version: Int
    public let nPlayers: Int
    /// Optional: a malformed/edge code can carry a null trump byte; valid
    /// replays always have a real trump.
    public let trump: Card?
    public let firstAttacker: Int
    /// Fool (loser) seat, or -1 for a stream that ends mid-game (v6).
    public let fool: Int
    public let discardCount: Int
    public let eliminationOrder: [Int]
    public let logs: [ReplayLog]

    public var isComplete: Bool { fool >= 0 }
    public var trumpSuit: Suit? { trump.flatMap { Suit(rawValue: $0.s) } }

    // A replay-wire card byte: 0xFF = none (nil), 0xFE = hidden ({-1,-1}), else a
    // clamped card id (mirrors ios_api.c j_wire_card).
    private static let cardNone: UInt8 = 0xFF
    private static let cardHidden: UInt8 = 0xFE
    private static func card(_ b: UInt8) -> Card? {
        if b == cardNone { return nil }
        if b == cardHidden { return Card(s: -1, v: -1) }
        let v = Int(min(b, 51))
        return Card(s: v / 13, v: (v % 13) + 1)
    }

    /// Parse the kernel's raw replay DECODE binary (fio_replay_decode_packed) —
    /// the same 20-byte header + log records fio_replay_decode_json walked into
    /// JSON. Returns nil if the buffer is short or a log record runs past the end.
    public static func decode(packed d: Data) -> DecodedReplay? {
        let b = [UInt8](d)
        let HDR = 20
        guard b.count >= HDR else { return nil }

        let version = Int(b[0])
        let nPlayers = Int(b[1])
        let trump = card(b[2])
        let firstAttacker = Int(b[3])
        let fool = b[4] == 0xFF ? -1 : Int(b[4])
        let discard = Int(b[5]) | (Int(b[6]) << 8)
        let nElim = Int(b[7])
        var elimination: [Int] = []
        for i in 0..<min(nElim, 8) where b[8 + i] != 0xFF { elimination.append(Int(b[8 + i])) }
        let nLogs = UInt32(b[16]) | (UInt32(b[17]) << 8) | (UInt32(b[18]) << 16) | (UInt32(b[19]) << 24)

        var logs: [ReplayLog] = []
        var q = HDR
        var li: UInt32 = 0
        while li < nLogs && q + 4 <= b.count {
            let type = Int(b[q]); let seat = b[q + 1] == 0xFF ? -1 : Int(b[q + 1])
            let defIdx = b[q + 2] == 0xFF ? -1 : Int(b[q + 2]); let nPairs = Int(b[q + 3])
            q += 4
            var pairs: [ReplayPair] = []
            var pr = 0
            while pr < nPairs && q + 2 <= b.count {
                // primary is always a real/hidden card; target is nil for a
                // single-card pair (an uncovered attack).
                let primary = card(b[q]) ?? Card(s: -1, v: -1)
                pairs.append(ReplayPair(primary: primary, target: card(b[q + 1])))
                q += 2; pr += 1
            }
            logs.append(ReplayLog(type: type, seat: seat, defenderIndex: defIdx, pairs: pairs))
            li += 1
        }

        return DecodedReplay(version: version, nPlayers: nPlayers, trump: trump,
                             firstAttacker: firstAttacker, fool: fool, discardCount: discard,
                             eliminationOrder: elimination, logs: logs)
    }
}

public struct ReplayLog: Codable, Equatable, Sendable {
    /// LOG_* type from game.h (0 game_start … 9 draw).
    public let type: Int
    /// Acting seat, or -1 for a system event.
    public let seat: Int
    /// Defender index for defender-change events, else -1.
    public let defenderIndex: Int
    public let pairs: [ReplayPair]
}

public struct ReplayPair: Codable, Equatable, Sendable {
    public let primary: Card
    /// The covered attack card for COVER pairs; null otherwise.
    public let target: Card?
}
