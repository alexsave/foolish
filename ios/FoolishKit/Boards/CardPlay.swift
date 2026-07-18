// CardPlay.swift — the ONE gesture→move resolver, shared by both boards
// (TableView in the app, MessageTableView in the iMessage extension) so a card
// plays identically in each. This is the SwiftUI mirror of the web client's
// `determineGameAction` (src/contexts/DragContext.tsx): the UI keeps a dumb
// selection of cards and, at play time, resolves (cards, where-they-were-dropped)
// into ONE concrete legal move by matching against the kernel's legal menu.
//
// It decides NO rules — it only picks, from `legal` (the kernel's enumerated
// legal moves), the entry a gesture refers to. If nothing matches, the gesture is
// rejected. Card identity is (suit,value); order never matters, so cards compare
// as sets, exactly as the web compares field-by-field ignoring order.

import Foundation

/// Where a drag ended / what the player is aiming a selection at.
public enum PlayTarget: Equatable {
    /// Dropped back in the hand area — reorder/cancel, no move.
    case hand
    /// Dropped on the uncovered attack of battle `index` — a cover target.
    case battle(Int)
    /// Dropped on empty table space — an attack (attacker) or pass/auto-cover (defender).
    case table
}

public enum CardPlay {

    /// Resolve a play. `cards` is the selection (or the single dragged/tapped card),
    /// `isDefender` whether the local seat defends, `battles` the current table, and
    /// `legal` the kernel's legal-move menu for this seat. Returns the matching
    /// legal `Move`, or nil if the gesture maps to nothing legal.
    public static func resolve(cards: [Card], target: PlayTarget, isDefender: Bool,
                               battles: [BattleView], legal: [Move]) -> Move? {
        guard !cards.isEmpty else { return nil }
        let want = Set(cards.map(\.identity))
        func sameCards(_ m: Move) -> Bool { Set(m.cards.map(\.identity)) == want }

        if !isDefender {
            // Attacker: the only card play is an attack with exactly these cards
            // (one card, or several of the same rank — the kernel enumerates which).
            return legal.first { $0.type == .attack && sameCards($0) }
        }

        // Defender.
        switch target {
        case .hand:
            return nil
        case .battle(let i):
            guard i >= 0, i < battles.count, battles[i].defense == nil else { return nil }
            let attack = battles[i].attack
            // A cover with these cards that covers THIS attack (single cover:
            // attackCards == [attack]; multicover: attack is among them).
            return legal.first {
                $0.type == .cover && sameCards($0)
                    && ($0.attackCards ?? []).contains(attack)
            }
        case .table:
            // Empty space: a pass (bounce the bout) wins if legal with these cards…
            if let pass = legal.first(where: { $0.type == .pass && sameCards($0) }) {
                return pass
            }
            // …otherwise auto-target a cover, but only if it is unambiguous (exactly
            // one legal cover uses this selection). Mirrors the web's single-valid-
            // target / kernelUnambiguousCover fallback.
            let covers = legal.filter { $0.type == .cover && sameCards($0) }
            return covers.count == 1 ? covers.first : nil
        }
    }

    /// The battle indices a current selection could legally cover — for highlighting
    /// drop targets while dragging / after selecting (mirrors the web's live
    /// drop-preview + coverable set).
    public static func coverableBattles(cards: [Card], battles: [BattleView],
                                        legal: [Move]) -> Set<Int> {
        guard !cards.isEmpty else { return [] }
        let want = Set(cards.map(\.identity))
        var out: Set<Int> = []
        for (i, b) in battles.enumerated() where b.defense == nil {
            let coversThis = legal.contains {
                $0.type == .cover && Set($0.cards.map(\.identity)) == want
                    && ($0.attackCards ?? []).contains(b.attack)
            }
            if coversThis { out.insert(i) }
        }
        return out
    }

    /// The zero-selection control moves available now (pickup / good), read straight
    /// off the menu — the action bar's enable state.
    public static func has(_ type: MoveType, in legal: [Move]) -> Bool {
        legal.contains { $0.type == type }
    }

    /// Whether to surface the "Good" (finish attacking / бито) button. The kernel
    /// legality allows an attacker to say good whenever (bots do), but the web only
    /// SHOWS the button once every attack on the table is covered — cleaner, and you
    /// can still throw in more instead. This is a UI gate over C's own battle state,
    /// not a re-implemented rule (`.good` legality still comes from the menu).
    public static func canSayGood(battles: [BattleView], legal: [Move]) -> Bool {
        has(.good, in: legal) && !battles.isEmpty && battles.allSatisfy { $0.defense != nil }
    }

    /// Whether the selection can be attacked / passed / covered right now — the
    /// selection-driven buttons' enable state (Attack / Pass / Cover).
    public static func canAttack(_ cards: [Card], legal: [Move]) -> Bool {
        resolveVerb(.attack, cards: cards, legal: legal)
    }
    public static func canPass(_ cards: [Card], legal: [Move]) -> Bool {
        resolveVerb(.pass, cards: cards, legal: legal)
    }
    public static func canCover(_ cards: [Card], battles: [BattleView], legal: [Move]) -> Bool {
        !coverableBattles(cards: cards, battles: battles, legal: legal).isEmpty
    }

    private static func resolveVerb(_ type: MoveType, cards: [Card], legal: [Move]) -> Bool {
        guard !cards.isEmpty else { return false }
        let want = Set(cards.map(\.identity))
        return legal.contains { $0.type == type && Set($0.cards.map(\.identity)) == want }
    }
}
