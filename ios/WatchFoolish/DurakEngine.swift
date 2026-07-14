// DurakEngine.swift — a compact, self-contained HEADS-UP Durak (you vs one bot)
// so the "Bot game" actually plays, to show the design in motion. This is a
// warm-up stand-in; the real app links FoolishKit's kernel (WATCHOS_APP_PLAN §6).
// Simplifications vs full Durak: strict alternation (one uncovered attack at a
// time), no перевод/pass. Rules that matter — beating, trump, throw-in by rank,
// pickup, draw-to-6, fool detection — are real.

import Foundation

struct DurakEngine {
    enum Who { case you, bot }
    enum Step { case attack, defend }

    var deck: [Card] = []            // draw pile; last element (the trump card) is drawn last
    var trumpCard: Card
    var trump: Suit { trumpCard.suit }
    var you: [Card] = []
    var bot: [Card] = []
    var table: [Battle] = []
    var attacker: Who = .you
    var step: Step = .attack
    var discardCount = 0
    var fool: String?                // "You" / "Bot" once the game ends

    /// Whose turn it is to act right now.
    var toAct: Who { step == .attack ? attacker : (attacker == .you ? .bot : .you) }

    // MARK: setup

    static func newGame() -> DurakEngine {
        var full: [Card] = []
        for s in Suit.allCases { for v in 6...14 { full.append(Card(suit: s, value: v)) } }
        full.shuffle()
        let trumpCard = full.removeFirst()          // set aside as the trump; drawn last
        var e = DurakEngine(deck: full, trumpCard: trumpCard)
        for _ in 0..<6 {
            if let c = e.draw() { e.you.append(c) }
            if let c = e.draw() { e.bot.append(c) }
        }
        e.deck.append(trumpCard)                    // trump card sits at the bottom
        e.sortHands()
        // Lowest trump holder attacks first.
        let youLow = e.you.filter { $0.suit == e.trump }.map(\.value).min()
        let botLow = e.bot.filter { $0.suit == e.trump }.map(\.value).min()
        e.attacker = (botLow ?? 99) < (youLow ?? 99) ? .bot : .you
        if e.attacker == .bot { e.runBot() }
        return e
    }

    private mutating func draw() -> Card? { deck.isEmpty ? nil : deck.removeFirst() }
    private mutating func sortHands() {
        let t = trump    // local so the closure doesn't capture `self` while sorting
        let order: (Card, Card) -> Bool = { a, b in
            let at = a.suit == t, bt = b.suit == t
            if at != bt { return !at }              // trumps sort last
            if a.suit != b.suit { return a.suit.rawValue < b.suit.rawValue }
            return a.value < b.value
        }
        let sorted1 = you.sorted(by: order); you = sorted1
        let sorted2 = bot.sorted(by: order); bot = sorted2
    }

    // MARK: rules

    func beats(_ cover: Card, _ attack: Card) -> Bool {
        if cover.suit == attack.suit { return cover.value > attack.value }
        return cover.suit == trump && attack.suit != trump
    }
    private var tableRanks: Set<Int> { Set(table.flatMap { [$0.attack.value] + ($0.cover.map { [$0.value] } ?? []) }) }
    private var uncoveredIndex: Int? { table.firstIndex { $0.cover == nil } }
    private var allCovered: Bool { !table.isEmpty && table.allSatisfy { $0.cover != nil } }

    /// Can `card` be added as an attack right now (by whoever is attacking)?
    func legalAttack(_ card: Card, hand: [Card]) -> Bool {
        guard step == .attack, allCovered || table.isEmpty, table.count < 6, hand.contains(card) else { return false }
        return table.isEmpty || tableRanks.contains(card.value)
    }

    // MARK: your moves (menu for the Action screen)

    func youLegal(selected: Card?) -> [WMove] {
        guard fool == nil, toAct == .you else { return [] }
        if attacker == .you {          // you attack
            if let c = selected, legalAttack(c, hand: you) { return [.attack] }
            if allCovered { return [.done] }
            return []
        } else {                        // you defend
            guard let i = uncoveredIndex else { return [.pickup] }
            if let c = selected, beats(c, table[i].attack) { return [.cover(table[i].id), .pickup] }
            return [.pickup]
        }
    }

    mutating func youPlay(_ move: WMove, selected: Card?) {
        guard toAct == .you, fool == nil else { return }
        switch move {
        case .attack:
            if let c = selected, legalAttack(c, hand: you) {
                you.removeAll { $0 == c }
                table.append(Battle(id: table.count, attack: c))
                step = .defend
            }
        case .cover(let bid):
            if let c = selected, let i = table.firstIndex(where: { $0.id == bid }), beats(c, table[i].attack) {
                you.removeAll { $0 == c }; table[i].cover = c; step = .attack
            }
        case .pickup:
            you.append(contentsOf: table.flatMap { [$0.attack] + ($0.cover.map { [$0] } ?? []) })
            endBout(defenderPickedUp: true)
        case .done:
            if allCovered { endBout(defenderPickedUp: false) }
        case .pass:
            break
        }
        sortHands()
        runBot()
    }

    // MARK: bot

    /// Run the bot for as long as it's the bot's turn.
    mutating func runBot() {
        var guardN = 0
        while fool == nil, toAct == .bot, guardN < 40 {
            guardN += 1
            if attacker == .bot {          // bot attacks
                let candidates = bot.filter { legalAttack($0, hand: bot) }
                    .sorted { weight($0) < weight($1) }
                // Throw a low card if it has one AND the table isn't huge; else бито.
                if let c = candidates.first, (table.isEmpty || table.count < 4), weight(c) < 40 {
                    bot.removeAll { $0 == c }
                    table.append(Battle(id: table.count, attack: c)); step = .defend
                } else if allCovered {
                    endBout(defenderPickedUp: false)
                } else { break }
            } else {                        // bot defends
                guard let i = uncoveredIndex else { break }
                let cov = bot.filter { beats($0, table[i].attack) }.sorted { weight($0) < weight($1) }.first
                if let c = cov {
                    bot.removeAll { $0 == c }; table[i].cover = c; step = .attack
                } else {
                    bot.append(contentsOf: table.flatMap { [$0.attack] + ($0.cover.map { [$0] } ?? []) })
                    endBout(defenderPickedUp: true)
                }
            }
            sortHands()
        }
    }

    /// Card weight for the greedy bot: prefer shedding low non-trumps.
    private func weight(_ c: Card) -> Int { (c.suit == trump ? 100 : 0) + c.value }

    // MARK: bout resolution + draw + end

    private mutating func endBout(defenderPickedUp: Bool) {
        if !defenderPickedUp { discardCount += table.count + table.compactMap(\.cover).count }
        let previousAttacker = attacker
        table.removeAll(); step = .attack
        // Attacker draws first, then defender.
        drawUp(previousAttacker); drawUp(previousAttacker == .you ? .bot : .you)
        // Pickup = defender skips their attack; otherwise roles swap.
        if defenderPickedUp {
            attacker = previousAttacker
        } else {
            attacker = previousAttacker == .you ? .bot : .you
        }
        checkEnd()
    }

    private mutating func drawUp(_ who: Who) {
        while (who == .you ? you.count : bot.count) < 6, let c = draw() {
            if who == .you { you.append(c) } else { bot.append(c) }
        }
        sortHands()
    }

    private mutating func checkEnd() {
        guard deck.isEmpty else { return }
        let youOut = you.isEmpty, botOut = bot.isEmpty
        if youOut && !botOut { fool = "Bot" }
        else if botOut && !youOut { fool = "You" }
        else if youOut && botOut { fool = "Nobody" }
    }
}
