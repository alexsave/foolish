// THE SEATS ON THE RING, AND THE NAMES ON THEM - the opponents' badges, where
// they sit, and the order they finish in.
//
// The local player is visual-index 0 (bottom centre) and is drawn as the hand,
// so the ring is the OTHER seats; a spectator holds no seat and takes the
// public board's convention instead.

import SwiftUI
import Foundation   // sin/cos for the ring placement

extension MessageTableView {

    /// The finish order for the end screen: rank 1 = first player out (best),
    /// counting up to the fool last. `eliminationOrder` is first-out first and
    /// holds everyone except the fool; the fool is `view.gameOver` (the one seat
    /// still holding cards), given the last place. Mirrors web WinScreen.
    func finishRows(_ view: GameView) -> [FinishRow] {
        Self.finishRows(view, names: controller.names, mySeat: controller.mySeat)
    }

    /// …with the NAMES attached, which is the only half of a result screen that
    /// is this client's. The ORDER is `FinishOrder.places`
    /// (c/src/anim_plan.c's anim_finish_rows), so the spectator screen ranks a
    /// finished game the same way (round 20: "spectators should still be able to
    /// see win screen") and cannot disagree with the player's about who was the
    /// fool. A spectator holds no seat, which `mySeat: -1` says.
    static func finishRows(_ view: GameView, names: [Int: String], mySeat: Int) -> [FinishRow] {
        FinishOrder.places(view, mySeat: mySeat).map {
            FinishRow(place: $0.place, total: $0.total,
                      name: names[$0.seat] ?? "Seat \($0.seat + 1)", isYou: $0.isYou)
        }
    }

    private func name(_ seat: Int) -> String { controller.names[seat] ?? "Seat \(seat + 1)" }

    /// One opponent seat badge, publishing its frame in `boardSpace` so bout-end
    /// flights can target it. Placed on the ring by `ringPoint`.
    func opponentSeat(_ p: PlayerView, _ view: GameView) -> some View {
        #if DEBUG
        // Which mark this seat is WEARING, and what it was derived from. The
        // counterpart to `traceGrid`, and there for the same reason: a role mark
        // that fails to appear is "which of these four inputs said no", and the
        // board is the only thing that can answer. Deduped per seat, so a board
        // at rest is silent.
        Self.traceMark(seat: p.seat, defender: shownIsDefender(p.seat, view),
                       attacker: showsSword(seat: p.seat, isOut: p.isOut, view),
                       good: shownSaidGood(p.seat, view), out: p.isOut,
                       flying: roleDepartingSeats.contains(p.seat)
                            || roleArrivingSeats.contains(p.seat),
                       roles: shownRoles(view),
                       battles: view.battles.count, sweep: sweepBattles.count)
        #endif
        return FSeatBadge(name: name(p.seat),
                   handCount: shownHandCount(p),
                   // Round 16: the ROLE the board is currently showing, which
                   // during a bout-end sequence is still the one from before the
                   // move - the marks change when their flight plays, not when
                   // the view carrying the new roles arrives.
                   isDefender: shownIsDefender(p.seat, view),
                   isAttacker: showsSword(seat: p.seat, isOut: p.isOut, view),
                   saidGood: shownSaidGood(p.seat, view),
                   isOut: p.isOut,
                   seat: p.seat,
                   // Round 20: the seat that opens the bout wears the tinted
                   // sword. Asked of the SHOWN roles like every other mark on
                   // this badge, so the tint moves with the hand-off rather than
                   // a beat before it.
                   opensBout: shownRoles(view).firstAttacker == p.seat,
                   markDeparting: roleDepartingSeats.contains(p.seat),
                   markArriving: roleArrivingSeats.contains(p.seat),
                   // Round 28: edge-on once this seat is out - asked of what the
                   // board is SHOWING, like every other lagging value on this
                   // badge, so the collapse rides the move that caused it.
                   collapsed: shownOut(p.seat, view))
        // `SeatFramesKey` comes from INSIDE the badge now, off its mini fan -
        // see FSeatBadge for why. Measuring the whole badge here put every
        // seat's flight pad about 13pt under the cards it is supposed to be
        // about, because the badge's role row outweighs its name.
    }

    /// A seat's centre on the web's 35% ellipse (PlayerRing): the local player is
    /// visual-index 0 → bottom centre (drawn as the hand); opponents fan clockwise.
    /// Percentages resolve against width vs height, which reads as an oval on a
    /// non-square board - exactly the web's trick.
    func ringPoint(seat: Int, n: Int, in size: CGSize) -> CGPoint {
        // A seatless viewer has no bottom-centre to be at, so the seats are
        // their own visual order - the same convention the public bubble board
        // uses (MessageBoardView.ringPoint), which is what a spectator has been
        // looking at up to this point.
        let visual = isSpectating ? seat % n : (seat - controller.mySeat + n) % n
        let rad = 2 * Double.pi * Double(visual) / Double(max(n, 1))
        // A compressed board (the compact drawer) pushes the ring a little higher
        // so it and the hand don't crowd the middle - but only a little, or the
        // top badge clips against the drawer's rounded top edge. Round-6: this
        // now rides the SAME continuous collapse fraction as everything else
        // (0.35 expanded -> 0.38 fully compact), so the ring eases open as the
        // drawer collapses rather than jumping at a single height (bugs 2/4).
        let ry = 0.35 + 0.03 * Self.collapseFraction(height: size.height)
        // Wider than it is tall (0.42 vs 0.35). At eight players the battle
        // grid wraps to four across and the side seats sat right on top of it -
        // Oleg's and Dima's names were behind the cards. Pushing the ring out
        // horizontally is the only room there is: the vertical radius is
        // already fighting the hand and the drawer's rounded top edge, and a
        // badge is ~70pt wide against a 375pt board, so 0.42 leaves ~22pt of
        // margin at the widest seats and no more is available. This does not
        // make eight players roomy - the owner's read, and it is right - it
        // just stops the collision being the first thing you see.
        let x = (-sin(rad) * 0.42 + 0.5) * size.width
        let y = ( cos(rad) * ry + 0.5) * size.height
        return CGPoint(x: x, y: y)
    }
}
