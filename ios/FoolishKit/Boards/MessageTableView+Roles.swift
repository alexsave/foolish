// THE ROLE MARKS - shield, sword and check: who is WEARING one right now, and
// which of them travel when the roles change hands.
//
// The marks LAG the game state: they stay where they are until the move that
// moved them has been watched (`shownRoles` over the ledger), and the two that
// belong to a seat and then belong to another one - the defender's shield and
// the opener's sword - are the only two that fly. Everything else a role change
// does is a gesture the mark makes where it stands.

import SwiftUI
import Foundation

extension MessageTableView {

    /// Does MY OWN role mark - shield/check/sword, `selfRoleIndicator` - draw
    /// right now? The two ways it can be silenced, in one place:
    ///
    /// - the end screen is up: the board is gone, so its decorations go too;
    /// - I am a spectator (round 21): a seatless viewer holds no role at all.
    ///
    /// Note that "I am OUT of the game" is NOT here. An out player still watches
    /// the rest of the game from their seat, and `selfRoleMark` already draws
    /// them no mark (`isOut ? nil`) while keeping the seat's landing pad
    /// published for role flights that are still crossing it.
    static func showsSelfRoleMark(isOver: Bool, showResults: Bool, isSpectating: Bool) -> Bool {
        if showsEndScreen(isOver: isOver, showResults: showResults) { return false }
        return !isSpectating
    }

    /// Does this seat wear the sword? An attacker keeps it until THEY say good,
    /// even once every attack on the table is covered — not "any uncovered
    /// battle exists", which erased every sword the instant the table filled up.
    ///
    /// But on an EMPTY table only ONE seat can act at all: the seat that opens
    /// the bout. Marking every non-defender then was just wrong — "when no cards
    /// are on the table, and only the first attacker can move, ONLY the first
    /// attacker gets the sword. Everyone else has no icon." Once the bout is
    /// open, throw-ins make the others attackers for real, and they get one.
    func showsSword(seat: Int, isOut: Bool, _ view: GameView) -> Bool {
        // Round 16: asked of the roles the board is SHOWING, not the ones the
        // kernel has moved on to - so a bout end does not re-cast every sword on
        // the table a beat before the sequence that earns it has played. The
        // battles are read live because they are already animated: the table
        // clears through the sweep, not through this.
        let roles = shownRoles(view)
        guard seat != roles.defender, !isOut, (roles.goodMask & (1 << seat)) == 0 else { return false }
        return view.battles.isEmpty && sweepBattles.isEmpty ? seat == roles.firstAttacker : true
    }

    /// My own role mark - shield/check/sword - mirroring FSeatBadge's roleRow for
    /// opponents (note 3: the local player never saw their own role before, only
    /// the special-cased first-attacker sword).
    ///
    /// WHEN IT DRAWS AT ALL is `showsSelfRoleMark`, and it is asked HERE rather
    /// than left to the caller. This comment used to claim the opposite - that
    /// "the game-over screen replaces the whole board, so 'game over' is already
    /// handled by the caller never reaching here" - and that was simply false:
    /// the caller is an `.overlay` on the container that WRAPS the board/results
    /// branch, so the swap to `FGameOverList` never touches it and the mark drew
    /// straight on over the finished game. A wrong comment is what kept the bug:
    /// it answered the exact question anyone auditing this would have asked.
    /// The predicate is a value now, so it can be tested, and so the end screen
    /// and this mark cannot hold different opinions about whether the game is
    /// over.
    @ViewBuilder
    func selfRoleIndicator(_ view: GameView) -> some View {
        // Both silencing rules live in `showsSelfRoleMark`. The spectator one is
        // round 21's: `showsSword` would answer true for seat -1 on any open
        // table (it is not the defender, it has not said good, and there are
        // cards down), so a watcher would be shown a sword of their own under an
        // empty hand.
        if Self.showsSelfRoleMark(isOver: controller.isOver, showResults: showResults,
                                  isSpectating: isSpectating) {
            selfRoleMark(view)
        } else {
            EmptyView()
        }
    }

    private func selfRoleMark(_ view: GameView) -> some View {
        let mySeat = controller.mySeat
        let isOut = view.me?.isOut ?? false
        let isDefender = shownIsDefender(mySeat, view)
        let saidGood = shownSaidGood(mySeat, view)
        let isAttacker = showsSword(seat: mySeat, isOut: isOut, view)
        #if DEBUG
        // ROUND 22: MY OWN mark is traced too. Only the opponent badges were,
        // so every question about the mark the player is actually looking at -
        // "I did not see the sword->good rotation" is one - had to be answered
        // by reasoning about the code instead of by reading the log. The one
        // seat whose mark is drawn from a different call site is the one seat
        // whose mark nobody could see.
        Self.traceMark(seat: mySeat, defender: isDefender, attacker: isAttacker,
                       good: saidGood, out: isOut,
                       flying: roleDepartingSeats.contains(mySeat)
                            || roleArrivingSeats.contains(mySeat),
                       roles: shownRoles(view),
                       battles: view.battles.count, sweep: sweepBattles.count)
        #endif
        // ONE size table for both role rows: `FRoleMark.size`. This call site
        // and FSeatBadge's `roleRow` used to carry their own numbers with a
        // comment on each asking the other to keep in step, which is not a
        // mechanism - my own role must not read bigger than an opponent's just
        // because it is mine. Round 16 goes further and shares the whole MARK
        // (FRoleCoin), so my shield flips, fades and flies exactly as theirs do,
        // and my own seat is a landing pad like any other.
        let mark: RoleMarkKind? = isOut ? nil
            : saidGood ? .check
            : isDefender ? .shield
            : isAttacker ? (shownRoles(view).firstAttacker == mySeat ? .leadSword : .sword)
            : nil
        return FRoleCoin(kind: mark,
                         departing: roleDepartingSeats.contains(mySeat),
                         arriving: roleArrivingSeats.contains(mySeat))
            .background(GeometryReader { g in
                Color.clear.preference(key: RoleMarkFramesKey.self,
                                       value: [mySeat: g.frame(in: .named(boardSpace))])
            })
    }

    // MARK: - the roles, and the marks that carry them (round 16)

    /// What the badges are wearing, as a value. Not a GameView: this is only the
    /// three facts a role mark is drawn from, so comparing two of them answers
    /// "did anything about the roles change" without a whole board diff.
    /// What the badges are wearing. Lives in the SDK beside the rules that read
    /// it (`RoleBeat`, over c/src/anim_plan.c) rather than inside a view.
    typealias RoleState = RoleMarks

    /// THE ROLES A COLD OPEN SHOULD DRAW BEFORE IT HAS PLAYED ANYTHING: the ones
    /// the bubble FOUND. A pure function of the controller, so `body` may read it
    /// on the very first paint - the same trick, and the same reason, as
    /// `pendingOpen`, which holds the counts back over the same window.
    ///
    /// Round 21, measured on the rig: the first paint of a cold open drew the
    /// marks from the FINAL view, and the seed inside `runEventStream` did not
    /// land for another ~50ms. Alex, who is about to be shown attacking, wore a
    /// shield for three frames and then flipped out of it - a coin flip into a
    /// role that seat never held, right as the replay began. (Pre-dates this
    /// round: round 16 seeded from the stream's first event and had the same
    /// window.) Nil once the marks are being driven properly, and nil for a board
    /// with nothing to replay - both of those draw the live view, as always.
    private var pendingRoles: RoleState? {
        guard unstartedReplay != nil,
              let prior = controller.openReplayPriorState else { return nil }
        return RoleState(prior)
    }

    /// The roles the board should DRAW right now - the frozen ones during a
    /// sequence, the pre-move ones on a cold open that has not started, the live
    /// ones at rest.
    func shownRoles(_ view: GameView) -> RoleState {
        ledger.roles ?? pendingRoles ?? RoleState(view)
    }

    func shownIsDefender(_ seat: Int, _ view: GameView) -> Bool {
        shownRoles(view).defender == seat
    }

    /// ROUND 28: is the board DRAWING this seat as out (badge edge-on)?
    ///
    /// The frozen answer while a sequence is running, the live one at rest -
    /// exactly `shownRoles`' shape, and the nil case is what makes a seat that
    /// was already out when the board opened collapse with no animation.
    func shownOut(_ seat: Int, _ view: GameView) -> Bool {
        if let out = ledger.out { return out.contains(seat) }
        return view.players.first { $0.seat == seat }?.isOut ?? false
    }
    func shownSaidGood(_ seat: Int, _ view: GameView) -> Bool {
        (shownRoles(view).goodMask & (1 << seat)) != 0
    }

    /// Hand the roles over to `target`, flying whatever actually moved.
    ///
    /// THE TWO THAT TRAVEL. A defender's shield and the first attacker's sword
    /// are the only marks that BELONG to a seat and then belong to another one -
    /// so they are the only two that fly, and every other change (a sword
    /// becoming a check, a check clearing at the end of a bout) is a gesture the
    /// mark makes where it stands. That is the owner's "most swords can fade":
    /// an attacker who simply may not attack any more did not give their sword
    /// to anyone.
    ///
    /// A flight needs BOTH pads to have published; when one has not (a cold
    /// first layout, a seat that just went out) the mark still changes, it just
    /// changes in place. Never a reason to withhold the state.
    /// Returns true when a mark actually took off - the caller inside a
    /// sequence awaits that, so the hand-off is a beat of the sequence rather
    /// than something still in the air after it ends.
    @discardableResult
    func syncRoles(to target: RoleState, in view: GameView, animated: Bool) -> Bool {
        let old = ledger.roles
        // `.handOff` - the one claim that is not a sequence and is still allowed
        // to write. See ShownLedger.swift: this is the only writer that reads
        // what the badges are wearing, works out which marks changed hands and
        // FLIES them, and its one caller outside a stream (the `!sequenced`
        // branch of the board's `onChange`) is the only thing that animates a
        // pass's shield hand-off.
        ledger.write(.handOff) { $0.roles = target }
        guard animated, !reduceMotion, let old, old != target, !view.isOver else {
            // Only when something actually moved and did NOT fly: a cold board
            // with nothing to hand over from, a Reduce Motion snap, a game that
            // just ended. The silent case (nothing changed at all) is most view
            // changes and would drown the trace.
            if old != target {
                AnimLog.say("roles -> d\(target.defender) fa\(target.firstAttacker) g\(target.goodMask) (no hand-off: from=\(old.map { "d\($0.defender) fa\($0.firstAttacker) g\($0.goodMask)" } ?? "nil") animated=\(animated) over=\(view.isOver))")
            }
            return false
        }
        let flights = Self.roleFlights(from: old, to: target, pads: roleMarkFrames)
        AnimLog.say("roles d\(old.defender) fa\(old.firstAttacker) -> d\(target.defender) fa\(target.firstAttacker) pads=\(roleMarkFrames.keys.sorted()) flying=\(flights.count)")
        guard !flights.isEmpty else { return false }
        // THE HAND-OFF BEGINS NOW, in the same update as the roles that sent the
        // mark flying - not a hop later, which let the receiving badge start an
        // ordinary flip to the mark still in the air (two shields).
        if RoleCoinMotion.live.syncFlightSeats { beginRoleFlights(flights) }
        Task { await runRoleFlights(flights) }
        return true
    }

    /// WHICH MARKS TRAVEL between two role states, and from where to where.
    ///
    /// A defender's shield and the first attacker's sword are the only marks
    /// that BELONG to a seat and then belong to another one, so they are the
    /// only two that fly. Everything else a role change does - a sword becoming
    /// a check, a check clearing at the end of a bout, an attacker who may no
    /// longer attack - is a gesture the mark makes where it stands, which is the
    /// owner's "most swords can fade": a mark that flies is a mark that went
    /// somewhere, and nobody took those.
    ///
    /// The SWORD leaves the seat that opened the bout that just ended, even when
    /// that seat is currently wearing a check for having said good. What travels
    /// is the right to open, not the glyph that happened to be on screen.
    ///
    /// A flight needs BOTH pads to have published. When one has not - a cold
    /// first layout, a seat that just went out and stopped drawing a mark - that
    /// mark simply changes in place. Never a reason to withhold the state; the
    /// board's roles are already committed by the time this is asked.
    ///
    /// Static and pure so the rule can be read (and tested) without a board.
    static func roleFlights(from old: RoleState, to target: RoleState,
                            pads: [Int: CGRect]) -> [RoleFlight] {
        func pad(_ seat: Int) -> CGPoint? {
            guard let r = pads[seat], r != .zero else { return nil }
            return CGPoint(x: r.midX, y: r.midY)
        }
        var flights: [RoleFlight] = []
        // The shield goes to whoever is defending now - including a PASS
        // (perevod), which is the same hand-off happening inside a bout.
        if old.defender != target.defender,
           let from = pad(old.defender), let to = pad(target.defender) {
            flights.append(RoleFlight(id: "shield-\(old.defender)-\(target.defender)",
                                      kind: .shield, from: from, to: to,
                                      fromSeat: old.defender, toSeat: target.defender,
                                      // ROUND 21, the owner: "the first attacker
                                      // sword fully spins around, but the shield
                                      // kinda turns a little bit then turns back.
                                      // Make the shield spin all the way around
                                      // too." The lean was 24 degrees, and that
                                      // last clause is the bug in it: a ghost that
                                      // ends its flight at 24 degrees is replaced
                                      // by a real shield drawn upright, so the
                                      // mark visibly snapped back on landing. A
                                      // WHOLE turn is the only lean that ends
                                      // where the badge draws it - the hand-off is
                                      // seamless because 360 and 0 are the same
                                      // angle - and it makes both marks speak the
                                      // one language the rest of this file does.
                                      spin: 360))
        }
        if old.firstAttacker != target.firstAttacker,
           let from = pad(old.firstAttacker), let to = pad(target.firstAttacker) {
            flights.append(RoleFlight(id: "sword-\(old.firstAttacker)-\(target.firstAttacker)",
                                      // Round 20: what flies is the OPENER's
                                      // sword, so the ghost wears the opener's
                                      // tint - the whole point of the tint is
                                      // that you can follow this one across the
                                      // table and see which seat it settles on.
                                      kind: .leadSword, from: from, to: to,
                                      fromSeat: old.firstAttacker, toSeat: target.firstAttacker,
                                      // A full turn: it is being thrown to the
                                      // next player to swing.
                                      spin: 360))
        }
        return flights
    }

    // The three timings a role change can have are the kernel's answer now -
    // `RoleBeat` over c/src/anim_plan.c's anim_goods_opening /
    // anim_goods_cleared / anim_pass_hand_off. What is left here is WHERE in the
    // sequence each one is fired, which is this file's business: the opening one
    // is awaited at the top of `runEventStream`, the other two are launched
    // beside the beat's own flights so the marks and the card move together.

    /// Carry the marks across, then hand the badges back their own copies. The
    /// endpoints are blank for the duration, so there is exactly one of each
    /// mark on screen at every instant of the hand-off.
    /// THE WHOLE HAND-OFF, IN ONE UPDATE: the ghost that carries the mark, and
    /// the seats it leaves and lands on.
    ///
    /// These three cannot be split across two updates in either order. Blanking
    /// the departing seat first leaves the board with NO shield on it for a
    /// paint (owner, on the frame he caught at the release: "THERE SHOULD ALWAYS
    /// BE EXACTLY ONE SHIELD... it seems to blink out for a single frame when we
    /// release the card that was dragged"); marking the seats after the roles
    /// have published lets the receiving badge flip to the mark that is still in
    /// the air (two shields). Idempotent - `runRoleFlights` calls it again for
    /// the flag's other state.
    @MainActor
    private func beginRoleFlights(_ f: [RoleFlight]) {
        roleFlightToken += 1
        roleDepartingSeats = Set(f.map(\.fromSeat))
        roleArrivingSeats = Set(f.map(\.toSeat))
        roleFlights = f
        roleProgress = 0
    }

    @MainActor
    private func runRoleFlights(_ f: [RoleFlight]) async {
        AnimLog.say("role flight [\(f.map { "\($0.kind):\($0.fromSeat)->\($0.toSeat)" }.joined(separator: " "))]")
        if roleFlights.map(\.id) != f.map(\.id) { beginRoleFlights(f) }
        let mine = roleFlightToken
        // One paint at the take-off pad before the tween starts - the same beat
        // BoardAnimator.play gives a card, and for the same reason: an animation
        // that starts in the frame its view is created in has nothing to
        // interpolate from.
        try? await Task.sleep(nanoseconds: 25_000_000)
        guard mine == roleFlightToken else { return }
        withAnimation(.timingCurve(0.25, 0.46, 0.45, 0.94, duration: roleFlightTime)) {
            roleProgress = 1
        }
        try? await Task.sleep(nanoseconds: UInt64(roleFlightTime * 1_000_000_000))
        guard mine == roleFlightToken else { return }
        roleFlights = []
        roleDepartingSeats = []
        roleArrivingSeats = []
        roleProgress = 0
    }
}
