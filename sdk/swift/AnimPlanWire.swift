// AnimPlanWire.swift - the count-freeze and the timed shape of a sequence,
// asked of the kernel.
//
// MessageTableView.preCounts used to answer the first half of this in Swift. It
// stayed there only because the C rule was wrong: the kernel walked back from
// the FINAL board over every event, and undoing a REFILL puts its cards back in
// the deck even though the flipped trump under the deck was dealt without ever
// being counted. The kernel anchors on the first event's own board now
// (c/src/anim_plan.c), so the Swift copy is gone and this is the crossing.
//
// THE STREAM IS AN INPUT, for the same reason BeatWire's is: a board animates
// the stream it was HANDED - a bubble's events, and often only half of them,
// because a staged bout end is cut at its settlement - and it asks from a
// SwiftUI render pass that cannot await the actor the resident game lives
// behind.

import Foundation
import CFoolish

/// The kernel's plan for one animated sequence: the board the display FREEZES
/// at before the first flight, then a step per event carrying its timing and
/// the board that step settles to. `fio_anim_plan_packed`'s answer, decoded once.
public struct AnimPlan: Equatable, Sendable {

    /// The whole board the display holds: deck, discard, every seat's hand
    /// count - AND THE BATTLE ROW.
    ///
    /// The row is the field this type spent eleven rounds without, and its
    /// absence was a shipped defect. The three counts froze to the board before
    /// the move and the row did not, so a replay opened with the deck badge
    /// correctly held at its pre-move value and the table beside it already in
    /// the arrangement the move produced - a pile 36pt to the side from the
    /// first painted frame, and never moving, because a cold open has no
    /// previous layout for SwiftUI to interpolate away from. Empty on a `Step`,
    /// where the row is the step's own board and the board commits it directly.
    public struct Counts: Equatable, Sendable {
        public let deck: Int
        public let discard: Int
        /// By seat, dense over the table.
        public let hand: [Int: Int]
        /// The battle row, in its real left-to-right order. Empty for "the
        /// kernel could not say", where a caller lays out the live table
        /// exactly as it did before the row was in the plan at all - never a
        /// truncated or invented row.
        public let battles: [BattleView]
        /// True when that row came off a board the kernel really had, false for
        /// the flat one-cell-per-card reading of a pickup. `PreBoutTable.paired`
        /// carried through: a caller choosing between two tables must not treat
        /// the second as a table.
        public let battlesPaired: Bool
        /// THE FLIPPED TRUMP THE WELL OPENS ON - the stock's other half, and
        /// the field this type shipped without for as long as it shipped
        /// without the row.
        ///
        /// `nil` means the trump has been dealt out on this board, not "the
        /// kernel could not say": a plan always has an answer, and the degraded
        /// `frozen(at:)` path answers with the view's own. Carrying the CARD
        /// rather than a flag is deliberate - once the trump is drawn the
        /// kernel keeps a stale card in the slot and the wire writes a
        /// placeholder over it, so a caller told only "there was one" would
        /// draw a wrong face. See c/src/anim_plan.h, AnimCounts.
        public let flipped: Card?

        /// The kernel's AnimCounts as the freeze this type carries. The ROW is
        /// two bytes per battle (the attack, then its cover or the no-card
        /// sentinel), which is why `n_battles` is not that array's length and
        /// crosses beside it.
        public init(kernel c: AnimCountsSnap) {
            var hand: [Int: Int] = [:]
            for (s, n) in c.hand.enumerated() { hand[s] = n }
            var row: [BattleView] = []
            row.reserveCapacity(c.nBattles)
            for i in 0..<min(c.nBattles, c.battles.count / 2) {
                let cover = c.battles[2 * i + 1]
                row.append(BattleView(attack: AnimPlan.card(UInt8(truncatingIfNeeded: c.battles[2 * i])),
                                      defense: cover == Int(FIO_PRETABLE_NONE)
                                               ? nil : AnimPlan.card(UInt8(truncatingIfNeeded: cover))))
            }
            self.init(deck: c.deck, discard: c.discard, hand: hand,
                      battles: row, battlesPaired: c.paired != 0,
                      // CARD_NONE once a refill has dealt the trump out.
                      flipped: c.flipped.value > 0
                               ? Card(s: c.flipped.suit, v: c.flipped.value) : nil)
        }

        public init(deck: Int, discard: Int, hand: [Int: Int],
                    battles: [BattleView] = [], battlesPaired: Bool = false,
                    flipped: Card? = nil) {
            self.deck = deck
            self.discard = discard
            self.hand = hand
            self.battles = battles
            self.battlesPaired = battlesPaired
            self.flipped = flipped
        }
    }

    public struct Step: Equatable, Sendable {
        /// EventType raw, and the acting seat (-1 for none).
        public let type: Int
        public let seat: Int
        /// EventLoc raw.
        public let from: Int
        public let to: Int
        public let cardCount: Int
        /// How long this step's flight runs, and when it starts relative to the
        /// sequence's first frame. The kernel owns the pacing; a platform never
        /// invents its own.
        public let durationMs: Int
        public let startMs: Int
        /// The board this step settles to - its OWN snapshot, which is what the
        /// badges are pinned to as the flight lands.
        public let counts: Counts
        /// Cards of this step that left the deck, and how many of those are
        /// bound for the flipped slot (which does not move the deck badge).
        public let inFlightFromDeck: Int
        public let inFlightToFlipped: Int

        public var kind: EventType? { EventType(rawValue: type) }
    }

    /// The board the display holds until the first flight lands.
    public let pre: Counts
    public let steps: [Step]
    /// Identities the sequence brings into being - hide them until the step
    /// that lands them, so they fly rather than popping in.
    public let veil: Set<String>
    /// Wall time of the whole sequence.
    public let totalMs: Int

    public static let empty = AnimPlan(pre: Counts(deck: 0, discard: 0, hand: [:]),
                                       steps: [], veil: [], totalMs: 0)

    /// Ask the kernel for a stream's plan against the board it settles on.
    ///
    /// A stream longer than the plan can hold, or one the kernel refuses,
    /// answers with the FINAL board frozen and no steps - the same
    /// degrade-to-less-animation discipline the other wire readers keep.
    public init(_ events: [GameEvent], finalView: GameView) {
        let np = finalView.players.count
        guard np >= 2, np <= Int(FIO_PLAN_SEATS), !events.isEmpty else {
            self = Self.frozen(at: finalView)
            return
        }
        let finalHand = Self.handBySeat(finalView)

        var input: [UInt8] = [UInt8(FIO_PLAN_VERSION), UInt8(np),
                              UInt8(min(events.count, 255)),
                              UInt8(clamping: finalView.deckCount),
                              UInt8(clamping: finalView.discardCount),
                              Self.denseId(of: finalView)]
        input.reserveCapacity(events.count * (12 + np + 2 * 6) + 5 + np)
        for s in 0..<np { input.append(UInt8(clamping: finalHand[s] ?? 0)) }
        for ev in events.prefix(255) {
            // Only REAL identities travel; a redacted card is a back and names
            // nothing the veil could hold. The COUNT still crosses, because the
            // arithmetic that undoes the first event reads it.
            let ids = ev.cards.compactMap { $0 }.filter { !$0.isHidden }
                .map { UInt8($0.s * 13 + ($0.v - 1)) }
            let board = ev.state
            input.append(UInt8(truncatingIfNeeded: ev.type))
            input.append(ev.seat >= 0 && ev.seat < 0xFF ? UInt8(ev.seat) : 0xFF)
            input.append(UInt8(truncatingIfNeeded: ev.from))
            input.append(UInt8(truncatingIfNeeded: ev.to))
            input.append(UInt8(clamping: ev.cards.count))
            input.append(UInt8(clamping: min(ids.count, ev.cards.count)))
            input.append(board == nil ? 0 : 1)
            input.append(UInt8(clamping: board?.deckCount ?? 0))
            input.append(UInt8(clamping: board?.discardCount ?? 0))
            // …AND THE TRUMP UNDER THE DECK, which is as much a part of "the
            // board this event committed" as the deck count above it.
            input.append(board.map(Self.denseId) ?? UInt8(FIO_PLAN_NO_FLIP))
            let bySeat = board.map(Self.handBySeat)
            for s in 0..<np { input.append(UInt8(clamping: bySeat?[s] ?? 0)) }
            input.append(contentsOf: ids.prefix(ev.cards.count))
            // …AND THE ROW that board carried, in the one encoding a table has
            // here (PreBoutTable.table). It is what makes the freeze's row
            // derivable at all: the row before a pass is this row with the
            // passed card taken back off it.
            input.append(contentsOf: PreBoutTable.table(board?.battles))
        }

        // The answer is the kernel's own AnimPlan (anim_plan.h), copied out
        // through the generated reader. It used to be flattened into a packed
        // block - an 85-byte head, a 23-byte stride per step, a veil tail - and
        // unpacked here, which was the same layout written down twice.
        let ok: Int32 = input.withUnsafeBufferPointer { p in
            fio_anim_plan(p.baseAddress, Int32(input.count))
        }
        guard ok == 0, let ptr = fio_anim_plan_ptr(), let pl = try? readAnimPlan(ptr),
              pl.pre.hand.count == np
        else { self = Self.frozen(at: finalView); return }

        self.totalMs = pl.totalMs
        self.pre = Counts(kernel: pl.pre)
        // A step's board carries NO row, deliberately, and anim_plan.h says
        // why: the row a step settles to IS that step's own snapshot, which
        // every client already commits as the flight lands (the same line that
        // pins the deck and the badges). Only the FREEZE needs a rule.
        self.steps = pl.steps.map { st in
            Step(type: st.type, seat: st.seat == ANIM_SEAT_NONE ? -1 : st.seat,
                 from: st.from, to: st.to, cardCount: st.nCards,
                 durationMs: st.durationMs, startMs: st.startMs,
                 counts: Counts(deck: st.deck, discard: st.discard,
                                hand: Self.seatDict(st.hand, seats: np)),
                 inFlightFromDeck: st.inFlightFromDeck,
                 inFlightToFlipped: st.inFlightToFlipped)
        }
        self.veil = Set(pl.veilIds.filter { $0 < 52 }.map { Card(s: $0 / 13, v: $0 % 13 + 1).identity })
    }

    private init(pre: Counts, steps: [Step], veil: Set<String>, totalMs: Int) {
        self.pre = pre
        self.steps = steps
        self.veil = veil
        self.totalMs = totalMs
    }

    /// Nothing to animate: the board sits at the state it already settled on.
    private static func frozen(at v: GameView) -> AnimPlan {
        AnimPlan(pre: Counts(deck: v.deckCount, discard: v.discardCount,
                             hand: handBySeat(v), flipped: trump(of: v)),
                 steps: [], veil: [], totalMs: 0)
    }

    /// A board's flipped trump, or nil once it has been dealt out. `hasFlipped`
    /// is the gate and `flipped` alone is not: after the draw the kernel keeps a
    /// stale card in the slot, and the state wire writes a canonical placeholder
    /// over it rather than that card (c/src/view.c).
    private static func trump(of v: GameView) -> Card? {
        guard v.hasFlipped, let f = v.flipped, !f.isHidden else { return nil }
        return f
    }

    private static func denseId(of v: GameView) -> UInt8 {
        guard let f = trump(of: v), f.s >= 0, f.s < 4, f.v >= 1, f.v <= 13
        else { return UInt8(FIO_PLAN_NO_FLIP) }
        return UInt8(f.s * 13 + (f.v - 1))
    }

    fileprivate static func card(_ id: UInt8) -> Card {
        Card(s: Int(id) / 13, v: Int(id) % 13 + 1)
    }

    private static func handBySeat(_ v: GameView) -> [Int: Int] {
        Dictionary(uniqueKeysWithValues: v.players.map { ($0.seat, $0.handCount) })
    }

    /// A seat-indexed hand block as the dictionary this type carries.
    private static func seatDict(_ hand: [Int], seats: Int) -> [Int: Int] {
        var out: [Int: Int] = [:]
        for s in 0..<min(seats, hand.count) { out[s] = hand[s] }
        return out
    }
}
