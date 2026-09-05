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

    /// Deck, discard and every seat's hand count, as one board.
    public struct Counts: Equatable, Sendable {
        public let deck: Int
        public let discard: Int
        /// By seat, dense over the table.
        public let hand: [Int: Int]

        public init(deck: Int, discard: Int, hand: [Int: Int]) {
            self.deck = deck
            self.discard = discard
            self.hand = hand
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
                              UInt8(clamping: finalView.discardCount)]
        input.reserveCapacity(events.count * (10 + np) + 5 + np)
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
            let bySeat = board.map(Self.handBySeat)
            for s in 0..<np { input.append(UInt8(clamping: bySeat?[s] ?? 0)) }
            input.append(contentsOf: ids.prefix(ev.cards.count))
        }

        let head = Int(FIO_PLAN_HEAD), stride = Int(FIO_PLAN_STRIDE)
        var out = [CChar](repeating: 0, count: head + 256 * stride + 256)
        let n: Int32 = input.withUnsafeBufferPointer { p in
            fio_anim_plan_packed(p.baseAddress, Int32(input.count), &out, Int32(out.count))
        }
        guard n >= Int32(head) else { self = Self.frozen(at: finalView); return }

        let b = out.prefix(Int(n)).map { UInt8(bitPattern: $0) }
        let count = Int(b[1]), seats = Int(b[2]), nVeil = Int(b[3])
        guard seats == np, b.count >= head + count * stride + nVeil else {
            self = Self.frozen(at: finalView)
            return
        }
        self.totalMs = Self.u32(b, 4)
        self.pre = Counts(deck: Int(b[8]), discard: Int(b[9]),
                          hand: Self.seatDict(b, at: 10, seats: np))

        var built: [Step] = []
        built.reserveCapacity(count)
        for i in 0..<count {
            let e = head + i * stride
            built.append(Step(type: Int(b[e]),
                              seat: b[e + 1] == 0xFF ? -1 : Int(b[e + 1]),
                              from: Int(b[e + 2]), to: Int(b[e + 3]),
                              cardCount: Int(b[e + 4]),
                              durationMs: Int(b[e + 5]) | (Int(b[e + 6]) << 8),
                              startMs: Self.u32(b, e + 7),
                              counts: Counts(deck: Int(b[e + 11]), discard: Int(b[e + 12]),
                                             hand: Self.seatDict(b, at: e + 15, seats: np)),
                              inFlightFromDeck: Int(b[e + 13]),
                              inFlightToFlipped: Int(b[e + 14])))
        }
        self.steps = built

        let v = head + count * stride
        var ids = Set<String>()
        for i in 0..<nVeil where b[v + i] < 52 {
            ids.insert(Card(s: Int(b[v + i]) / 13, v: Int(b[v + i]) % 13 + 1).identity)
        }
        self.veil = ids
    }

    private init(pre: Counts, steps: [Step], veil: Set<String>, totalMs: Int) {
        self.pre = pre
        self.steps = steps
        self.veil = veil
        self.totalMs = totalMs
    }

    /// Nothing to animate: the board sits at the state it already settled on.
    private static func frozen(at v: GameView) -> AnimPlan {
        AnimPlan(pre: Counts(deck: v.deckCount, discard: v.discardCount, hand: handBySeat(v)),
                 steps: [], veil: [], totalMs: 0)
    }

    private static func handBySeat(_ v: GameView) -> [Int: Int] {
        Dictionary(uniqueKeysWithValues: v.players.map { ($0.seat, $0.handCount) })
    }

    private static func u32(_ b: [UInt8], _ at: Int) -> Int {
        var v = 0
        for i in 0..<4 where at + i < b.count { v |= Int(b[at + i]) << (8 * i) }
        return v
    }

    private static func seatDict(_ b: [UInt8], at: Int, seats: Int) -> [Int: Int] {
        var out: [Int: Int] = [:]
        for s in 0..<seats where at + s < b.count { out[s] = Int(b[at + s]) }
        return out
    }
}
