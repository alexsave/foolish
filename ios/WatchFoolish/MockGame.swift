// MockGame.swift — a representative game state driving the watch screens for the
// design warm-up. NOT the real engine (the plan wires FoolishKit's kernel in W1/
// W2, docs/WATCHOS_APP_PLAN.md §6); this is enough to render every screen/state
// in the §4 sketches and tap through the flow. All "legal moves" are faked with
// simple role logic — on device this comes from the kernel's menu (§5.2).

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
        GameSummary(id: "g1", opponent: "Sveta", yourTurn: true,  handCount: 3, bot: false),
        GameSummary(id: "g2", opponent: "Boris", yourTurn: false, handCount: 6, bot: false),
    ]

    // The live table (the §4b heads-up example, dressed a little richer)
    @Published var opponents: [Opponent] = [
        Opponent(id: 0, name: "Sveta", handCount: 3)
    ]
    @Published var defenderSeat: Int = 0     // 0 = the ring's lone opponent defends
    @Published var attackerSeat: Int = -1    // -1 = you attack
    @Published var battles: [Battle] = [
        Battle(id: 0, attack: Card(suit: .spades, value: 14), cover: Card(suit: .spades, value: 10))
    ]
    @Published var hand: [Card] = [
        Card(suit: .clubs, value: 13), Card(suit: .hearts, value: 8), Card(suit: .diamonds, value: 14),
        Card(suit: .spades, value: 9), Card(suit: .diamonds, value: 5), Card(suit: .clubs, value: 6),
        Card(suit: .hearts, value: 11), Card(suit: .spades, value: 7),
    ]
    @Published var deckCount: Int = 12
    @Published var discardCount: Int = 6
    @Published var trump = Card(suit: .clubs, value: 7)
    @Published var selected: Card?
    @Published var foolName: String?

    let deckMax = 36

    var trumpSuit: Suit { trump.suit }

    /// Whether it's your move (drives the brass state). Here: you're attacking.
    var yourTurn: Bool { attackerSeat == -1 }

    /// The contextual pill menu for the current selection + role (faked §5.2).
    var legalMoves: [WMove] {
        let allCovered = battles.allSatisfy { $0.cover != nil }
        if attackerSeat == -1 {
            // You attack. A selection offers Attack; a fully-covered table offers Done.
            if selected != nil { return [.attack] }
            return allCovered && !battles.isEmpty ? [.done] : [.attack]
        } else {
            // You defend. No selection → Pickup; a selection → the Cover/Pass fork.
            guard let sel = selected else { return [.pickup] }
            if let b = battles.first(where: { $0.cover == nil }) { return [.cover(b.id), .pass] }
            _ = sel
            return [.pickup]
        }
    }

    func toggle(_ card: Card) {
        selected = (selected == card) ? nil : card
        WKInterfaceDevice.current().play(.click)
    }

    /// Apply a mock move (mutates just enough to feel alive).
    func play(_ move: WMove) {
        switch move {
        case .attack:
            if let s = selected { battles.append(Battle(id: battles.count, attack: s)); hand.removeAll { $0 == s } }
        case .cover(let bid):
            if let s = selected, let i = battles.firstIndex(where: { $0.id == bid }) {
                battles[i].cover = s; hand.removeAll { $0 == s }
            }
        case .pass, .done:
            battles.removeAll(); discardCount += 2
        case .pickup:
            battles.removeAll()
        }
        selected = nil
        WKInterfaceDevice.current().play(.click)
    }
}
