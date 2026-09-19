// THE RIG'S AUTO-PLAYER - FoolishHarness only, and DEBUG only.
//
// It plays through the SAME entry points a human tap hits (`playAt` /
// `playCover`), never `play(_:)` directly, so an auto-run exercises the real
// placement path - the preHide and the hand->table flight - which is where the
// "ghost card / cards jump" bugs live.

import SwiftUI
import Foundation

extension MessageTableView {

#if DEBUG
    /// WHICH of the moves a human could make here the rig plays: the first one,
    /// as it always has, unless `HARNESS_AUTOMOVE_KIND` names a type that is on
    /// the menu.
    ///
    /// ROUND 29 added the knob for the TRANSFER, and it is the only reason it
    /// exists. A pass is never the only thing a defender may do - it is offered
    /// beside the covers and the pickup, and the kernel lists the covers first -
    /// so the auto-player could not stage one, and Channel A of the pass shape
    /// (the one channel that does NOT replay a kernel stream, see
    /// `passHandOff`) had no way to be watched at all.
    static func autoPick(_ moves: [Move]) -> Move? {
        guard let want = ProcessInfo.processInfo.environment["HARNESS_AUTOMOVE_KIND"],
              let kind = MoveType(rawValue: want) else { return moves.first }
        // Falls back to the first move rather than refusing: a rig run that
        // reached a board where the wanted move is not legal should still play
        // something, and say so in the trace, instead of standing still and
        // reading as a hang.
        return moves.first { $0.type == kind } ?? moves.first
    }

    /// FoolishHarness only: play the first move a HUMAN could make here, through
    /// the same entry points a tap hits.
    ///
    /// Round 22 pulled this out of the mount `.task` so it can also be run after
    /// an ARRIVAL. An arrival folds into the live controller and keeps the
    /// board's identity - which is the whole point of round 12 - so `.task`
    /// never fires again, and the rig could stage a move only on the very first
    /// board it ever mounted. That made "somebody moves, then I reply and send"
    /// unreachable, which is exactly the sequence both 1.0(23) reports describe.
    /// `waitForBoard` polls until this board actually has a move to make.
    /// `adopt` bumps `arrivalTick` BEFORE it awaits `begin()`, so the task keyed
    /// on that tick runs while the controller is between chains - published
    /// menu still empty - and a single read there sees nothing to play. The
    /// mount call does not wait: a board that opens with no move for me is the
    /// ordinary "your opponent's turn" case, and spinning there would race the
    /// arrival's own auto-play for the same move.
    /// The moves a human may make on THIS board right now - the published menu
    /// narrowed by the kernel's own human rule (no `wait`, no `good` over an
    /// uncovered attack). The dev auto-player and HarnessModel's turn handoff
    /// have to agree on it: a handoff reading the raw menu passes the game to a
    /// seat whose only offer is a good this board will not let it make.
    ///
    /// The CONTROLLER narrows it, off the same bytes and in the same breath as
    /// the board they describe (`humanLegal`), so this board and `iCanAct`
    /// cannot be looking at two different menus.
    private func humanMoves() -> [Move] { controller.humanLegal }

    func autoPlayIfAsked(waitForBoard: Bool = false) async {
        let devAutoMove = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: "group.cards.foolish.msg")
            .map { FileManager.default.fileExists(atPath: $0.appendingPathComponent("dev.automove").path) }
            ?? false
        let asked = ProcessInfo.processInfo.environment["HARNESS_AUTOMOVE"] != nil || devAutoMove
        AnimLog.say("automove enter asked=\(asked) tick=\(controller.arrivalTick) "
            + "ready=\(controller.ready) hold=\(controller.pickupHold) legal=\(controller.legal.count) "
            + "human=\(humanMoves().count)")
        if asked, waitForBoard {
            let settle = Date().addingTimeInterval(20)
            while Date() < settle, humanMoves().isEmpty {
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
            AnimLog.say("automove board settled human=\(humanMoves().count)")
        }
        // WAIT OUT THE PICKUP HOLD rather than working around it. While the
        // round-16 hold stands the Take pill is not on screen (FActionBar's
        // `canPickup`) and `apply` refuses the move, so an auto-run that picked
        // it staged nothing and reported "nothing staged" - which reads as a rig
        // bug and is really the rig tapping a button that is not there. Skipping
        // the move instead would be worse: a pickup after somebody attacks me is
        // one of the commonest turns in the game, and a rig that can never play
        // it can never test it. So do what a player does - wait, then take.
        // HARNESS_AUTOMOVE_NOWAIT (round 42): DON'T wait - play into whatever
        // is on screen this instant, hold and running animation included. It is
        // the only way the rig can pose "the human drags a card while the open
        // replay is still flying", which no other knob reaches: every other
        // auto-play path settles first, and that is exactly the moment a live
        // play and a running stream can fight over the frozen counts (see
        // `releaseCounts`). Never set outside the rig.
        let nowait = ProcessInfo.processInfo.environment["HARNESS_AUTOMOVE_NOWAIT"] != nil
        if asked, !nowait {
            let waitUntil = Date().addingTimeInterval(20)
            while controller.pickupHold > 0, Date() < waitUntil {
                try? await Task.sleep(nanoseconds: 250_000_000)
            }
        }
        if asked,
           let view = controller.view,
           let m = Self.autoPick(humanMoves()) {
            AnimLog.say("automove: playing \(m.type) (hold=\(controller.pickupHold))")
            // Let the incoming replay (the OTHER player's last move flying
            // deck/seat→table on open) finish and rest so it's watchable,
            // THEN auto-play our move. Dev pacing only.
            let pace = Double(ProcessInfo.processInfo.environment["HARNESS_PACE"] ?? "") ?? 1
            try? await Task.sleep(nanoseconds: UInt64(2.4 * pace * 1_000_000_000))
            // Route through the SAME entry points a human tap hits (playAt /
            // playCover), not `play(m)` directly - so an auto-run exercises the
            // real placement path (preHide + the hand→table flight), which is
            // where the "ghost card / cards jump" bugs live. `play(m)` skips
            // all of that, so an auto-run of it can never reproduce them.
            switch m.type {
            case .attack, .pass: playAt(.table, m.cards, view)
            case .cover:         playCover(m.cards, view)
            case .pickup:        play(.pickup)
            case .good:          play(.good)
            default:             play(m)
            }
            // HARNESS_AUTOUNDO: after auto-playing, wait for the move to settle
            // (and the drawer to auto-collapse, if it does), then undo it - so a
            // run reproduces the "undo double animation / fade" the overlay-less
            // undo path shows, without a human tapping Undo.
            if ProcessInfo.processInfo.environment["HARNESS_AUTOUNDO"] != nil {
                try? await Task.sleep(nanoseconds: UInt64(3.0 * pace * 1_000_000_000))
                await controller.undo()
            }
        }
        }
#endif
}
