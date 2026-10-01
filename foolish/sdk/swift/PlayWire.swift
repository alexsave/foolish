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

    /// What a host knows about its own screen and transport, for the pills
    /// (legal.h PLAY_HOST_*) - the facts no board carries. Every fact about the
    /// board itself is the kernel's to read off the view.
    public struct Host: OptionSet, Sendable, Equatable {
        public let rawValue: UInt32
        public init(rawValue: UInt32) { self.rawValue = rawValue }

        /// A move is staged, waiting on Messages' Send.
        public static let staged     = Host(rawValue: UInt32(PLAY_HOST_STAGED))
        /// A move of mine is between the tap and the kernel's answer.
        public static let inFlight   = Host(rawValue: UInt32(PLAY_HOST_IN_FLIGHT))
        /// The board is still animating and refuses a play.
        public static let moving     = Host(rawValue: UInt32(PLAY_HOST_MOVING))
        /// A newer chain stood this seat down (round 20).
        public static let superseded = Host(rawValue: UInt32(PLAY_HOST_SUPERSEDED))
        /// The throw-in hold before a defender may take (round 16).
        public static let pickupHeld = Host(rawValue: UInt32(PLAY_HOST_PICKUP_HELD))
    }

    /// Ask the kernel about a selection on the board `view` shows, under the
    /// menu the seat was handed. Which seat defends is the kernel's to decide
    /// from the view's own fields.
    public static func probe(menu: Data, view: GameView, selection: [Card],
                             target: PlayTarget) -> PlayProbe {
        probe(menu: menu, battles: view.battles, powerSuit: view.powerSuit,
              mySeat: view.viewer, defender: view.defender,
              selection: selection, target: target)
    }

    /// The same question over a bare table. `menu` is the packed legal-move
    /// wire the seat was handed (`fio_legal_packed` bytes, or
    /// `MoveWire.emptyMenu` for a board that is offering nothing); `mySeat` and
    /// `defender` are the view's seats, -1 for a spectator.
    public static func probe(menu: Data, battles: [BattleView], powerSuit: Int,
                             mySeat: Int, defender: Int, selection: [Card],
                             target: PlayTarget) -> PlayProbe {
        let table = TableWire.encode(battles)
        let sel = selection.map(selectionByte)
        var out = [CChar](repeating: 0, count: 1024)

        let n: Int32 = menu.withUnsafeBytes { m in
            table.withUnsafeBufferPointer { t in
                sel.withUnsafeBufferPointer { s in
                    fio_play_probe(m.bindMemory(to: UInt8.self).baseAddress, Int32(menu.count),
                                   t.baseAddress, Int32(table.count / 2),
                                   Int32(powerSuit), Int32(mySeat), Int32(defender),
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
                         canAttack: flags & UInt8(PLAY_ANSWER_ATTACK) != 0,
                         canPass: flags & UInt8(PLAY_ANSWER_PASS) != 0,
                         canSayGood: flags & UInt8(PLAY_ANSWER_GOOD) != 0)
    }

    /// The moves a HUMAN may make on this board: the kernel's menu minus `wait`,
    /// minus `good` while any attack is still uncovered. For the callers that
    /// ask "can this seat do anything at all" rather than "is this one button
    /// live" - a turn handoff reading the raw menu hands the game to a seat
    /// whose only offer is a good the board will not let it make.
    public static func humanMoves(menu: Data, battles: [BattleView]) -> [Move] {
        let table = TableWire.encode(battles)
        var cap = 8 * 1024
        while true {
            var out = [CChar](repeating: 0, count: cap)
            let n: Int32 = menu.withUnsafeBytes { m in
                table.withUnsafeBufferPointer { t in
                    fio_play_human_menu(m.bindMemory(to: UInt8.self).baseAddress, Int32(menu.count),
                                        t.baseAddress, Int32(table.count / 2), &out, Int32(cap))
                }
            }
            if n >= 0 { return MoveWire.decode(Data(out.prefix(Int(n)).map { UInt8(bitPattern: $0) })) }
            guard n == -3, cap < (1 << 21) else { return [] }   // FIO_ECAP
            cap *= 2
        }
    }

    /// WHICH PILLS THE BOARD DRAWS (legal.h play_pills): the PLAY_PILL_* bits
    /// for `selection` on the board `view` shows, under the seat's `menu` and
    /// the host's own `host` bits. ONE kernel call: who defends, the table, the
    /// selection, out of play and whether the seat has a move at all are read
    /// off the view there, never here. The view crosses as its own fields.
    public static func pills(menu: Data, view: GameView, selection: [Card],
                             host: Host) -> UInt32 {
        let table = TableWire.encode(view.battles)
        let sel = selection.map(selectionByte)
        return menu.withUnsafeBytes { m in
            table.withUnsafeBufferPointer { t in
                sel.withUnsafeBufferPointer { s in
                    UInt32(fio_play_pills(m.bindMemory(to: UInt8.self).baseAddress, Int32(menu.count),
                                          t.baseAddress, Int32(table.count / 2), Int32(view.powerSuit),
                                          Int32(view.viewer), Int32(view.defender),
                                          Int32(view.me?.status ?? -1), Int32(view.status),
                                          s.baseAddress, Int32(sel.count), host.rawValue))
                }
            }
        }
    }

    // MARK: - the wire

    // THE TABLE IS THE KERNEL'S TO WRITE (TableWire). This file used to write
    // it, and spelled a card the viewer may not see as the no-card byte - which
    // read back as an OPEN battle: Good withheld over a covered table, a drop
    // target offered on a closed battle, and nothing refusing because the wire
    // was well-formed. The kernel's rule for that cell is stated once, in
    // ios_api.h beside the bytes.

    /// A selected card as the byte the menu names it by. A card that cannot be
    /// named crosses as the same unnameable byte the table uses, so it equals
    /// no menu card and the selection resolves to nothing - the only answer a
    /// question about a card nobody can see can have.
    private static func selectionByte(_ c: Card) -> UInt8 {
        CardSet.id(of: c) ?? UInt8(FIO_TABLE_UNKNOWN)
    }
}
