// DecodedReplay.swift — Codable mirror of fio_replay_decode_json's output
// (cnitro/ios/ios_api.c). Matches the shape of the web's DecodedReplay
// (@shared/replay/core.ts): header + the log/event stream. Card values are the
// {s,v} form; hidden cards decode to {s:-1,v:-1}, and `target` is null for
// single-card pairs.

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
