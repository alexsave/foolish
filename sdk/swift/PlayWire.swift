// PlayWire.swift - what a gesture on a board means, asked of the kernel.
//
// The rules between a finger and a move used to be Swift
// (FoolishKit/Boards/CardPlay.swift): which legal move a drop resolves to,
// which battles a selection could cover, which one the Cover button aims at,
// which moves a human may make at all. They are the same answers on any screen,
// so they are C now (c/src/legal.c, `play_*`) and this is the crossing.
//
// THE MENU IS AN INPUT, not something the kernel re-derives. A board renders a
// PUBLISHED pair - the menu it was handed and the table it was handed - and the
// iMessage board deliberately publishes an EMPTY menu while it holds a bout
// settlement back. It also asks these questions from a SwiftUI body, which
// cannot await the actor the resident game lives behind. So the bytes travel
// down with the question, and `fio_play_probe` reads nothing else.

import Foundation
import CFoolish

/// Where a drag ended / what the player is aiming a selection at.
public enum PlayTarget: Equatable {
    /// Dropped back in the hand area - reorder/cancel, no move.
    case hand
    /// Dropped on the uncovered attack of battle `index` - a cover target.
    case battle(Int)
    /// Dropped on empty table space - an attack (attacker) or pass/auto-cover (defender).
    case table

    var wire: Int32 {
        switch self {
        case .hand:            return Int32(FIO_PLAY_TARGET_HAND)
        case .table:           return Int32(FIO_PLAY_TARGET_TABLE)
        case .battle(let i):   return Int32(i)
        }
    }
}

/// Everything a board needs to know about one selection, from ONE kernel call -
/// so a highlight it paints cannot disagree with the move a release then makes.
public struct PlayProbe: Equatable, Sendable {
    /// The move this gesture resolves to, or nil if it names nothing legal.
    public let move: Move?
    /// The battles this selection could legally cover - the drop-target highlight.
    public let coverable: Set<Int>
    /// The battle the Cover BUTTON aims at, or nil.
    public let bestCover: Int?
    public let canAttack: Bool
    public let canPass: Bool
    /// Whether to surface the "Good" (finish attacking) button: the kernel menu
    /// always offers it, a human may not use it over an uncovered attack.
    public let canSayGood: Bool

    public var canCover: Bool { !coverable.isEmpty }

    static let none = PlayProbe(move: nil, coverable: [], bestCover: nil,
                                canAttack: false, canPass: false, canSayGood: false)
}

public enum PlayWire {

    /// Ask the kernel about a selection on this board. `menu` is the packed
    /// legal-move wire the seat was handed (`fio_legal_packed` bytes, or
    /// `MoveWire.emptyMenu` for a board that is offering nothing).
    public static func probe(menu: Data, battles: [BattleView], powerSuit: Int,
                             isDefender: Bool, selection: [Card],
                             target: PlayTarget) -> PlayProbe {
        let table = tableWire(battles)
        let sel = selection.map(cardByte)
        var out = [CChar](repeating: 0, count: 1024)

        let n: Int32 = menu.withUnsafeBytes { m in
            table.withUnsafeBufferPointer { t in
                sel.withUnsafeBufferPointer { s in
                    fio_play_probe(m.bindMemory(to: UInt8.self).baseAddress, Int32(menu.count),
                                   t.baseAddress, Int32(battles.count),
                                   Int32(powerSuit), isDefender ? 1 : 0,
                                   s.baseAddress, Int32(sel.count), target.wire,
                                   &out, Int32(out.count))
                }
            }
        }
        let head = Int(FIO_PLAY_PROBE_HEAD)
        guard n >= Int32(head + 4) else { return .none }

        let bytes = out.prefix(Int(n)).map { UInt8(bitPattern: $0) }
        let flags = bytes[0]
        let best = Int(Int8(bitPattern: bytes[1]))
        var mask: UInt64 = 0
        for i in 0..<8 { mask |= UInt64(bytes[2 + i]) << (8 * i) }
        var coverable: Set<Int> = []
        for i in 0..<64 where mask & (UInt64(1) << i) != 0 { coverable.insert(i) }

        return PlayProbe(move: MoveWire.decode(Data(bytes[head...])).first,
                         coverable: coverable,
                         bestCover: best >= 0 ? best : nil,
                         canAttack: flags & 1 != 0,
                         canPass: flags & 2 != 0,
                         canSayGood: flags & 4 != 0)
    }

    /// The moves a HUMAN may make on this board: the kernel's menu minus `wait`,
    /// minus `good` while any attack is still uncovered. For the callers that
    /// ask "can this seat do anything at all" rather than "is this one button
    /// live" - a turn handoff reading the raw menu hands the game to a seat
    /// whose only offer is a good the board will not let it make.
    public static func humanMoves(menu: Data, battles: [BattleView]) -> [Move] {
        let table = tableWire(battles)
        var cap = 8 * 1024
        while true {
            var out = [CChar](repeating: 0, count: cap)
            let n: Int32 = menu.withUnsafeBytes { m in
                table.withUnsafeBufferPointer { t in
                    fio_play_human_menu(m.bindMemory(to: UInt8.self).baseAddress, Int32(menu.count),
                                        t.baseAddress, Int32(battles.count), &out, Int32(cap))
                }
            }
            if n >= 0 { return MoveWire.decode(Data(out.prefix(Int(n)).map { UInt8(bitPattern: $0) })) }
            guard n == -3, cap < (1 << 21) else { return [] }   // FIO_ECAP
            cap *= 2
        }
    }

    // MARK: - the wire

    /// Card byte: suit*13 + value-1, the same numbering the move wire uses.
    /// 0xFE is the kernel's "no card", which is what an uncovered battle carries.
    private static func cardByte(_ c: Card) -> UInt8 {
        c.isHidden ? 0xFE : UInt8(c.s * 13 + (c.v - 1))
    }

    /// The table as the kernel reads it: two bytes per battle, the attack then
    /// its cover or the no-card sentinel.
    private static func tableWire(_ battles: [BattleView]) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(battles.count * 2 + 2)
        for b in battles {
            out.append(cardByte(b.attack))
            out.append(b.defense.map(cardByte) ?? 0xFE)
        }
        out.append(0xFE)   // never read; keeps baseAddress non-nil for an empty table
        return out
    }
}
