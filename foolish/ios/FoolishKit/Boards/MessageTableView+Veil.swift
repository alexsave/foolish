// THE VEIL, AND THE COUNTS THAT LAG BEHIND IT - MessageTableView's part of
// round-4 notes 3/5.
//
// The board renders the game as it was BEFORE the move it is about to animate,
// and it has to be able to do that on its very FIRST paint - `.onChange` runs
// after `body`, so anything that waits for it is a paint too late, which is the
// whole "starts already landed and then animates" family of bugs. So the window
// is a pure function of the controller (`unstartedReplay`, `pendingOpen`), the
// badges read three sources in one order, and everything a play or a sequence
// froze is handed back in exactly one place per direction.

import SwiftUI
import Foundation

extension MessageTableView {

    // MARK: the veil (round-4 notes 3/5) — see `settled`

    /// The open-replay this board has NOT started yet, if any: the cards it will
    /// move and the counts the board should show until it does. A pure function
    /// of the controller, so `body` may read it on the very first paint — which
    /// is the whole point, that being the paint the onChange path misses.
    /// nil once `settled`, or when there is nothing to replay.
    /// THE OPEN REPLAY THIS BOARD HAS NOT STARTED YET - the window itself, as
    /// one name, because three things were spelling it out separately.
    ///
    /// It opens when a bubble is tapped and the controller says a replay is
    /// outstanding, and it closes when `flyBoutEndToDiscard` consumes that flag
    /// on its way to starting the sequence. Inside it the board must render the
    /// game as it was BEFORE the move it is about to animate - that is the veil
    /// - and because it is a pure function of the controller, `body` may read it
    /// on the very first paint. That is the whole point: the onChange path runs
    /// AFTER body, so anything that waits for it is a paint too late, which is
    /// the "starts already landed and then animates" family of bugs.
    ///
    /// nil once the replay has started, and nil when there is nothing to replay.
    /// Round 43: `pendingOpen` and `pendingRoles` each re-derived this, one of
    /// them phrasing the same test as two guards, so a change to what "not
    /// started yet" means had two places to be made and two chances to diverge.
    var unstartedReplay: [GameEvent]? {
        Veil.unstartedReplay(replayPending: controller.replayPending,
                             events: controller.openReplayEvents)
    }

    /// `placed` is what this replay will put DOWN on the table - the third
    /// thing the window has to answer, and for the same reason as the other two:
    /// `setSweep` derives it (`AnimBeats(events).placed`) from the `onChange`
    /// that starts the sequence, which is a paint after `battlesArea` has
    /// already drawn the pre-bout table this window renders. See
    /// `pendingSweepUnplaced`.
    var pendingOpen: (ids: Set<String>, counts: AnimPlan.Counts, placed: Set<String>)? {
        guard let events = unstartedReplay, let view = controller.view else { return nil }
        return (controller.openReplayTouchedCardIds, AnimPlan(events, finalView: view).pre,
                AnimBeats(events).placed)
    }

    /// WHAT THE PENDING PRE-BOUT TABLE IS STILL WAITING FOR - the cards it holds
    /// a slot for that this replay has not flown in yet.
    ///
    /// `battlesArea` renders that table synchronously while the window is open,
    /// so a bout-ending cover's board does not blink empty. But the veil for it
    /// lived only in `sweepUnplaced`, which `setSweep` writes from the
    /// `onChange` a paint LATER - so on the paint in between the table drew the
    /// cover already lying on its attack, and the pair opened TILTED. The rig
    /// films the whole thing (owner, 1.0(47): "I briefly saw the uncovered cards
    /// animate from rotated back to straight, then rotate back as my cover cards
    /// flew to cover"):
    ///
    ///     grid sweeping=true cells=1 pairs=1 visible=2 hidden=0 tilt=1   <-- opens tilted
    ///     veil preHide [2-12,3-11]
    ///     grid sweeping=true cells=1 pairs=1 visible=1 hidden=1 tilt=0   <-- straightens
    ///     grid sweeping=true cells=1 pairs=1 visible=1 hidden=1 tilt=1   <-- tilts again
    ///
    /// Same rule as `setSweep`'s, asked one paint earlier, so the handoff to the
    /// state it writes changes nothing on screen.
    /// THE RULE IS THE KERNEL'S (`Veil.sweepUnplaced`), and `setSweep` asks it
    /// too - the whole point is that the two paints cannot answer differently.
    static func pendingSweepUnplaced(placed: Set<String>, table: [BattleView]) -> Set<String> {
        table.isEmpty ? [] : Veil.sweepUnplaced(placed: placed,
                                                table: PreBoutTable.cardIds(table))
    }

    /// Every card the board must render as not-yet-there: what is in flight
    /// right now, plus what the veil is still holding back. THE set passed to
    /// the battle grid and the hand fan, so "is this card on the table" has one
    /// answer and the cover tilt can be read straight off it.
    var veiledCardIds: Set<String> {
        Veil.veiled(hidden: animator.hidden, pendingOpen: pendingOpen?.ids,
                    handBeforeMyMove: handBeforeMyMove,
                    myHand: controller.view?.me?.hand)
    }

    /// WHAT THE FAN IS HOLDING BACK ON THIS PAINT - the veil's answer while its
    /// window is open, the armed `handHoldback` after it. Every render site asks
    /// this; nothing reads `handHoldback` to draw with.
    ///
    /// 1.0(43), owner: "we shouldn't start with 5 cards, fade the one I threw
    /// back in and rearrange animation, then throw it out. The visual should
    /// START with the 6 cards, and just fly the one." `replayLastMoveOnOpen`
    /// arms the holdback synchronously, but IT runs from the view's `onChange`,
    /// which is a paint after the board's first - so the fan opened on the
    /// kernel hand alone and the played card faded in and re-centred the row
    /// before anything flew. The rig has it as a `fan-rows` line at seq=0
    /// (`laid=6 hand=5 held=1`): an onChange only fires on a CHANGE, so a line
    /// there at all is the count having been 5 for the paints before it.
    ///
    /// Exactly `pendingOpen`'s cure, for the same reason `settled`'s doc gives:
    /// while the veil is up the answer is a pure function of the controller, so
    /// `body` may have it on the very first paint. Both sides ask the same
    /// kernel rule (`HandLayout.myPlacedCards`) of the same stream, so the
    /// handoff when the window shuts is invisible.
    var fanHoldback: [Card] {
        Self.fanHoldback(unstarted: unstartedReplay, armed: handHoldback,
                         mySeat: controller.mySeat)
    }

    /// The rule above, as a value. `armed` wins its own order and the veil's
    /// answer only adds what it does not already name: the two are the same set
    /// on every path that reaches here, and a union is what keeps a sequence
    /// still flying its own cards from having them pulled out of the fan when a
    /// second bubble raises a fresh veil over it.
    ///
    /// A SPECTATOR NEEDS NO GUARD. `mySeat` is passed straight through because
    /// spectating IS seat -1, and the kernel spends -1 on "no particular player"
    /// as well as on "no seat" - so `myPlacedCards` already answers nothing for
    /// one, and a ternary here would be a second spelling of that rule.
    static func fanHoldback(unstarted: [GameEvent]?, armed: [Card], mySeat: Int) -> [Card] {
        guard let unstarted else { return armed }
        let pending = HandLayout.myPlacedCards(unstarted, mySeat: mySeat)
        guard !armed.isEmpty else { return pending }
        let have = Set(armed.map(\.identity))
        return armed + pending.filter { !have.contains($0.identity) }
    }

    /// Round-6 bug 10: which veiled hand cards reserve NO fan width YET. A deal
    /// heading for my hand is veiled from the moment the whole sequence starts,
    /// but if it also RESERVED its slot from then, my present cards would slide
    /// left "in anticipation" while unrelated earlier steps (other seats' deals
    /// / pickups) play - the exact complaint. The rule is `Veil.handSlotDeferred`
    /// (c/src/anim_plan.c's anim_veil_hand_slot_deferred).
    var handSlotDeferred: Set<String> {
        Veil.handSlotDeferred(veiled: veiledCardIds,
                              flying: Veil.flying(hidden: animator.hidden,
                                                  preHidden: animator.preHidden),
                              holdback: fanHoldback)
    }

    /// A seat badge's displayed hand count: the per-step override once a
    /// sequence is running, else the veil's pre-move value, else the truth.
    func shownHandCount(_ p: PlayerView) -> Int {
        if let n = ledger.hand[p.seat] { return n }
        if let n = pendingOpen?.counts.hand[p.seat] { return n }
        return p.handCount
    }
    func shownDeckCount(_ view: GameView) -> Int {
        let n = ledger.deck ?? pendingOpen?.counts.deck ?? view.deckCount
        #if DEBUG
        // Where a displayed count COMES FROM. "The board is a bit behind" is
        // always one of three sources disagreeing with the kernel, and only the
        // board knows which one it used.
        Self.traceCount(shown: n, override: ledger.deck,
                        veil: pendingOpen?.counts.deck, truth: view.deckCount)
        #endif
        return n
    }
    func shownDiscardCount(_ view: GameView) -> Int {
        ledger.discard ?? pendingOpen?.counts.discard ?? view.discardCount
    }

    /// THE TRUMP SLOT THE WELL IS DRAWING - the stock's other half, asked
    /// exactly the way `shownDeckCount` asks for the first half, off the same
    /// three sources in the same order.
    ///
    /// It used to be the ONE value on the deck well that skipped all three and
    /// read `view` directly, and that was the whole of the 1.1(55) report: "on
    /// a bout ending good, if the flipped card would've been animated in the
    /// resulting animation, it DOES NOT SHOW at first in the pile BEFORE the
    /// deal animations play. The deck shows, but not the flipped card." A
    /// refill that reaches past the stock deals the trump out, so the board the
    /// move commits has no flipped card - and with the deck count frozen at its
    /// pre-move value and the trump read off that board, the well drew a pile
    /// of four standing on nothing. (The badge went with it: it counts
    /// deck + trump, so it read one low for the whole window.)
    func shownTrumpSlot(_ view: GameView) -> TrumpSlot {
        Self.shownTrump(held: ledger.trump, frozen: pendingOpen?.counts.flipped,
                        committed: TrumpSlot.of(view))
    }

    /// The three sources, in order - a value function so a test can ask the
    /// real rule rather than a copy of it. A HELD `.airborne` or `.gone` is an
    /// ANSWER, not a miss: the step whose flight takes the trump out from under
    /// the pile writes exactly those, and falling through to the freeze there
    /// would put the card back.
    ///
    /// `frozen` is a plain `Card?` because a FREEZE cannot be airborne: it is
    /// the board before the sequence started, when nothing was in the air.
    static func shownTrump(held: TrumpSlot?, frozen: Card?, committed: TrumpSlot) -> TrumpSlot {
        if let held { return held }
        if let frozen { return .card(frozen) }
        return committed
    }

    /// THE ASYMMETRY, as two value rules so it can be asserted without a board.
    ///
    /// The two halves of the stock leave on different beats and both are right.
    /// A card in the air is no longer in the PILE, so the count and the well's
    /// copy of the trump release the instant the flight starts. But the bare
    /// glyph says the trump does not exist anywhere, which is only true once
    /// the flight has LANDED - so departure can never write `.gone`.
    static func trumpAtDeparture(_ board: GameView) -> TrumpSlot {
        let landed = TrumpSlot.of(board)
        return landed == .gone ? .airborne : landed
    }
    static func trumpAtLanding(_ board: GameView) -> TrumpSlot { TrumpSlot.of(board) }

    /// Freeze every displayed count to `v` — the board as it looked BEFORE the
    /// move we are about to animate. Synchronous on purpose, and it must run
    /// BEFORE `controller.apply` (`play` does), not from the onChange the view
    /// change triggers: onChange fires after body, so a freeze there is already
    /// one paint late and shows up as a badge that jumps to its final value,
    /// snaps back, and then counts up again — the twitch.
    ///
    /// My own seat is deliberately left alone: my hand is the fan, not a badge,
    /// and the cards it is about to gain are hidden by `animator.preHide`
    /// instead, so the fan holds its final layout while the cards fly into it.
    func freezeCounts(to v: GameView) {
        // A SEED, NOT AN OVERRIDE - exactly what the ROLES are, further down
        // the same block, and for exactly the reason written there: "a move played while
        // a sequence is still animating (an impatient tap, the harness's
        // auto-move) would freeze the roles to a board that has ALREADY
        // rotated". The counts were left open to the same thing. A running
        // stream froze them to the board before ITS move and walks them forward
        // one step per landing flight; a live play of mine then froze them
        // AGAIN, to a board several steps further on, and the stream's next step
        // put them back - so a seat badge counted DOWN to its final value, back
        // UP to where the replay had got to, and down again. Measured on the rig
        // (`take`, HARNESS_AUTOMOVE_NOWAIT, slowmo 8): seat 3 went 6 -> 5 -> 6
        // -> 5 with only one card ever leaving that seat.
        //
        // The play loses nothing by deferring. It starts no sequence of its own
        // (there is nothing to claim until the kernel publishes a table slot),
        // so the stream that owns the ledger is still the newest one, it is
        // still walking the counts forward, and its teardown still hands them
        // back. A board at rest - every other caller, and the common case - is
        // not sequencing and freezes exactly as it always did.
        // ROUND 43 - A TRIGGER PROPOSED AND STRUCK, with the run that struck it.
        //
        // The worry: `playBoutEnd` also calls this, and unlike `play` it DOES
        // start a sequence - so deferring here would have the new stream open
        // from the old stream's mid-state instead of from `old`.
        //
        // MEASURED, not argued (scenario `take`, 4 players, HARNESS_AUTOMOVE +
        // HARNESS_AUTOMOVE_KIND=pickup + HARNESS_SLOWMO=25 - the hold lapses at
        // 15s while a 25x replay is still on its second of three steps, which is
        // how a BOUT-ENDING move gets played into a running stream; NOWAIT
        // cannot pose it, because `apply` refuses a pickup while the hold
        // stands). The same board twice, once with this guard and once with the
        // freeze forced through it:
        //
        //                       s0   s1(me)  s2   s3      board before my move
        //   deferred (shipped)   4      -     6    6      s3=5
        //   forced               4      -     6    5      s3=5
        //
        // One badge, one card, and the deck and discard identical. Seat 3 held
        // its pre-stream 6 until the pickup's own first step, which then wrote
        // 5. It is a LAG, never a lead and never a step backwards - because
        // every step writes ABSOLUTE counts out of that event's kernel state
        // (`s.deckCount`, `p.handCount`, below and in `runEventStream`), so
        // whatever mid-state a new stream opens from is corrected by its first
        // step, not carried.
        //
        // And forcing it costs more than it buys. `playBoutEnd` does not claim
        // `animSequenceToken` until `runEventStream` runs a Task hop later, so a
        // forced freeze sits unprotected in that gap - the OLD stream's next
        // step begins by writing counts (the deck on a deal/refill, the acting
        // badge on `AnimBeats.badgeDropsAsCardsLeave`) BEFORE it reaches the token check
        // that would abandon it. That is round 42's measured twitch, re-admitted
        // to straighten one lagging badge. Struck, in the shape of edcb91f.
        //
        // ROUND 44 moved the guard itself into `ShownLedger.write`; what is left
        // here is the CLAIM, `.bystander`, which is the whole of the reasoning
        // above stated in one word. Every branch of the block below is inside
        // it, exactly as the `guard` used to return past all of them.
        ledger.write(.bystander, by: "freezeCounts",
                     note: "badges stay "
                        + (controller.view?.players ?? []).map { "s\($0.seat)=\(shownHandCount($0))" }
                            .joined(separator: " ")) { l in
            l.deck = v.deckCount
            // …and the trump under it, frozen at the same synchronous moment
            // and for the same reason: `v` is the board BEFORE the move, and a
            // bout end's refill can deal the trump out, so a well reading the
            // committed board would lose it before anything moved.
            l.trump = TrumpSlot.of(v)
            l.discard = v.discardCount
            // ROUND 28: and who is drawn as out, frozen at the same synchronous
            // moment and for the same reason as the counts - `isOut` is already
            // true in the view this move produced, so a badge read straight off
            // it would be edge-on before the cards that emptied the hand had
            // moved.
            l.out = Set(v.players.filter(\.isOut).map(\.seat))
            var counts: [Int: Int] = [:]
            for p in v.players where p.seat != controller.mySeat { counts[p.seat] = p.handCount }
            l.hand = counts
            // ROUND 16: and the roles with them, at the same synchronous moment
            // and for the same reason - by the time an onChange could do it the
            // board has already drawn the marks at their new seats and there is
            // nothing left to fly.
            //
            // A SEED, not an override, and that survives the move into the
            // ledger unchanged. The roles are "what the badges are wearing", and
            // once they exist they are only ever advanced by `syncRoles` (the
            // `.handOff` claim) - which knows what changed and flies it. Writing
            // the current view over them here would erase exactly that: a move
            // played while a sequence is still animating (an impatient tap, the
            // harness's auto-move) would freeze the roles to a board that has
            // ALREADY rotated, and the hand-off the sequence was about to play
            // would find nothing to hand over.
            l.seedMarks(from: v, outs: false)
        }
    }

    /// THE HOLDBACK'S RESCUE, in one place because there are now four teardowns
    /// that need it and copying a guard four times is how the third copy ends up
    /// meaning something slightly different.
    ///
    /// `epoch` is the caller's own `veiledAt`. A holdback armed AFTER that veil
    /// belongs to a sequence that replaced this one and must survive - the
    /// reasoning written out at `runEventStream`'s teardown ("a superseded one
    /// would be wiping the holdback its replacement just armed"), in the units
    /// that order the two events rather than in `animSequenceToken`, which a
    /// fresh open replay does not claim until a Task hop later. The ordering
    /// itself is `Veil.holdbackIsMine` (anim_holdback_is_mine).
    func releaseHoldback(raisedBy epoch: Int) {
        guard Veil.holdbackIsMine(armedAt: handHoldbackAt, teardownAt: epoch) else {
            AnimLog.say("holdback kept (armed at \(handHoldbackAt) > teardown \(epoch))")
            return
        }
        guard !handHoldback.isEmpty else { return }
        AnimLog.say("holdback rescue - \(handHoldback.count) card(s) let go by a teardown "
            + "[\(handHoldback.map(\.identity).joined(separator: ","))]")
        handHoldback = []
    }

    /// Take down the veil a live play put up and hand back everything that play
    /// froze - the counts, the roles, the swept table, the resting ghost.
    ///
    /// Everything except the placement is handed back only when NOTHING IS
    /// ANIMATING (round 43, written out in the body): a play refused mid-stream
    /// never froze those pieces in the first place, and the sequence that did
    /// still owns them.
    ///
    /// One implementation for the two ways a play can come to nothing: a
    /// rejection the kernel reports (`rejectTick`) and a refusal at a door that
    /// never reached the kernel at all (`apply` returning false). They used to
    /// be separate: the first was written out longhand in the `rejectTick`
    /// handler and the second did not exist, which is the leak. Safe to call
    /// twice - it is keyed off the ledger, which it clears.
    func releaseLivePlayVeil() {
        passHeldAt = nil   // a refused pass has no pair coming to fill its slot
        // My own hand veil goes back unconditionally: `play` raised it
        // unconditionally too (it is not behind `freezeCounts`' guard), it names
        // only MY cards, and the one other place that touches it
        // (`flyBoutEndToDiscard`) consumes it rather than owning it - so there
        // is nobody else it could be taken from.
        handBeforeMyMove = nil
        // ROUND 43: THE COUNTS AND THE ROLES ARE STILL NOT OURS TO GIVE BACK.
        //
        // Round 42 taught `freezeCounts` and `releaseCounts` that the badges,
        // the deck, the discard, the out set and the roles belong to whichever
        // SEQUENCE is animating - it froze them to the board before its move and
        // walks them forward one step per landing flight. This function did the
        // very same writes with no guard at all, and its three callers all reach
        // it mid-sequence: the `rejectTick` onChange, the `controller.superseded`
        // door in `play`, and `apply` returning false. The last of those is not
        // theoretical - `MessageTurnController.apply` refuses for the whole of a
        // red retraction (`conflictRetracting`), and that retraction is
        // `flyUndoReturn` holding `sequenceDepth >= 1`. So a tap during a
        // retraction had `play` correctly DECLINE to freeze the counts and then,
        // one line later, release what it had just declined to touch: every
        // badge snapped forward to the retracted base and snapped back when the
        // arrival's replay re-seeded them. That backwards move is what the
        // `backwardsPaints` oracle counts.
        //
        // The roles are the same story with a worse ending: nilled mid-sequence
        // they leave `syncRoles`' closing hand-off with `old == nil`, so no mark
        // flies and the shield / sword teleport.
        //
        // Deferring costs nothing here for the same reason it costs nothing in
        // `releaseCounts`: a refused play starts no sequence and claims no
        // `animSequenceToken`, so the stream still running is still the newest
        // one and its teardown still hands all of this back.
        //
        // ROUND 44: the guard itself is `ShownLedger.write`'s now, and the
        // three copies of it are one. All that is left here is the claim.
        let released = ledger.write(.bystander, by: "releaseLivePlayVeil",
                                    note: "counts/roles/sweep stay with the running sequence") { l in
            l.deck = nil; l.trump = nil; l.discard = nil; l.hand = [:]; l.battles = nil
            // The out badges with them, which this function used to leave
            // frozen: `freezeCounts` seeds them in the same breath as the counts
            // and both `releaseCounts` and the stream teardown nil them in the
            // same breath, so a release that skipped them pinned `shownOut`
            // (which prefers the ledger over the live view whenever it is
            // non-nil) at the pre-move board until some later stream happened to
            // clear it.
            l.out = nil
            // Round 16: and the roles `play` froze on the way in. Nothing
            // happened, so there is nothing to hand over - this is the same
            // board they were frozen from, which is why it is a plain release
            // and not a sync.
            l.roles = nil
        }
        // …and the pre-bout table `play` laid out for a pickup / good that then
        // never happened. Left standing it is a phantom table over a board the
        // game says is empty (the `sweepVisibleNow` oracle's own defect).
        //
        // ON THE SAME TERMS as the ledger, which is what `write` returning a
        // Bool is for: this is not the ledger, but it was frozen by the same
        // play and a sequence that owns it drops it in its own teardown. A
        // refusal can land in the middle of a bout-end sequence sweeping a table
        // of its OWN (the buttons are gone by then, but a drag still reaches
        // `apply`), and tearing that grid down would take the cards off the
        // table mid-flight.
        if released { clearSweep() }
        // THE PLACEMENT HALF IS UNCONDITIONAL, and deliberately outside the
        // guard above. These cards are this play's own property - `playAt` hid
        // them, no sequence knows about them, and nothing else will ever hand
        // them back - so a refusal during somebody else's flight must still
        // reveal them or they are gone from the fan for the life of the board.
        guard let p = pendingPlacement else { return }
        pendingPlacement = nil
        pendingCover = nil
        let ids = Set(p.cards.map(\.identity))
        // Round-8: drop the resting held ghost `playAt` spawned - the move will
        // never fly, so reveal the hand copy AND clear the ghost, or a static
        // ghost would sit at the source.
        animator.cancelHeld(ids)
        animator.reveal(ids)
    }
}
