// Models.swift — Codable Swift mirrors of the JSON the C bridge emits
// (cnitro/ios/ios_api.c). These are the ONLY representation of game state in
// the app; there is no second, Swift-computed copy of the rules (§3). Field
// names match the bridge's JSON exactly — change one, change the other.
//
// Card uses the compact {s,v} shape from the design doc (§16.A2). The web's
// PersonalGame wire (suit/value, string statuses) is decoded separately in
// Net/ for online play (§8); this file is the offline/engine-facing view.

import Foundation

// MARK: - Card

/// One playing card. suit 0..3 (spades, hearts, clubs, diamonds), value 1..13
/// (Ace = 13). A redacted / face-down card is {s:-1, v:-1}.
public struct Card: Codable, Equatable, Hashable, Sendable {
    public let s: Int
    public let v: Int

    public init(s: Int, v: Int) { self.s = s; self.v = v }

    public static let hidden = Card(s: -1, v: -1)
    public var isHidden: Bool { s < 0 || v < 0 }

    /// Stable identity used as the `matchedGeometryEffect` id for animation
    /// (BoardDiff, §16.B4). Real cards animate by identity; hidden cards get
    /// synthetic slot ids assigned by the renderer, never this.
    public var identity: String { "\(s)-\(v)" }

    public var suit: Suit? { Suit(rawValue: s) }
}

public enum Suit: Int, CaseIterable, Sendable {
    case spades = 0, hearts = 1, clubs = 2, diamonds = 3

    public var isRed: Bool { self == .hearts || self == .diamonds }
    /// Card-face glyph. Kept here (presentation of a suit) — not a game rule.
    public var glyph: String {
        switch self {
        case .spades: return "♠"
        case .hearts: return "♥"
        case .clubs: return "♣"
        case .diamonds: return "♦"
        }
    }
}

public enum CardRank {
    /// Face-value label for a card (6..10, J, Q, K, A). Ace = 13.
    public static func label(_ value: Int) -> String {
        switch value {
        case 13: return "A"
        case 12: return "K"
        case 11: return "Q"
        case 10: return "10"
        case 1: return "A" // never in a 36-card deck; large deck uses 1 = Ace-low unused
        default: return String(value)
        }
    }
}

// MARK: - Views (masked per-viewer game state)

public struct BattleView: Codable, Equatable, Sendable {
    public let attack: Card
    public let defense: Card?    // nil when the attack is still uncovered

    public init(attack: Card, defense: Card?) {
        self.attack = attack
        self.defense = defense
    }
}

/// Game / player status integers mirror game.h (GAME_STATUS_*, PLAYER_STATUS_*).
public enum GameStatus: Int, Sendable { case waiting = 0, playing = 1, gameOver = 2 }
public enum SeatStatus: Int, Sendable { case idle = 0, ready = 1, `in` = 2, out = 3 }

public struct PlayerView: Codable, Equatable, Identifiable, Sendable {
    public let seat: Int
    public let name: String
    public let status: Int
    public let handCount: Int
    public let awaitingAttack: Bool
    public let strategyKey: Int
    /// Real cards only for the viewer seat; nil for every other seat (render
    /// `handCount` card backs). This is the "you only see your own hand" rule
    /// enforced in the kernel (view.c), not here.
    public let hand: [Card]?

    public var id: Int { seat }
    public var seatStatus: SeatStatus { SeatStatus(rawValue: status) ?? .idle }
    public var isOut: Bool { seatStatus == .out }
}

public struct GameView: Codable, Equatable, Sendable {
    public let status: Int
    public let numPlayers: Int
    public let powerSuit: Int
    public let deckCount: Int
    public let discardCount: Int
    public let hasFlipped: Bool
    public let firstAttacker: Int
    public let defender: Int
    public let viewer: Int
    public let goodMask: Int
    /// Fool (loser) seat once the game is over, else -1.
    public let gameOver: Int
    public let flipped: Card?
    public let battles: [BattleView]
    public let eliminationOrder: [Int]
    public let players: [PlayerView]

    public var gameStatus: GameStatus { GameStatus(rawValue: status) ?? .waiting }
    public var isOver: Bool { gameOver >= 0 }
    public var trumpSuit: Suit? { Suit(rawValue: powerSuit) }

    public func player(_ seat: Int) -> PlayerView? { players.first { $0.seat == seat } }
    public var me: PlayerView? { player(viewer) }
    public func hasSaidGood(_ seat: Int) -> Bool { (goodMask & (1 << seat)) != 0 }
}

// MARK: - Moves

public enum MoveType: String, Codable, Sendable {
    case attack, cover, pass, pickup, good, wait, unknown
}

/// One move — a legal-move menu entry, an intent to apply, or (with `seat`) the
/// move a bot just made (fio_bot_step_json result).
public struct Move: Codable, Equatable, Sendable {
    public let type: MoveType
    public let cards: [Card]
    /// Cover only: the attack cards being covered, positionally paired with `cards`.
    public let attackCards: [Card]?
    /// Present only on bot-step results — the seat that acted.
    public let seat: Int?

    public init(type: MoveType, cards: [Card] = [], attackCards: [Card]? = nil, seat: Int? = nil) {
        self.type = type
        self.cards = cards
        self.attackCards = attackCards
        self.seat = seat
    }

    public static let pickup = Move(type: .pickup)
    public static let good = Move(type: .good)

    /// Compact JSON for fio_apply_json. Synthesized Codable omits nil optionals
    /// (encodeIfPresent), so `attackCards`/`seat` are absent unless set — exactly
    /// what the C parser (find_key) expects.
    public func jsonString() -> String {
        let enc = JSONEncoder()
        guard let data = try? enc.encode(self), let s = String(data: data, encoding: .utf8) else {
            return "{\"type\":\"\(type.rawValue)\",\"cards\":[]}"
        }
        return s
    }
}
