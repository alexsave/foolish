// StandInKernel.swift - the local stand-in that lets the shell run with no
// kernel: a fixed hand of dice, a second seat that plays itself, and NO RULES.
//
// KERNEL: delete this file (and its uses in TallybonesHost.init and the
// tests' fixtures) once BridgeKernel exists. It is not a model of the game
// and must never become one: it knows two canned hands and their canned
// scores, and that is all.
//
// The canned hands are chosen so every keep-and-reroll lands on one of them:
// roll 1 is always H1, and a rerolled die takes H2's value in its slot. H1 and
// H2 differ only in die 3, so whatever is kept the tray shows H1 or H2, and
// the preview table below has both. Its numbers were worked out by hand from
// T4 and typed in; nothing here scores dice.
//
// What it does keep faithfully, because the screens must be built for it:
// T11. A staged KEEP shows its rerolling dice as 0 (blank) and derives
// nothing; the values appear at `commit()`, the send echo.

import Foundation

@MainActor
public final class StandInKernel: TallyKernel {

    /// Roll 1 of every turn: three threes, a five and a two.
    public static let h1 = [3, 3, 5, 2, 3]
    /// What a rerolled die becomes: a full house.
    public static let h2 = [3, 3, 5, 5, 3]

    /// The canned scores of the two canned hands, in Category order (T4 by
    /// hand: numbers are the sum of that number; alike and Any are the sum of
    /// all five; the full house is 25). nil for any other dice.
    public static func preview(_ dice: [Int]) -> [Int?] {
        switch dice {
        case h1: return [0, 2, 9, 0, 5, 0, 16, 0, 0, 0, 0, 0, 16]
        case h2: return [0, 0, 9, 0, 10, 0, 19, 0, 25, 0, 0, 0, 19]
        default: return Array(repeating: nil, count: Category.count)
        }
    }

    private enum Draft: Equatable { case none, keep(Int), score(Category), start }

    private let gameId: Int
    private var resident = false
    private var started = false
    private var names: [String] = []
    private let me = 0
    private var moves = 0
    private var draft: Draft = .none
    private var turn = 0
    private var roll = 0
    private var dice = Array(repeating: 0, count: TrayModel.diceCount)
    private var kept = Array(repeating: false, count: TrayModel.diceCount)
    private var slots: [[Int?]] = []
    private var nickname = ""

    /// `started`: skip the lobby, for previews and tests.
    public init(started: Bool = false, gameId: Int = 1) {
        self.gameId = gameId
        if started {
            _ = newGame(dm: true)
            startNow()
        }
    }

    // MARK: identity and records: nothing to keep

    public func identify(me participant: Data) {}
    public func sender(of text: String?, isDM: Bool, iSent: Bool) {}
    public func setNickname(_ name: String) { nickname = name }
    public func nameIsOK(_ name: String) -> Bool {
        let t = name.trimmingCharacters(in: .whitespaces)
        return !t.isEmpty && t.count <= 16            // pickemup's 16 (D23), the kernel's
    }
    public func loadSeats(_ bytes: Data?) {}
    public func seatsIfDirty() -> Data? { nil }

    // MARK: the resident

    public var layoutMatches: Bool { true }

    public var text: String? {
        guard resident else { return nil }
        return "tallybones-standin://g/\(gameId)/\(moves + (draft == .none ? 0 : 1))"
    }

    public var hasDraft: Bool { draft != .none }

    public func adopt(_ text: String, arrival: Bool) -> Int {
        // The stand-in cannot replay anybody's history: it accepts only its
        // own resident's link back (the send echo) and refuses the rest.
        if let mine = self.text, mine == text { return 0 }
        if resident, text == "tallybones-standin://g/\(gameId)/\(moves)" { return 0 }
        return -1
    }

    public func commit() {
        switch draft {
        case .none:
            return
        case .start:
            startNow()
        case .keep(let mask):
            roll += 1
            for i in 0..<TrayModel.diceCount where mask & (1 << i) == 0 { dice[i] = Self.h2[i] }
            kept = (0..<TrayModel.diceCount).map { mask & (1 << $0) != 0 }
        case .score(let c):
            slots[turn][c.rawValue] = Self.preview(dice)[c.rawValue] ?? 0
            nextTurn()
            // the second seat plays itself: its first open slot, H2's score
            if turn != me, !over, let open = slots[turn].firstIndex(where: { $0 == nil }) {
                slots[turn][open] = Self.preview(Self.h2)[open] ?? 0
                nextTurn()
            }
        }
        draft = .none
        moves += 1
    }

    public func cancelDraft() { draft = .none }

    public func newGame(dm: Bool) -> Bool {
        resident = true
        started = false
        names = [nickname.isEmpty ? "Alex" : nickname, "Bo"]
        slots = names.map { _ in Array(repeating: nil, count: Category.count) }
        moves = 0
        draft = .none
        return true
    }

    public func sameGame(_ a: String, _ b: String) -> Bool {
        let prefix = "tallybones-standin://g/\(gameId)/"
        return a.hasPrefix(prefix) && b.hasPrefix(prefix)
    }

    public func prefer(_ a: String, over b: String) -> Int {
        let n = { (s: String) in Int(s.split(separator: "/").last ?? "") ?? 0 }
        return n(b) - n(a)
    }

    // MARK: reads

    private var over: Bool { started && slots.allSatisfy { $0.allSatisfy { $0 != nil } } }
    private var myTurn: Bool { started && !over && turn == me }

    public func view() -> TallyView {
        guard resident else { return TallyView() }
        var v = TallyView(readable: true, me: me, names: names)
        v.lobby = LobbyModel(offer: started ? .none : .start, canLeave: false,
                             rows: names.enumerated().map { "\($0.offset + 1). \($0.element)" },
                             footnote: names.isEmpty ? nil : "\(names[0]) rolls first")
        var shownDice = dice
        var shownKept = kept
        switch draft {
        case .keep(let mask):
            // T11: the rerolling dice are unknown until the bubble is sent
            for i in 0..<TrayModel.diceCount where mask & (1 << i) == 0 { shownDice[i] = 0 }
            shownKept = (0..<TrayModel.diceCount).map { mask & (1 << $0) != 0 }
            v.draft = .keep
        case .score(let c):
            v.draft = .score(c)
        case .start:
            v.draft = .lobby
        case .none:
            v.draft = .none
        }
        let known = shownDice.allSatisfy { $0 != 0 }
        let keepDraft = v.draft == .keep
        v.tray = TrayModel(dice: started ? shownDice : Array(repeating: 0, count: TrayModel.diceCount),
                           kept: shownKept, roll: started ? roll : 0, turn: started && !over ? turn : nil,
                           phase: !started ? .lobby : over ? .over : roll >= 3 ? .choosing : .rolling,
                           canKeep: myTurn && known && roll < 3 && !keepDraft,
                           canRoll: myTurn && known && roll < 3 && !keepDraft,
                           canScore: myTurn && known && !keepDraft)
        v.cards = names.indices.map { seat in
            var s = slots[seat]
            if seat == me, case .score(let c) = draft { s[c.rawValue] = Self.preview(dice)[c.rawValue] ?? 0 }
            // STAND-IN: a plain sum. The bonus is never decided here: that is
            // the kernel's (T4), so it stays open.
            let numbers = Category.numbers.compactMap { s[$0.rawValue] }.reduce(0, +)
            return CardModel(seat: seat, name: names[seat], slots: s, numbersSum: numbers, bonus: nil,
                             total: s.compactMap { $0 }.reduce(0, +))
        }
        v.headline = headline(v)
        v.rollLine = started && !over && roll > 0 ? "Roll \(roll) of 3" : ""
        v.caption = caption(v, shownDice: shownDice)
        return v
    }

    private func headline(_ v: TallyView) -> String {
        guard started else { return "" }
        if over {
            let best = v.cards.max { $0.total < $1.total }
            return best.map { "\($0.name) wins with \($0.total)" } ?? ""
        }
        if v.draft == .keep { return "Send to roll" }
        return turn == me ? "Your roll" : "\(names[turn]) to roll"
    }

    private static let spelled = ["no dice", "one", "two", "three", "four", "all five"]

    private func caption(_ v: TallyView, shownDice: [Int]) -> String {
        switch draft {
        case .start:
            return "Tallybones: \(names.joined(separator: ", "))"
        case .keep(let mask):
            let keptValues = (0..<TrayModel.diceCount).filter { mask & (1 << $0) != 0 }.map { String(dice[$0]) }
            let rerolls = TrayModel.diceCount - keptValues.count
            if keptValues.isEmpty { return "\(names[me]) rerolls all five" }
            return "\(names[me]) keeps \(keptValues.joined(separator: ", ")) and rerolls \(Self.spelled[rerolls])"
        case .score(let c):
            let n = Self.preview(dice)[c.rawValue] ?? 0
            return "\(names[me]) scores \(categoryName(c)), \(n) points"
        case .none:
            return v.headline.isEmpty ? "Tallybones" : v.headline
        }
    }

    public func string(_ s: TallyString) -> String {
        switch s {
        case .lobbyTitle:    return "Tallybones"
        case .join:          return "Join"
        case .start:         return "Start"
        case .leave:         return "Leave"
        case .lobbyWaiting:  return "Waiting for the first roll"
        case .lobbyAlone:    return "Send this to invite the chat"
        case .lobbyFull:     return "The table is full"
        case .namePrompt:    return "Your name"
        case .unreadable:    return "Can't read this bubble"
        case .roll:          return "Roll"
        case .numbersSum:    return "Sum"
        case .bonus:         return "Bonus at 63"
        case .total:         return "Total"
        case .cardTitleMine: return "Your card"
        case .close:         return "Close"
        }
    }

    public func categoryName(_ c: Category) -> String {
        switch c {
        case .ones:       return "Ones"
        case .twos:       return "Twos"
        case .threes:     return "Threes"
        case .fours:      return "Fours"
        case .fives:      return "Fives"
        case .sixes:      return "Sixes"
        case .threeAlike: return "Three Alike"
        case .fourAlike:  return "Four Alike"
        case .fullHouse:  return "Full House"
        case .shortRun:   return "Short Run"
        case .longRun:    return "Long Run"
        case .tallybones: return "Tallybones"
        case .any:        return "Any"
        }
    }

    public func errorLine(_ code: Int) -> String {
        "This bubble needs the game kernel, which this build does not have yet."
    }

    public var inviteCaption: String { "Tallybones: sit down and roll" }

    // MARK: moves

    public func mayRoll(keeping mask: Int) -> Bool {
        let v = view()
        return v.tray.canRoll && mask >= 0 && mask < 31
    }

    public func roll(keeping mask: Int) -> TallyStage? {
        guard mayRoll(keeping: mask) else { return nil }
        draft = .keep(mask)
        return TallyStage(caption: view().caption, collapse: true)
    }

    public func score(_ c: Category) -> TallyStage? {
        guard view().tray.canScore, slots[me][c.rawValue] == nil else { return nil }
        draft = .score(c)
        return TallyStage(caption: view().caption, collapse: false)
    }

    public func join() -> TallyStage? { nil }
    public func leave() -> TallyStage? { nil }

    public func start() -> TallyStage? {
        guard resident, !started else { return nil }
        draft = .start
        return TallyStage(caption: view().caption, collapse: false)
    }

    // MARK: the canned table

    private func startNow() {
        started = true
        turn = 0
        beginTurn()
    }

    private func beginTurn() {
        roll = 1
        dice = Self.h1
        kept = Array(repeating: false, count: TrayModel.diceCount)
    }

    private func nextTurn() {
        turn = (turn + 1) % max(names.count, 1)
        beginTurn()
    }
}
