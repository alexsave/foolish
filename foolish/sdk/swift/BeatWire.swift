// BeatWire.swift - the SHAPE of an animated sequence, asked of the kernel.
//
// What plays together, what waits, what a beat carries with it used to be Swift
// (MessageTableView's parallelGroups / placedOnTable / outsWith / holdsAfter /
// badgeDropsAsCardsLeave, and the role beat's goodsOpening / goodsCleared /
// passHandOff). They give the same answers on any screen, so they are C now
// (c/src/anim_plan.c) and this is the crossing. Only rendering - the springs,
// the rects, the tweens - stayed behind.
//
// THE STREAM IS AN INPUT, not something the kernel re-derives, and that is a
// correctness requirement rather than a style choice. A board animates the
// stream it was HANDED: a bubble's events, and often only HALF of them, because
// a staged bout end is cut at its settlement and the second half is withheld
// until Send. It also asks from a SwiftUI render pass, which cannot await the
// actor the resident game lives behind.
//
// ONE CALL FOR THE WHOLE SHAPE, because a board that asked for a beat's grouping
// and its hold separately could be told two different things.

import Foundation
import CFoolish

/// What the role badges are WEARING - which is not the live board: a sequence
/// freezes the marks and walks them forward one beat at a time.
public struct RoleMarks: Equatable, Sendable {
    public let defender: Int
    public let firstAttacker: Int
    public let goodMask: Int

    public init(defender: Int, firstAttacker: Int, goodMask: Int = 0) {
        self.defender = defender
        self.firstAttacker = firstAttacker
        self.goodMask = goodMask
    }
    public init(_ v: GameView) {
        self.init(defender: v.defender, firstAttacker: v.firstAttacker, goodMask: v.goodMask)
    }
    fileprivate init(_ triple: [Int32]) {
        self.init(defender: Int(triple[0]), firstAttacker: Int(triple[1]),
                  goodMask: Int(triple[2]))
    }
}

/// A stream's beats, and everything each one carries. `fio_beats_packed`'s
/// answer, decoded once.
public struct AnimBeats: Equatable, Sendable {

    public struct Beat: Equatable, Sendable {
        /// Where this beat's events sit in the stream it was built from.
        public let range: Range<Int>
        /// The LEAD event's type (EventType raw) and seat. A multi-card cover is
        /// several events and one beat; everything a step reads off "the event"
        /// reads it off this one.
        public let type: Int
        public let seat: Int
        /// Rest after this beat - a cover whose bout end follows it.
        public let holds: Bool
        /// It moved a card at all (which is what lets it adopt the out notices
        /// trailing it), and it put one down on the table.
        public let moved: Bool
        public let placedAny: Bool
        /// The acting badge drops as these cards LEAVE rather than as they land.
        public let dropsBadge: Bool
        /// The seats that go out WITH this beat's card motion.
        public let outs: Set<Int>
        /// Seats that laid cards via ATTACK_PASS here, as a bitmask - the input
        /// to a transfer's hand-off, which only the kernel can read as a rule.
        public let attackPassSeats: Int
        /// The identities this beat puts on the table.
        public let placed: Set<String>
        /// The good mask of the board this beat settles to (its LAST event's),
        /// or nil for a beat whose steps carried no board.
        public let goodMask: Int?

        public var count: Int { range.count }
        public var kind: EventType? { EventType(rawValue: type) }
    }

    public let beats: [Beat]
    /// Every identity the WHOLE stream puts down on the table - the cards whose
    /// arrival is a thing to watch, as opposed to the ones already lying there.
    public let placed: Set<String>
    /// The good mask of the stream's FIRST step, or nil when it has no steps.
    public let firstGoodMask: Int?

    public static let empty = AnimBeats(beats: [], placed: [], firstGoodMask: nil)

    /// Ask the kernel for a stream's shape.
    ///
    /// A stream longer than the kernel's plan can hold, or one it refuses,
    /// answers `.empty` - the same degrade-to-less-animation discipline the rest
    /// of the wire readers keep.
    public init(_ events: [GameEvent]) {
        guard !events.isEmpty else { self = .empty; return }
        var input: [UInt8] = [UInt8(FIO_BEATS_VERSION),
                              UInt8(min(events.count, 255))]
        input.reserveCapacity(events.count * 8 + 2)
        for ev in events.prefix(255) {
            // Only REAL identities travel: a redacted card is a back, and a back
            // names nothing the table can be drawn from.
            let ids = ev.cards.compactMap { $0 }.filter { !$0.isHidden }
                .map { UInt8($0.s * 13 + ($0.v - 1)) }
            let good = ev.state?.goodMask
            input.append(UInt8(truncatingIfNeeded: ev.type))
            input.append(ev.seat >= 0 && ev.seat < 0xFF ? UInt8(ev.seat) : 0xFF)
            input.append(good == nil ? 0 : 1)
            input.append(UInt8(truncatingIfNeeded: good ?? 0))
            input.append(UInt8(min(ids.count, 255)))
            input.append(contentsOf: ids.prefix(255))
        }

        // The answer is the kernel's own AnimBeats (anim_plan.h), copied out
        // through the generated reader. It used to be a packed block with a
        // 12-byte head and a 17-byte stride, written in ios_api.c and unpacked
        // here - the same layout in two languages.
        let ok: Int32 = input.withUnsafeBufferPointer { p in
            fio_beats(p.baseAddress, Int32(input.count))
        }
        guard ok == 0, let ptr = fio_beats_ptr(), let bs = try? readAnimBeats(ptr)
        else { self = .empty; return }

        self.firstGoodMask = bs.firstGoodMask == ANIM_NO_MASK ? nil : bs.firstGoodMask
        self.placed = Self.identities(bs.placedIds)

        var out_beats: [Beat] = []
        out_beats.reserveCapacity(bs.beats.count)
        for b in bs.beats {
            var outs = Set<Int>()
            for s in 0..<8 where b.outsMask & (1 << s) != 0 { outs.insert(s) }
            out_beats.append(Beat(range: b.first..<(b.first + b.nEvents),
                                  type: b.type,
                                  seat: b.seat == ANIM_SEAT_NONE ? -1 : b.seat,
                                  holds: b.flags & ANIM_BEAT_HOLDS != 0,
                                  moved: b.flags & ANIM_BEAT_MOVED != 0,
                                  placedAny: b.flags & ANIM_BEAT_PLACED != 0,
                                  dropsBadge: b.flags & ANIM_BEAT_DROPS != 0,
                                  outs: outs,
                                  attackPassSeats: b.attackPassSeats,
                                  placed: Self.identities(b.placedIds),
                                  goodMask: b.goodMask == ANIM_NO_MASK ? nil : b.goodMask))
        }
        self.beats = out_beats
    }

    private init(beats: [Beat], placed: Set<String>, firstGoodMask: Int?) {
        self.beats = beats
        self.placed = placed
        self.firstGoodMask = firstGoodMask
    }

    /// Dense card ids back to the identities the board animates by.
    private static func identities(_ ids: UInt64) -> Set<String> {
        var out = Set<String>()
        for id in 0..<52 where ids & (UInt64(1) << id) != 0 {
            out.insert(Card(s: id / 13, v: id % 13 + 1).identity)
        }
        return out
    }

    /// Does a step of this kind take cards OUT of the acting seat's hand? The
    /// `dropsBadge` question for a caller holding one event rather than a beat:
    /// a badge drops as its cards LEAVE (they are in the air, and a badge still
    /// counting them is claiming a hand that big plus the flight) and ticks up
    /// only as arriving cards land. A step the wire does not recognise moves
    /// nobody's hand.
    public static func badgeDropsAsCardsLeave(_ kind: EventType?) -> Bool {
        fio_badge_drops_as_cards_leave(Int32(kind?.rawValue ?? -1)) != 0
    }
}

/// WHICH MARKS CHANGE AT WHICH POINT of a sequence - three timings, not one.
/// The prose is in c/src/anim_plan.h; each of these is the kernel's answer.
public enum RoleBeat {

    /// The state a stream should OPEN on, or nil for "start playing straight
    /// away". A good being SET is somebody's move and belongs in front of the
    /// consequences it caused; only ADDED goods, and only ever added to what is
    /// already shown.
    public static func goodsOpening(shown: RoleMarks?, firstGoodMask: Int?) -> RoleMarks? {
        ask(shown) { s, out in
            fio_roles_goods_opening(Int32(s.defender), Int32(s.firstAttacker),
                                    Int32(s.goodMask), Int32(firstGoodMask ?? -1), out)
        }
    }

    /// The mirror image, played in parallel with the throw-in that cleared it:
    /// the card and the marks are one event and neither leads.
    public static func goodsCleared(shown: RoleMarks?, stepGoodMask: Int?) -> RoleMarks? {
        ask(shown) { s, out in
            fio_roles_goods_cleared(Int32(s.defender), Int32(s.firstAttacker),
                                    Int32(s.goodMask), Int32(stepGoodMask ?? -1), out)
        }
    }

    /// A PASS (perevod): the shield travels with the transfer card. `beat` names
    /// who laid cards; the kernel decides which of them was a transfer, because
    /// an attack and a pass are the same event type and only the rules tell them
    /// apart. `finalDefender` is the bubble's FINAL board, the only place in a
    /// stream the new defender ever appears.
    public static func passHandOff(shown: RoleMarks?, beat: AnimBeats.Beat,
                                   finalDefender: Int) -> RoleMarks? {
        ask(shown) { s, out in
            fio_roles_pass_hand_off(Int32(s.defender), Int32(s.firstAttacker),
                                    Int32(s.goodMask), Int32(beat.attackPassSeats),
                                    Int32(finalDefender), out)
        }
    }

    /// A board with nothing on its badges yet has nothing to change, which is
    /// the one case the kernel is not asked about at all.
    private static func ask(_ shown: RoleMarks?,
                            _ call: (RoleMarks, UnsafeMutablePointer<Int32>) -> Int32)
        -> RoleMarks? {
        guard let shown else { return nil }
        var triple = [Int32](repeating: 0, count: Int(FIO_ROLES_OUT))
        let changed = triple.withUnsafeMutableBufferPointer { p in
            call(shown, p.baseAddress!)
        }
        return changed == 1 ? RoleMarks(triple) : nil
    }
}
