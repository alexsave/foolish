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
        case .done:  return "Done"
        }
    }
}

@MainActor
final class MockGame: ObservableObject {
    // Games list (root)
    let games: [GameSummary] = [
        GameSummary(id: "g1", opponent: "Sveta", yourTurn: true,  handCount: 5, bot: false),
        GameSummary(id: "g2", opponent: "Boris", yourTurn: true,  handCount: 6, bot: false),
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

    private var loadedId: String?
    let deckMax = 36
    var trumpSuit: Suit { trump.suit }

    /// You attack (vs defend) — used only for the legal-menu branch.
    var youAttack: Bool { attackerSeat == -1 }
    /// It's your move (drives the brass state); every scenario here is your turn.
    var yourTurn: Bool { foolName == nil }

    /// The contextual pill menu for the current selection + role (faked §5.2).
    var legalMoves: [WMove] {
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
        selected = nil; foolName = nil
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
        case "bot":  // fresh offline game — you ATTACK an empty table
            opponentName = "Espresso"
            opponents = [Opponent(id: 0, name: "Espresso", handCount: 6)]
            attackerSeat = -1; defenderSeat = 0
            trump = Card(suit: .diamonds, value: 8)
            battles = []
            hand = [Card(suit: .clubs, value: 6), Card(suit: .diamonds, value: 9), Card(suit: .spades, value: 11),
                    Card(suit: .hearts, value: 7), Card(suit: .diamonds, value: 14), Card(suit: .clubs, value: 13)]
            deckCount = 30; discardCount = 0; isEndgame = false
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

    /// Apply a mock move (mutates just enough to feel alive; the endgame scenario
    /// resolves to the fool reveal so that screen is reachable in-flow).
    func play(_ move: WMove) {
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

    /// Re-deal (Rematch) — drop back to a fresh copy of the last scenario.
    func rematch() {
        let id = loadedId ?? "g1"; loadedId = nil; load(id)
    }
}
