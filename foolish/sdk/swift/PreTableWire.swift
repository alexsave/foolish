// PreTableWire.swift - the table a bout end sweeps, asked of the kernel.
//
// A board opening on a pickup or a discard has to lay out the table the sweep
// is about to take, so each card flies from where it actually sat. The settled
// board it holds is already empty, so that table has to come from the stream.
//
// The rule used to be Swift (MessageTurnController.preBoutTable) and could not
// answer the hard half of it: a pickup crosses as a flat card list, so the
// Swift reading laid every card in its own uncovered cell and a table of two
// battles with one covered came back as three. The board renders the sweep
// through the same grid the live table used, so a differently shaped table
// animates every card into its new cell BEFORE anything flies off it - round
// 12's "they did not animate directly from their table positions, but seemed to
// spread out to an evenly spaced row, AND THEN fly to the hand".
//
// The kernel has what Swift did not: every step carries the board it committed,
// and a board keeps its battles paired. So the rule is now c/src/anim_plan.c's
// anim_pre_bout_table and this is the crossing. The FLAT reading survives - a
// pickup that leads its stream carries no earlier board at all - but it comes
// back marked (`paired == false`) instead of passing itself off as a table.
//
// THE STREAM IS AN INPUT, for BeatWire's reason: a board animates the stream it
// was handed, often half a bubble, and asks from a SwiftUI body that cannot
// await the actor the resident game lives behind.

import Foundation
import CFoolish

public struct PreBoutTable: Equatable, Sendable {
    /// The table, in its real left-to-right order.
    public let battles: [BattleView]
    /// True when the pairing came off a board the kernel really had. False when
    /// it is the flat reading - the right cards in a shape nobody vouched for.
    /// A caller choosing between two tables must not take the second for one.
    public let paired: Bool

    public static let empty = PreBoutTable(battles: [], paired: false)

    /// Ask the kernel which table `events` sweeps. `prior` is the board the
    /// stream opened on, the only place a single-action pickup turn's table
    /// still exists.
    public init(_ events: [GameEvent], prior: GameView? = nil) {
        // A stream longer than the kernel's cap is refused there and answers
        // `.empty` - the same degrade-to-less-animation discipline BeatWire
        // keeps over the same streams, and no bubble comes near it.
        let evs = Array(events.prefix(255))
        var input: [UInt8] = [UInt8(FIO_PRETABLE_VERSION), UInt8(evs.count)]
        input.append(contentsOf: Self.table(prior?.battles))
        for ev in evs {
            input.append(UInt8(truncatingIfNeeded: ev.type))
            input.append(contentsOf: Self.table(ev.state?.battles))
            // A pickup's cards ARE its table and the kernel never masks one; a
            // redacted card names nothing and is simply not listed. A LIST, not
            // a table - so the dense id, with nothing off the deck listed.
            let ids = ev.cards.compactMap { $0 }.compactMap(CardSet.id(of:))
            input.append(UInt8(min(ids.count, 255)))
            input.append(contentsOf: ids.prefix(255))
        }

        var out = [CChar](repeating: 0, count: Int(FIO_PRETABLE_HEAD) + 2 * 128)
        let n: Int32 = input.withUnsafeBufferPointer { p in
            fio_pre_bout_table_packed(p.baseAddress, Int32(input.count), &out, Int32(out.count))
        }
        guard n >= Int32(FIO_PRETABLE_HEAD) else { self = .empty; return }
        let b = out.prefix(Int(n)).map { UInt8(bitPattern: $0) }
        let count = Int(b[1])
        guard b.count >= Int(FIO_PRETABLE_HEAD) + 2 * count else { self = .empty; return }

        var table: [BattleView] = []
        table.reserveCapacity(count)
        for i in 0..<count {
            let at = Int(FIO_PRETABLE_HEAD) + 2 * i
            let cover = b[at + 1]
            // Dense ids only: the kernel reads a board with an unnameable cell
            // as no board (ios_api.h), so nothing off the deck reaches here.
            table.append(BattleView(attack: CardSet.card(b[at]),
                                    defense: cover == UInt8(FIO_PRETABLE_NONE)
                                             ? nil : CardSet.card(cover)))
        }
        self.battles = table
        self.paired = b[2] != 0
    }

    private init(battles: [BattleView], paired: Bool) {
        self.battles = battles
        self.paired = paired
    }

    /// One board as the wire's table: a count, then the kernel's own table
    /// bytes (TableWire). A board holding a card the viewer may not see is
    /// sent as it is - the unnameable cell and all - and it is the KERNEL that
    /// reads such a board as no board, for the reason it states in ios_api.h:
    /// this answer is laid out by identity, and that cell has none. This used
    /// to be decided here, by sending "no board" in its place; the decision has
    /// not changed, only who makes it, so that four table writers could become
    /// one and the rule could live beside the bytes.
    ///
    /// Shared with AnimPlanWire, which sends every step's row for the same
    /// reason this sends the prior board: the row a stream OPENS on is a fact
    /// about the stream, and one encoding of it means the two wires cannot
    /// disagree about what a table is.
    static func table(_ battles: [BattleView]?) -> [UInt8] {
        guard let battles, !battles.isEmpty, battles.count < Int(FIO_PRETABLE_NONE)
        else { return [UInt8(FIO_PRETABLE_NONE)] }
        let bytes = TableWire.encode(battles)
        guard !bytes.isEmpty else { return [UInt8(FIO_PRETABLE_NONE)] }
        return [UInt8(bytes.count / 2)] + bytes
    }
}

// THE TABLE UNDER A SWEEP - which cards are on it, whether one table accounts
// for another, and which of the three candidates the grid actually paints.
//
// These were `MessageTableView.sweepIds` / `coveredSweep` / `shownTable`, and
// they belong beside `PreBoutTable` because they are the choices made ABOUT its
// answer: three call sites were choosing between two tables, and the subset test
// they each wrote out by hand had to mean the same thing at all of them or a
// covered pair drops off the table mid-sweep. The rule is
// c/src/anim_plan.c's anim_table_* / anim_covered_sweep_accepts /
// anim_shown_table now, so it means one thing.
/// The pass preview's empty slot - the kernel's `anim_pass_slot_shown`.
public enum PassSlotWire {
    public static func shown(previewing: Bool, dragging: Bool, seenThisDrag: Bool,
                             overDeadPair: Bool, heldAt: Int?, battles: Int,
                             hold: Bool, sticky: Bool) -> Bool {
        let rules = (hold ? FIO_PASS_HOLD : 0) | (sticky ? FIO_PASS_STICKY : 0)
        return fio_pass_slot_shown(previewing ? 1 : 0, dragging ? 1 : 0, seenThisDrag ? 1 : 0,
                                   overDeadPair ? 1 : 0, Int32(heldAt ?? -1), Int32(battles),
                                   Int32(rules)) == 1
    }
}

public extension PreBoutTable {

    /// The identities a battle table holds - each attack, and its cover where it
    /// has one.
    static func cardIds(_ battles: [BattleView]) -> Set<String> {
        let bytes = wire(battles)
        return CardSet.identities(bytes.withUnsafeBufferPointer {
            fio_table_card_ids($0.baseAddress, Int32(bytes.count / 2))
        })
    }

    /// Does `outer` account for every card on `inner`? The one subset test the
    /// table choices rest on.
    static func covers(_ outer: [BattleView], _ inner: [BattleView]) -> Bool {
        let a = wire(outer), b = wire(inner)
        return a.withUnsafeBufferPointer { ap in
            b.withUnsafeBufferPointer { bp in
                fio_table_covers(ap.baseAddress, Int32(a.count / 2),
                                 bp.baseAddress, Int32(b.count / 2)) == 1
            }
        }
    }

    /// The table a bout-ending COVER should be swept off: the kernel's covered
    /// table, the same one a receiver's open replay lays out, so both sides
    /// sweep the identical board. nil unless it is a real board the kernel had
    /// AND accounts for everything the live sweep already holds - the live sweep
    /// is the real prior view and is never wrong about which cards were on the
    /// table, so the swap is only ever earned by ADDING the cover to it.
    static func coveredSweep(_ events: [GameEvent], current: [BattleView]) -> [BattleView]? {
        let pre = PreBoutTable(events)
        let a = wire(pre.battles), b = wire(current)
        let ok = a.withUnsafeBufferPointer { ap in
            b.withUnsafeBufferPointer { bp in
                fio_covered_sweep_accepts(pre.paired ? 1 : 0,
                                          ap.baseAddress, Int32(a.count / 2),
                                          bp.baseAddress, Int32(b.count / 2)) == 1
            }
        }
        return ok ? pre.battles : nil
    }

    /// Which table the grid paints, and whether that is a sweep. Three sources
    /// in falling order of authority: the live table, the sweep a move of my own
    /// captured synchronously, and the pre-bout table of an open replay this
    /// board has not started (which exists only because an arrival publishes its
    /// view a paint before anything sets the sweep).
    static func shownTable(live: [BattleView], sweep: [BattleView],
                           pending: [BattleView], holdLeaving: Bool) -> (shown: [BattleView], sweeping: Bool) {
        // The same three sources, given as ROWS, so the kernel can see a card
        // leaving a table that stays (anim_shown_table_rows) - see
        // UndoHoldsTableTests for the undo that lost its card for 90ms.
        var sweeping: Int32 = 0
        let l = wire(live), w = wire(sweep)
        let which = l.withUnsafeBufferPointer { lp in
            w.withUnsafeBufferPointer { wp in
                fio_shown_table_rows(lp.baseAddress, Int32(l.count / 2), wp.baseAddress, Int32(w.count / 2),
                                     Int32(pending.count), holdLeaving ? 1 : 0, &sweeping)
            }
        }
        switch which {
        case FIO_SHOWN_LIVE:    return (live, sweeping != 0)
        case FIO_SHOWN_SWEEP:   return (sweep, sweeping != 0)
        case FIO_SHOWN_PENDING: return (pending, sweeping != 0)
        default:                return ([], false)
        }
    }

    static func shownTable(live: [BattleView], sweep: [BattleView],
                           pending: [BattleView]) -> (shown: [BattleView], sweeping: Bool) {
        var sweeping: Int32 = 0
        let which = fio_shown_table(Int32(live.count), Int32(sweep.count),
                                    Int32(pending.count), &sweeping)
        switch which {
        case FIO_SHOWN_LIVE:    return (live, sweeping != 0)
        case FIO_SHOWN_SWEEP:   return (sweep, sweeping != 0)
        case FIO_SHOWN_PENDING: return (pending, sweeping != 0)
        default:                return ([], false)
        }
    }

    /// A table as the kernel writes it (TableWire). This was the one of the
    /// four Swift table writers that had the masked case right - an empty cell
    /// and an unnameable card as different bytes, so a sweep over a card nobody
    /// can name is refused rather than certified - and that answer is now the
    /// kernel's for every table, stated in ios_api.h beside the bytes. The
    /// battle count each call below sends is `count / 2` of these bytes, never
    /// the array's, so a length can never outrun its buffer.
    private static func wire(_ battles: [BattleView]) -> [UInt8] { TableWire.encode(battles) }
}
