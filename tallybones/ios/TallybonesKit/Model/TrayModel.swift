// TrayModel.swift - the plain values every Tallybones screen draws from.
//
// NOTHING HERE DECIDES A RULE. These are the shapes the kernel's view fills
// (tb_view, DECISIONS T7: everybody sees everything, so there is no masked
// view): the five dice, the keep marks, which roll of the turn this is, whose
// turn it is, every seat's thirteen slots and total, the phase, and the
// kernel's verdicts on what this phone may do right now. The verdicts are
// fields, not functions, so no view ever works one out.
//
// KERNEL: the integration worker fills these from the generated readers
// (Generated/TallybonesKernel.swift) inside the bridge-backed TallyKernel;
// no view changes.

import Foundation

public enum TallyPhase: Equatable, Sendable {
    /// Seats are joining (pk_lobby, DECISIONS T2).
    case lobby
    /// A turn is under way and the player may still reroll (roll 1 or 2).
    case rolling
    /// The third roll is in: the player must score (T3).
    case choosing
    /// Every seat has filled all thirteen slots (T5).
    case over
}

/// The thirteen categories of T4, in card order: the numbers half, then the
/// combinations half. Only the ORDER lives here; the names are the kernel's
/// words (TallyKernel.categoryName) and the scores are the kernel's numbers.
public enum Category: Int, CaseIterable, Sendable, Identifiable {
    case ones, twos, threes, fours, fives, sixes
    case threeAlike, fourAlike, fullHouse, shortRun, longRun, tallybones, any

    public var id: Int { rawValue }
    /// The numbers half (T4): Ones to Sixes.
    public var isNumbers: Bool { rawValue <= Category.sixes.rawValue }

    public static let numbers: [Category] = allCases.filter { $0.isNumbers }
    public static let combinations: [Category] = allCases.filter { !$0.isNumbers }
    public static let count = 13
}

/// The five dice in the middle of the felt.
public struct TrayModel: Equatable, Sendable {
    public static let diceCount = 5

    /// 1...6, or 0 for UNKNOWN: a die that is being rerolled by a KEEP bubble
    /// that has not been sent yet (T11), or a turn that has not rolled.
    public var dice: [Int]
    /// The keep marks as the tray should show them.
    public var kept: [Bool]
    /// Which roll of the turn is on the tray, 1...3; 0 before any.
    public var roll: Int
    /// The seat whose turn it is, or nil (the lobby, the end).
    public var turn: Int?
    public var phase: TallyPhase

    // The kernel's verdicts for THIS phone (the tray never works them out):
    /// A tap on a die may toggle its keep mark.
    public var canKeep: Bool
    /// The Roll pill is live (for the marks the kernel was last asked about).
    public var canRoll: Bool
    /// A tap on an open row of my card may score the dice there.
    public var canScore: Bool

    public init(dice: [Int] = Array(repeating: 0, count: TrayModel.diceCount),
                kept: [Bool] = Array(repeating: false, count: TrayModel.diceCount),
                roll: Int = 0, turn: Int? = nil, phase: TallyPhase = .lobby,
                canKeep: Bool = false, canRoll: Bool = false, canScore: Bool = false) {
        self.dice = dice
        self.kept = kept
        self.roll = roll
        self.turn = turn
        self.phase = phase
        self.canKeep = canKeep
        self.canRoll = canRoll
        self.canScore = canScore
    }

    /// Every die has a value: nothing is waiting on a send.
    public var allKnown: Bool { dice.allSatisfy { $0 != 0 } }

    /// The keep marks as the 5-bit mask the KEEP move carries (T7), die 0 in
    /// bit 0. A representation, not a rule: which masks are moves is the
    /// kernel's.
    public static func mask(_ kept: [Bool]) -> Int {
        kept.enumerated().reduce(0) { $0 | ($1.element ? 1 << $1.offset : 0) }
    }
}

/// One seat's card: thirteen slots, the numbers-half sum, the bonus and the
/// total, all the kernel's numbers.
public struct CardModel: Equatable, Sendable, Identifiable {
    public var seat: Int
    public var name: String
    /// Indexed by Category.rawValue; nil = still open.
    public var slots: [Int?]
    /// The numbers half so far.
    public var numbersSum: Int
    /// The bonus once it is decided (35 or 0), nil while it still can go
    /// either way. The threshold is the kernel's (T4: 63).
    public var bonus: Int?
    public var total: Int

    public var id: Int { seat }

    public init(seat: Int, name: String, slots: [Int?] = Array(repeating: nil, count: Category.count),
                numbersSum: Int = 0, bonus: Int? = nil, total: Int = 0) {
        self.seat = seat
        self.name = name
        self.slots = slots
        self.numbersSum = numbersSum
        self.bonus = bonus
        self.total = total
    }

    public func score(_ c: Category) -> Int? { slots[c.rawValue] }
    public var filled: Int { slots.compactMap { $0 }.count }
}

/// The lobby, as the kernel offers it to this phone (pk_lobby_offered's
/// shape, copied into tb_lobby, T2).
public struct LobbyModel: Equatable, Sendable {
    public enum Offer: Equatable, Sendable { case none, join, start, invite, waiting, full }
    public var offer: Offer
    public var canLeave: Bool
    /// The numbered roster, one line per seat, in the kernel's words.
    public var rows: [String]
    /// Who starts, or nil.
    public var footnote: String?

    public init(offer: Offer = .none, canLeave: Bool = false, rows: [String] = [], footnote: String? = nil) {
        self.offer = offer
        self.canLeave = canLeave
        self.rows = rows
        self.footnote = footnote
    }
}

/// What is staged in the input field and not sent yet.
public enum DraftKind: Equatable, Sendable {
    case none
    /// A KEEP: the rerolling dice are unknown until it is sent (T11).
    case keep
    case score(Category)
    /// A join, a leave or a start.
    case lobby
}

/// Everything a screen reads, in one value: the kernel's view of the resident.
public struct TallyView: Equatable, Sendable {
    /// The resident is a game this build can read.
    public var readable: Bool
    /// My seat, or nil when this phone is not seated (a spectator).
    public var me: Int?
    public var names: [String]
    public var tray: TrayModel
    public var cards: [CardModel]
    public var lobby: LobbyModel
    public var draft: DraftKind
    /// The status line: "Bo to roll", "Alex wins with 241" (T8).
    public var headline: String
    /// "Roll 2 of 3", or "" when there is no roll to count.
    public var rollLine: String
    /// The one-line caption the bubble picture carries (T8, T11: a KEEP's
    /// names no rerolled value).
    public var caption: String

    public init(readable: Bool = false, me: Int? = nil, names: [String] = [], tray: TrayModel = TrayModel(),
                cards: [CardModel] = [], lobby: LobbyModel = LobbyModel(), draft: DraftKind = .none,
                headline: String = "", rollLine: String = "", caption: String = "") {
        self.readable = readable
        self.me = me
        self.names = names
        self.tray = tray
        self.cards = cards
        self.lobby = lobby
        self.draft = draft
        self.headline = headline
        self.rollLine = rollLine
        self.caption = caption
    }

    public var phase: TallyPhase { tray.phase }
    public var myCard: CardModel? { me.flatMap { m in cards.first { $0.seat == m } } }
}
