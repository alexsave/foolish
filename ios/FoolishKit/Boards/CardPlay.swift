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

    /// Which battle the COVER BUTTON should aim a selection at, when more than
    /// one uncovered attack it could beat is on the table.
    ///
    /// Round 16, owner: "when we tap a card then tap cover, if it is ambiguous
    /// as to which card will be covered, go ahead and cover the highest value
    /// card that can be covered by that card. Trump is higher than non trump.
    /// If there are multiple highest value cards that can be covered, just
    /// choose one." Deliberately NOT the watch's answer, which prompts with a
    /// picker screen; and deliberately not the DRAG path's answer either - a
    /// drag names its target by landing on it, so `resolve(.battle(i))` is
    /// already unambiguous and is left alone.
    ///
    /// Why the highest and not the lowest (which is what a plain "first
    /// coverable index" happened to give, since the table grows left to right):
    /// spending a card on the biggest thing it can beat is the move that keeps
    /// the most of your hand useful. The old rule was not a rule at all - it
    /// was table ORDER, so which attack got covered depended on the sequence
    /// the attackers happened to throw in.
    ///
    /// Ties (two coverable attacks of the same rank and trumpiness, i.e. the
    /// same value in two off-suits) resolve to the leftmost, so the choice is
    /// at least repeatable; the owner's rule is explicitly "just choose one".
    /// nil when the selection covers nothing.
    public static func bestCoverTarget(cards: [Card], battles: [BattleView],
                                       legal: [Move], trumpSuit: Suit?) -> Int? {
        // Sorted, because the source is a Set: an unordered walk would make the
        // tie-break depend on hashing rather than on the table.
        let options = coverableBattles(cards: cards, battles: battles, legal: legal).sorted()
        var best: Int?
        for i in options {
            if best == nil || strength(battles[i].attack, trumpSuit) > strength(battles[best!].attack, trumpSuit) {
                best = i
            }
        }
        return best
    }

    /// How high a card stands in Durak's own order: every trump outranks every
    /// non-trump, and within a class the kernel's rank value decides. Values run
    /// 1...13, so 100 is comfortably clear of any collision between the classes.
    private static func strength(_ card: Card, _ trumpSuit: Suit?) -> Int {
        let isTrump = trumpSuit != nil && card.suit == trumpSuit
        return card.v + (isTrump ? 100 : 0)
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

    /// The moves a HUMAN may actually make on this board right now — the kernel
    /// menu minus `wait`, minus `good` while any attack is still uncovered.
    ///
    /// That last exclusion is a UI rule, not a kernel one: the kernel always
    /// offers GOOD (bots need it, and `fio_actor_mask` has to agree with the
    /// packed menu), while the owner's rule is that an attacker cannot say good
    /// until the table is fully covered. `canSayGood` has always encoded that
    /// for the button; this is the same answer as a SET, for the two places
    /// that need to ask "can this seat do anything at all" rather than "is this
    /// one button live".
    ///
    /// Both callers are the dev auto-player (see MessageTableView's
    /// HARNESS_AUTOMOVE and HarnessModel's turn handoff), and they have to
    /// agree: a handoff that reads the raw kernel menu will pass the game to a
    /// seat whose only offer is a `good` the board will not let it make, and
    /// the run stops dead with no button on screen. That is not a deadlock in
    /// the game — the DEFENDER can always still cover or take — it is the
    /// auto-player asking the wrong question.
    public static func humanMoves(battles: [BattleView], legal: [Move]) -> [Move] {
        let goodAllowed = canSayGood(battles: battles, legal: legal)
        return legal.filter { m in
            switch m.type {
            case .wait: return false
            case .good: return goodAllowed
            default:    return true
            }
        }
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
