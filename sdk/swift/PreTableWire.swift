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
            // redacted card names nothing and is simply not listed.
            let ids = ev.cards.compactMap { $0 }.filter { !$0.isHidden }.map(Self.id)
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
            table.append(BattleView(attack: Self.card(b[at]),
                                    defense: cover == UInt8(FIO_PRETABLE_NONE)
                                             ? nil : Self.card(cover)))
        }
        self.battles = table
        self.paired = b[2] != 0
    }

    private init(battles: [BattleView], paired: Bool) {
        self.battles = battles
        self.paired = paired
    }

    /// One board as the wire's table: a count, then the attack and its cover (or
    /// the "no card" byte) per battle. A board with a REDACTED card on it cannot
    /// be described honestly, so it crosses as no board at all rather than as a
    /// table with an invented card in it - the kernel never masks a table, so
    /// this is a corrupt input rather than a case.
    private static func table(_ battles: [BattleView]?) -> [UInt8] {
        guard let battles, !battles.isEmpty, battles.count < Int(FIO_PRETABLE_NONE),
              battles.allSatisfy({ !$0.attack.isHidden && !($0.defense?.isHidden ?? false) })
        else { return [UInt8(FIO_PRETABLE_NONE)] }
        var out: [UInt8] = [UInt8(battles.count)]
        for b in battles {
            out.append(id(b.attack))
            out.append(b.defense.map(id) ?? UInt8(FIO_PRETABLE_NONE))
        }
        return out
    }

    private static func id(_ c: Card) -> UInt8 { UInt8(c.s * 13 + (c.v - 1)) }
    private static func card(_ id: UInt8) -> Card {
        Card(s: Int(id) / 13, v: Int(id) % 13 + 1)
    }
}
