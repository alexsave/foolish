// MockGame.swift — representative game state driving the watch screens for the
// design warm-up. NOT the real engine (the plan wires FoolishKit's kernel in W1/
// W2, docs/WATCHOS_APP_PLAN.md §6); this is enough to render every screen/state
// in the §4 sketches and tap through the flow. Each Games-list row loads a
// distinct scenario via `load(_:)` so all pill/table states are reachable.

import SwiftUI
import WatchKit

struct Opponent: Identifiable, Equatable {
    let id: Int          // ring seat index (0 = 12 o'clock, clockwise)
    let name: String
    let handCount: Int
    var isOut: Bool = false
}

struct Battle: Identifiable, Equatable {
    let id: Int
    let attack: Card
    var cover: Card?
}

/// A row in the Games list.
struct GameSummary: Identifiable {
    let id: String
    let opponent: String
    let yourTurn: Bool
    let handCount: Int
    let bot: Bool
}

/// A contextual move offered as a bottom pill (§5.2). On device these are the
/// kernel's legal menu, verbatim.
enum WMove: Equatable {
    case attack, cover(Int), pass, pickup, done
    var label: String {
        switch self {
        case .attack: return "Attack"
        case .cover: return "Cover"
        case .pass:  return "Pass"
        case .pickup: return "Pickup"
        case .done:  return "Good"
        }
    }
}

@MainActor
final class MockGame: ObservableObject {
    // Games list (root)
    let games: [GameSummary] = [
        GameSummary(id: "g1", opponent: "Sveta",  yourTurn: true,  handCount: 5, bot: false),
        GameSummary(id: "g2", opponent: "Boris",  yourTurn: true,  handCount: 6, bot: false),
        GameSummary(id: "g8", opponent: "8-seat", yourTurn: true,  handCount: 7, bot: false),
    ]

    // The live table
    @Published var opponentName = "Sveta"
    @Published var opponents: [Opponent] = []
    @Published var defenderSeat: Int = 0     // ring seat that's defending; -1 = you defend
    @Published var attackerSeat: Int = -1    // ring seat that's attacking; -1 = you attack
    @Published var battles: [Battle] = []
    @Published var hand: [Card] = []
    @Published var deckCount: Int = 12
    @Published var discardCount: Int = 6
    @Published var trump = Card(suit: .clubs, value: 7)
    @Published var selected: Card?
    @Published var foolName: String?
    @Published private(set) var isEndgame = false

    /// The "Bot game" is a REAL heads-up Durak (DurakEngine); the online rows are
    /// static design scenarios. Non-nil only for the bot game.
    private var engine: DurakEngine?

    private var loadedId: String?
    let deckMax = 36
    var trumpSuit: Suit { trump.suit }

    /// You attack (vs defend) — used only for the faked legal-menu branch.
    var youAttack: Bool { attackerSeat == -1 }
    /// It's your move (drives the brass state).
    var yourTurn: Bool {
        if let e = engine { return e.fool == nil && e.toAct == .you }
        return foolName == nil
    }

    /// The contextual pill menu for the current selection + role.
    var legalMoves: [WMove] {
        if let e = engine { return e.youLegal(selected: selected) }
        let allCovered = !battles.isEmpty && battles.allSatisfy { $0.cover != nil }
        if youAttack {
            if selected != nil { return [.attack] }
            return allCovered ? [.done] : [.attack]
        } else {
            guard selected != nil else { return [.pickup] }
            if let b = battles.first(where: { $0.cover == nil }) { return [.cover(b.id), .pass] }
            return [.pickup]
        }
    }

    /// Set up the table for a tapped game. Idempotent per id so returning from
    /// the Action screen doesn't wipe moves in progress.
    func load(_ gameId: String) {
        guard gameId != loadedId else { return }
        loadedId = gameId
        selected = nil; foolName = nil; engine = nil
        switch gameId {
        case "g2":   // Boris — you DEFEND an uncovered attack (Pickup / Cover·Pass)
            opponentName = "Boris"
            opponents = [Opponent(id: 0, name: "Boris", handCount: 6)]
            attackerSeat = 0; defenderSeat = -1
            trump = Card(suit: .spades, value: 6)
            battles = [Battle(id: 0, attack: Card(suit: .hearts, value: 9))]
            hand = [Card(suit: .spades, value: 13), Card(suit: .hearts, value: 10), Card(suit: .diamonds, value: 12),
                    Card(suit: .spades, value: 7), Card(suit: .clubs, value: 8), Card(suit: .diamonds, value: 6)]
            deckCount = 20; discardCount = 2; isEndgame = false
        case "g8":   // 8-seat stress: 7 opponents on the ring, several battles
            opponentName = "Table"
            opponents = [
                Opponent(id: 0, name: "Sveta", handCount: 6), Opponent(id: 1, name: "Boris", handCount: 3),
                Opponent(id: 2, name: "Katya", handCount: 2), Opponent(id: 3, name: "Dima", handCount: 8),
                Opponent(id: 4, name: "Lena", handCount: 9), Opponent(id: 5, name: "Max", handCount: 1, isOut: true),
                Opponent(id: 6, name: "Nina", handCount: 6),
            ]
            attackerSeat = -1; defenderSeat = 3      // seat D defends; you (and others) attack
            trump = Card(suit: .hearts, value: 6)
            battles = [
                Battle(id: 0, attack: Card(suit: .spades, value: 12), cover: Card(suit: .spades, value: 13)),
                Battle(id: 1, attack: Card(suit: .diamonds, value: 9)),
                Battle(id: 2, attack: Card(suit: .clubs, value: 7), cover: Card(suit: .hearts, value: 8)),
            ]
            hand = [Card(suit: .hearts, value: 14), Card(suit: .spades, value: 6), Card(suit: .clubs, value: 10),
                    Card(suit: .diamonds, value: 11), Card(suit: .hearts, value: 13), Card(suit: .spades, value: 8),
                    Card(suit: .clubs, value: 9)]
            deckCount = 8; discardCount = 14; isEndgame = false
        case "bot":  // a REAL heads-up game vs the bot
            opponentName = "Bot"
            engine = DurakEngine.newGame()
            mirror()
            return
        default:     // g1 Sveta — you ATTACK, table fully covered (Done); endgame
            opponentName = "Sveta"
            opponents = [Opponent(id: 0, name: "Sveta", handCount: 3)]
            attackerSeat = -1; defenderSeat = 0
            trump = Card(suit: .clubs, value: 7)
            battles = [Battle(id: 0, attack: Card(suit: .spades, value: 14), cover: Card(suit: .spades, value: 10))]
            hand = [Card(suit: .clubs, value: 13), Card(suit: .hearts, value: 8), Card(suit: .diamonds, value: 14),
                    Card(suit: .spades, value: 9), Card(suit: .diamonds, value: 5)]
            deckCount = 12; discardCount = 6; isEndgame = true
        }
    }

    func toggle(_ card: Card) {
        selected = (selected == card) ? nil : card
        WKInterfaceDevice.current().play(.click)
    }

    /// Mirror the real engine's state into the published fields (bot game).
    private func mirror() {
        guard let e = engine else { return }
        battles = e.table
        hand = e.you
        opponents = [Opponent(id: 0, name: "Bot", handCount: e.bot.count)]
        deckCount = e.deck.count
        discardCount = e.discardCount
        trump = e.trumpCard
        if e.attacker == .you { attackerSeat = -1; defenderSeat = 0 }
        else { attackerSeat = 0; defenderSeat = -1 }
        foolName = e.fool
        selected = nil
    }

    /// Apply a move. The bot game runs the real engine (and the bot's reply);
    /// the static online scenarios mutate just enough to feel alive.
    func play(_ move: WMove) {
        if engine != nil {
            engine!.youPlay(move, selected: selected)
            mirror()
            WKInterfaceDevice.current().play(.click)
            return
        }
        switch move {
        case .attack:
            if let s = selected { battles.append(Battle(id: battles.count, attack: s)); hand.removeAll { $0 == s } }
        case .cover(let bid):
            if let s = selected, let i = battles.firstIndex(where: { $0.id == bid }) {
                battles[i].cover = s; hand.removeAll { $0 == s }
            }
        case .pass:
            battles.removeAll(); discardCount += 2
        case .done:
            battles.removeAll(); discardCount += 2
            if isEndgame { foolName = opponentName }   // you closed it out — they're the fool
        case .pickup:
            battles.removeAll()
        }
        selected = nil
        WKInterfaceDevice.current().play(.click)
    }

    /// Re-deal (Rematch). The bot game deals a fresh engine; scenarios reload.
    func rematch() {
        if engine != nil { engine = DurakEngine.newGame(); mirror(); return }
        let id = loadedId ?? "g1"; loadedId = nil; load(id)
    }
}
