// HandWire.swift - what the fan lays out, and in what order, asked of the
// kernel.
//
// Three questions the board used to answer itself: which cards the fan is given
// (my hand, plus whatever a replay is still holding back), how many of them it
// actually draws, and the order they sit in. None of them is about a screen -
// the fan places cards by INDEX, so the answer is a list, and the same list on
// any client. They are c/src/anim_plan.c's anim_fan_cards / anim_laid_count /
// anim_hand_laid_out now.
//
// THE ARRANGEMENT IS AN INPUT, not state this layer keeps. It is the player's
// own grow-only memory of where their cards have sat (a card you covered with
// and picked back up resumes its slot), it lives per game in the client's
// store, and it legitimately names cards that are not in the hand at all. The
// kernel is told it; it does not own it.
//
// The web states the same contract from the other side as `displayedHand`
// (src/state/clientReconcile.ts).

import Foundation
import CFoolish

public enum HandLayout {

    /// What the fan is asked to lay out: my hand, plus whatever an open replay
    /// is still holding back. A held-back card appears ONCE, and a card the
    /// kernel hand already contains is never doubled by it - the fan places by
    /// index, so a duplicate identity is two cards in one slot.
    ///
    /// NO FAST PATH for an empty holdback, deliberately. It would answer a card
    /// the kernel cannot name (one outside the 52) differently depending on
    /// whether anything was held, and one function may not have two rules.
    public static func fanCards(_ hand: [Card], holding holdback: [Card]) -> [Card] {
        let h = CardSet.ids(hand), held = CardSet.ids(holdback)
        var out = [CChar](repeating: 0, count: h.count + held.count + 1)
        let n: Int32 = h.withUnsafeBufferPointer { hp in
            held.withUnsafeBufferPointer { kp in
                fio_fan_cards(hp.baseAddress, Int32(h.count),
                              kp.baseAddress, Int32(held.count), &out, Int32(out.count))
            }
        }
        guard n >= 0 else { return hand }
        return out.prefix(Int(n)).map { CardSet.card(UInt8(bitPattern: $0)) }
    }

    /// How many cards the fan LAYS OUT: the hand it is really given, minus the
    /// deals still deferring their slot. The trigger for the `fan-rows` trace
    /// and the value that trace re-reads live are the same call, or the log
    /// disagrees with what made it print.
    public static func laidCount(hand: [Card], holding holdback: [Card],
                                 deferred: Set<String>) -> Int {
        let h = CardSet.ids(hand), held = CardSet.ids(holdback)
        let n: Int32 = h.withUnsafeBufferPointer { hp in
            held.withUnsafeBufferPointer { kp in
                fio_laid_count(hp.baseAddress, Int32(h.count),
                               kp.baseAddress, Int32(held.count), CardSet.bits(deferred))
            }
        }
        return n < 0 ? 0 : Int(n)
    }

    /// The array the fan actually draws, in the order it draws it: the deferred
    /// cards drop out, then the local arrangement decides. Ids `order` knows
    /// keep their relative order from it; ids it does not know append in kernel
    /// order (a pickup or a draw lands rightmost); stale entries and repeats
    /// fall out by construction.
    public static func laidOut(hand: [Card], deferred: Set<String>, order: [String]) -> [Card] {
        let cards = CardSet.ids(hand)
        let arrangement = order.compactMap { CardSet.byIdentity[$0] }
        var out = [CChar](repeating: 0, count: cards.count + 1)
        let n: Int32 = cards.withUnsafeBufferPointer { cp in
            arrangement.withUnsafeBufferPointer { op in
                fio_hand_laid_out(cp.baseAddress, Int32(cards.count), CardSet.bits(deferred),
                                  op.baseAddress, Int32(arrangement.count),
                                  &out, Int32(out.count))
            }
        }
        guard n >= 0 else { return hand }
        return out.prefix(Int(n)).map { CardSet.card(UInt8(bitPattern: $0)) }
    }

    /// Does this step put a card from a hand onto the table? `kind` nil is a
    /// step this client cannot name, and moves nothing.
    public static func isPlacement(_ kind: EventType?) -> Bool {
        fio_is_placement(Int32(kind?.rawValue ?? -1)) != 0
    }

    /// The cards THIS stream takes out of MY hand and puts on the table, in
    /// stream order - the seed for the board's holdback. A placement by any
    /// other seat, and every non-placement step, moves nothing out of my hand;
    /// and a seatless viewer places nothing, because the kernel spends seat -1
    /// on "no particular player" as well as on "no seat".
    public static func myPlacedCards(_ events: [GameEvent], mySeat: Int) -> [Card] {
        events.filter { fio_is_my_placement(Int32($0.type), Int32($0.seat), Int32(mySeat)) != 0 }
              .flatMap { $0.cards.compactMap { $0 } }
    }
}
