// MaskedView.swift - a board as one viewer sees it, READ BY THE KERNEL.
//
// This file used to walk view.c's state_put layout in Swift: 60 lines of
// offsets, counts and card bytes sitting beside the C that writes them, kept in
// step by hand, and deriving the game-over rule of its own accord while it was
// there. The kernel has had a reader for that wire since Phase 5a - the client
// slot, c/src/client_table.h, which is what the WEB reads its boards with - so
// the walk is gone and this is the crossing: adopt into the slot, copy the
// TableView out through the generated snapshot reader, hand back the GameView
// the app's views are written against.
//
// WHAT THAT MOVED INTO C, besides the offsets: which seat is the fool
// (fool_of), which hands a viewer may see, and - on the envelope path - that
// the roster's status outranks the blob's copy of it. Each of those was a rule
// this file answered on its own.
//
// A refused board is nil. The slot refuses rather than clamps (its header says
// so), so a payload that does not read whole never becomes a board with a
// plausible wrong number on it.

import Foundation
import CFoolish

public enum MaskedView {

    /// Decode the packed masked state for `viewer` into a GameView. Player names
    /// are NOT on this wire (identity lives in the roster); callers merge them.
    /// Returns nil on a short or malformed buffer.
    public static func decode(_ bytes: Data, viewer: Int) -> GameView? {
        let rc: Int32 = bytes.withUnsafeBytes { raw in
            fio_view_of_state(raw.bindMemory(to: UInt8.self).baseAddress, Int32(bytes.count), Int32(viewer))
        }
        return rc == Int32(CLIENT_OK) ? current() : nil
    }

    /// The RESIDENT game as `viewer` sees it, with no bytes in between: the
    /// kernel masks its own board and reads it back. -1 for the spectator view.
    public static func resident(viewer: Int) -> GameView? {
        fio_view_of_resident(Int32(viewer)) == Int32(CLIENT_OK) ? current() : nil
    }

    /// A server response envelope (the create response, the player_views.view
    /// column): the board AND the table's identity, which is the roster trailer
    /// riding behind it. Names and is_ai come back on the seats.
    public static func envelope(_ bytes: Data) -> GameView? {
        let rc: Int32 = bytes.withUnsafeBytes { raw in
            fio_view_of_envelope(raw.bindMemory(to: UInt8.self).baseAddress, Int32(bytes.count))
        }
        return rc == Int32(CLIENT_OK) ? current() : nil
    }

    /// THE WHOLE VIEW the last adopt filled, snapshot and all - for the callers
    /// that need what a GameView does not carry: the table's id and title, the
    /// seats' ids, which of them are bots, and the committed version. nil when
    /// the view does not read whole, which KernelLayout makes unreachable.
    public static func table() -> TableViewSnap? {
        guard let p = fio_view_ptr() else { return nil }
        return try? readTableView(p)
    }

    /// The view the kernel just filled, as the app's model.
    private static func current() -> GameView? { table().map(GameView.init(kernel:)) }
}

public extension GameView {
    /// The kernel's TableView as the app's board. The ONE place these two
    /// shapes meet: everything above it is generated, everything below it is
    /// what the SwiftUI views are written against.
    init(kernel v: TableViewSnap) {
        let battles = v.battles.map { b in
            BattleView(attack: Card(s: b.attack.suit, v: b.attack.value),
                       // CARD_NONE (value -2) is an attack nobody has covered.
                       defense: b.defense.value < 0 ? nil : Card(s: b.defense.suit, v: b.defense.value))
        }
        let hand = v.mySeat >= 0 ? v.myHand.map { Card(s: $0.suit, v: $0.value) } : nil
        let players = v.seats.enumerated().map { (seat, s) in
            PlayerView(seat: seat, name: s.name, status: s.status, handCount: s.handCount,
                       awaitingAttack: s.awaitingAttack,
                       // The board carries no strategy key - a seat is a bot or
                       // not (ViewSeat.is_ai), and nothing renders the ladder.
                       strategyKey: 0,
                       hand: seat == v.mySeat ? hand : nil)
        }
        self.init(status: v.status, numPlayers: v.seats.count, powerSuit: v.powerSuit,
                  deckCount: v.deckCount, discardCount: v.discardPileLength,
                  hasFlipped: v.hasFlipped, firstAttacker: v.firstAttacker,
                  defender: v.defender, viewer: v.mySeat, goodMask: v.goodMask,
                  // The fool, once there is one: the kernel's own game_done
                  // rule, which this file used to restate from the statuses.
                  gameOver: v.fool,
                  flipped: v.hasFlipped ? Card(s: v.flipped.suit, v: v.flipped.value) : nil,
                  battles: battles, eliminationOrder: v.elimination, players: players)
    }
}
