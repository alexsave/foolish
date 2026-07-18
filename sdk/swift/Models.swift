// Models.swift — Codable Swift mirrors of the JSON the C bridge emits
// (c/ios/ios_api.c). These are the ONLY representation of game state in
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
    /// Face-value label for a card. The kernel `value` is a rank INDEX, not the
    /// face number: value 1='2' … 9='10', 10='J', 11='Q', 12='K', 13='A' — the
    /// exact web VALUE_MAP (src/utils/cards.ts / server/api/core/constants.ts). The
    /// 36-card deck (min_value_for 2p = 5) therefore runs value 5..13 = 6..A, so a
    /// value 5 is a SIX, not a five (that off-by-mapping was the "5♦" bug).
    public static func label(_ value: Int) -> String {
        switch value {
        case 13: return "A"
        case 12: return "K"
        case 11: return "Q"
        case 10: return "J"
        case 9:  return "10"
        default: return String(value + 1)   // 1='2' … 8='9'
        }
    }

    /// Spoken rank for VoiceOver (ace/king/…/ten/nine).
    public static func spoken(_ value: Int) -> String {
        switch value {
        case 13: return "ace"
        case 12: return "king"
        case 11: return "queen"
        case 10: return "jack"
        case 9:  return "ten"
        default: return String(value + 1)
        }
    }
}

// MARK: - Views (masked per-viewer game state)

public struct BattleView: Codable, Equatable, Sendable {
    public let attack: Card
    public let defense: Card?    // nil when the attack is still uncovered

    // A synthesized memberwise init is internal, so FoolishApp (a separate
    // module) could not build one — which is why GalleryView's previews did not
    // compile. Decoding a kernel view never needs this; previews do.
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

    // Explicit, because the implicit memberwise init is internal and FoolishNet
    // is a separate module since the FoolishKit/FoolishNet split.
    public init(seat: Int, name: String, status: Int, handCount: Int,
                awaitingAttack: Bool, strategyKey: Int, hand: [Card]?) {
        self.seat = seat
        self.name = name
        self.status = status
        self.handCount = handCount
        self.awaitingAttack = awaitingAttack
        self.strategyKey = strategyKey
        self.hand = hand
    }
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

    // Explicit, because the implicit memberwise init is internal and FoolishNet
    // is a separate module since the FoolishKit/FoolishNet split.
    public init(status: Int, numPlayers: Int, powerSuit: Int, deckCount: Int,
                discardCount: Int, hasFlipped: Bool, firstAttacker: Int, defender: Int,
                viewer: Int, goodMask: Int, gameOver: Int, flipped: Card?,
                battles: [BattleView], eliminationOrder: [Int], players: [PlayerView]) {
        self.status = status
        self.numPlayers = numPlayers
        self.powerSuit = powerSuit
        self.deckCount = deckCount
        self.discardCount = discardCount
        self.hasFlipped = hasFlipped
        self.firstAttacker = firstAttacker
        self.defender = defender
        self.viewer = viewer
        self.goodMask = goodMask
        self.gameOver = gameOver
        self.flipped = flipped
        self.battles = battles
        self.eliminationOrder = eliminationOrder
        self.players = players
    }

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
/// move a bot just made (fio_bot_drive_packed result).
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

/// What a move is worth pausing for — the kernel's classification
/// (BOT_PACE_* in c/src/bot_drive.h). The app never turns these into
/// milliseconds itself: `BotDrive.delayMs` already carries the kernel's answer.
public enum PacingClass: Int, Codable, Sendable {
    case none = 0
    /// A silent action folded into this cycle — nothing to watch, no delay.
    case bundledPassive = 1
    /// A visible move — cards changed hands.
    case move = 2
    /// The bout resolved (discard / pickup / new defender).
    case roundTransition = 3
}

/// Why the kernel's bot cycle stopped (BOT_STOP_* in bot_drive.h).
public enum BotStop: Int, Codable, Sendable {
    /// No bot can act — a human's move is owed.
    case noEligible = 0
    case ended = 1
    /// Something visible landed; render it, wait `delayMs`, then drive again.
    case events = 2
    /// Hit the per-cycle action cap; call again to continue.
    case maxActions = 3
}

/// One action the kernel's bot cycle applied.
public struct BotAction: Codable, Equatable, Sendable {
    public let seat: Int
    public let type: MoveType
    public let cards: [Card]
    public let attackCards: [Card]?
    /// Raw pacing class; use `pacing`.
    public let pace: Int

    public var pacing: PacingClass { PacingClass(rawValue: pace) ?? .move }
    public var move: Move { Move(type: type, cards: cards, attackCards: attackCards, seat: seat) }
}

/// Where a card came from / went to (EVW_LOC_* in c/src/evwire.h).
public enum EventLoc: Int, Codable, Sendable {
    case deck = 0, hand = 1, table = 2, discard = 3, flipped = 4
    case none = 0xFF
}

/// What happened (EVW_T_* in c/src/evwire.h).
public enum EventType: Int, Codable, Sendable {
    case magicTransition = 0
    case deal = 1
    case flipped = 2
    case defenderMove = 3
    case attackPass = 4
    case cover = 5
    case pickup = 6
    case discard = 7
    case out = 8
    case refill = 9
    case cardsToTrash = 10
}

/// One animation event — the kernel's answer to "which card flies where".
///
/// This is the SAME stream the website plays (it decodes evwire and renders it;
/// it has never derived animations either). The board consumes these with
/// `matchedGeometryEffect`; it must never diff two `GameView`s to work out what
/// moved. `BoardDiff.swift` is cancelled for exactly this reason —
/// docs/C_CORE_CONSOLIDATION.md F4, docs/IOS_APP_DESIGN.md §16.B4.
public struct GameEvent: Codable, Equatable, Sendable {
    /// Raw code; use `kind`.
    public let type: Int
    /// The event's player seat, or -1.
    public let seat: Int
    /// Message template id (EVW_MSG_*). The UI renders its own copy.
    public let msg: Int
    /// Raw codes; use `fromLoc` / `toLoc`.
    public let from: Int
    public let to: Int
    /// `nil` entries are cards the kernel redacted — dealt/drawn into a hand
    /// that is not the viewer's. Render a card back; the identity never arrived.
    public let cards: [Card?]
    /// Cover only: the attack card being covered.
    public let target: Card?
    /// Cover only: which battle.
    public let battle: Int?
    /// The board AS OF this step, already masked for this viewer — commit it
    /// when this event's animation lands, exactly as the website commits the
    /// same per-event snapshot off the wire. It is what makes a multi-action
    /// cycle playable step by step instead of only at its final state, and why
    /// the board never has to work out the intermediate boards for itself.
    public let state: GameView?

    public var kind: EventType? { EventType(rawValue: type) }
    public var fromLoc: EventLoc { EventLoc(rawValue: from) ?? .none }
    public var toLoc: EventLoc { EventLoc(rawValue: to) ?? .none }
    /// nil seat for the "no particular player" events (discard, magic).
    public var actorSeat: Int? { seat >= 0 ? seat : nil }
}

/// One turn of the kernel's bot cycle (fio_bot_drive_packed).
///
/// The cycle applies 0..n actions and stops on the same conditions as the
/// website's loop; silent actions bundle rather than costing a delay each.
/// Everything here is the kernel's answer — the app decides only how to wait
/// and how to draw (docs/C_CORE_CONSOLIDATION.md §5).
public struct BotDrive: Codable, Sendable {
    public let actions: [BotAction]
    /// The cycle's animation plan, from the kernel — play these, don't diff.
    public let events: [GameEvent]
    /// Raw stop reason; use `stop`.
    public let stopRaw: Int
    /// Loser seat once the game is over, else -1.
    public let ended: Int
    /// How long to wait before driving again — from the ONE pacing table.
    public let delayMs: Int

    public var stop: BotStop { BotStop(rawValue: stopRaw) ?? .noEligible }
    public var isOver: Bool { ended >= 0 }
    /// The last visible action, for the board to animate.
    public var lastVisible: BotAction? { actions.last { $0.pacing != .bundledPassive } }

    private enum CodingKeys: String, CodingKey {
        case actions, events, ended, delayMs
        case stopRaw = "stop"
    }
}
