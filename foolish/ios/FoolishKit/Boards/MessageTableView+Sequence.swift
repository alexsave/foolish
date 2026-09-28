// ONE ANIMATED SEQUENCE - the kernel's evwire stream for a single move, played
// as ordered flights.
//
// THE one animation path, shared by the open-replay and the live bout-end, so
// neither derives what-flies-where from a GameView diff. Each displayed count
// is frozen to that step's OWN board (`GameEvent.state`) as its flight lands,
// so a count never jumps ahead of its animation. Sequences are NUMBERED and the
// newest wins: a superseded one stops issuing steps and does not tear down
// shared state that no longer belongs to it.

import SwiftUI
import Foundation

extension MessageTableView {

    /// Animate an ordered evwire stream (the kernel's events for ONE move) as
    /// sequential flights, freezing each displayed count to that step's OWN board
    /// (GameEvent.state) as its flight lands - so a count never jumps ahead of its
    /// animation, and there is no backward count arithmetic to keep in sync. THE
    /// one animation path, shared by the open-replay and the live bout-end, so
    /// neither derives what-flies-where from a GameView diff. The caller pre-hides
    /// the moved cards first (synchronously, before the first paint).
    func runEventStream(_ events: [GameEvent], finalView view: GameView,
                                openReplay: Bool = false, veiledAt: Int) async {
        // ROUND 30: LET THE SHEET FINISH COMING UP FIRST.
        //
        // Tapping a bubble expands the extension, and this board is mounted and
        // running while Messages is still sliding the sheet into view. The
        // replay is the entire reason the bubble was tapped, and it was spending
        // its opening beat behind the edge of the screen. The owner saw it on
        // the shortest gesture there is - "the rotation of the sword to check
        // animation started WHILE the view was coming up into view, so we barely
        // saw the sword" - and suspected it was every replay, not just a good.
        // It is: the card flights lose the same beat, they are just long enough
        // to survive it.
        //
        // This waits for the HOST's own signal and nothing more (see
        // `awaitSheetSettled`): the padding beat that shipped alongside it in
        // 1.0(34) was dead air, because `didTransition` already lands after the
        // sheet has visibly settled.
        //
        // OPEN REPLAYS ONLY. A live sequence is a move made on a board already
        // on screen, and holding that would put a lag on every tap.
        //
        // CLAIMED while it holds, and scoped to this `if` so that it holds for
        // the sheet transition and nothing else. The sequence's own hold is not
        // taken until further down, so an unclaimed wait here would have the
        // board answering "not animating" for the length of the hold - and
        // `BoardAnimator.waitForSettle`, which the extension awaits before
        // staging a bubble, would sail straight through it.
        if openReplay {
            let hold = BoardAnimator.holdSequence()
            defer { hold.release() }
            await Self.awaitSheetSettled()
        }
        let run = AnimLog.on ? AnimLog.nextRun() : 0
        AnimLog.say("stream#\(run) begin n=\(events.count) [\(events.map { "\($0.kind.map(String.init(describing:)) ?? "?")@\($0.seat)x\($0.cards.count)" }.joined(separator: " "))] depth=\(BoardAnimator.sequenceDepth)")
        // THE ONE LINE A FIELD REPORT NEEDS, and the one the trail did not have.
        //
        // `adopt turn N, K to animate` has been recorded since round 16, so a
        // trail says what an OPEN armed - and nothing at all about the stream
        // that runs when a move is SENT. Owner, 1.0(24)-25, twice: sending a
        // move played the animation belonging to the bubble BEFORE it. Their
        // trail ruled out a refusal (no `send-backwards`) and a reload (no
        // `adopt` after `send`), which leaves the board's own view-change path
        // and a boundary pointing one bubble back - and there was no way to see
        // which boundary it used.
        //
        // So: what kind of stream, how many steps, whose move, and the atom
        // boundary it was cut at. `animAtomsBefore` is the number that decides
        // which move gets replayed, so it is the number that has to be in the
        // trail. Cheap enough to leave in for good - one note per sequence, and
        // sequences are rare.
        FlightRecorder.note(openReplay ? "anim-open" : "anim-live",
                            "n=\(events.count) from=\(controller.animAtomsBefore) "
                            + "seats=\(Set(events.map(\.seat)).sorted().map(String.init).joined(separator: "/")) "
                            + "kinds=\(events.compactMap { $0.kind.map { String(describing: $0).prefix(4) } }.joined(separator: ","))")
        // ROUND 16, and the case a live board does not have: a COLD OPEN. Tapping
        // a bubble mounts a fresh board, so there is no "roles before this move"
        // in `@State` for the hand-off to start from (`freezeCounts` only runs
        // when this board was already watching), and a receiver opening a
        // bout-ending bubble must still watch the shield cross the table exactly
        // like the player who was already looking at it. Only ever seeds; a
        // frozen board keeps what it froze.
        //
        // AHEAD OF THE EMPTY-STREAM GUARD, because the stream that needs it most
        // is the empty one: a `good` that does not close the bout emits no step,
        // so the difference between these two role states is the ENTIRE move.
        // Seeded below the guard it never ran for exactly that case.
        //
        // ROUND 21 CORRECTED WHERE IT SEEDS FROM. It used to be the stream's own
        // first event, described here as "the board as that turn began" - which
        // it is not. An event's `state` is the table AS OF that step, so the
        // first one is already one move late, and for a move that shows up in
        // the roles rather than on the table there is then nothing left to
        // animate: a bubble carrying a `good` opened with the check printed on
        // the badge (the owner: "it started out already in GOOD"). The kernel
        // hands over the genuinely prior board now
        // (`controller.openReplayPriorState`); the first event remains the
        // fallback for the opens that have no earlier step to ask for - a
        // genesis deal, the first move on a fresh deal.
        //
        // ROUND 28 seeds the OUT badges from the same board and for the same
        // reason: an open replay has no `freezeCounts` behind it, so without it
        // a bubble whose move puts somebody out opens with the badge already
        // collapsed and nothing left to watch. One write, since it is one board.
        let priorBoard = controller.openReplayPriorState ?? events.first?.state
        ledger.write(.sequence) { $0.seedMarks(from: priorBoard, outs: true) }
        guard !events.isEmpty else {
            // ROUND 16: HAND THE COUNTS BACK. Every caller freezes them to the
            // pre-move board SYNCHRONOUSLY (`play`, then `flyBoutEndToDiscard`)
            // and relies on this function's teardown to release them - but that
            // teardown is installed below, past this guard, so a stream that
            // came back empty left every badge and the deck pinned to the board
            // before the move, for as long as it took the next move to arrive
            // and re-freeze them. An opponent stuck a card too high until they
            // played again is the owner's "briefly bumped, then they play a
            // single card and it goes back down".
            ledger.write(.sequence) { l in
                l.deck = nil; l.trump = nil; l.discard = nil; l.hand = [:]; l.battles = nil
                l.out = nil
            }
            releaseHoldback(raisedBy: veiledAt)   // see the teardown below - this guard returns ahead of it
            animator.clearPreHidden(raisedBy: veiledAt)
            // Nothing is in flight and nothing is going to be, so an orphan
            // handed on by a superseded sequence ends here (clearPreHidden
            // cannot reach one - see `orphanedOpens`).
            let orphans = orphanedOpens.filter { animator.isHidden($0) }
            if !orphans.isEmpty {
                AnimLog.say("stream#\(run) rescue-reveal \(orphans.count) orphaned opens")
                animator.reveal(orphans)
            }
            orphanedOpens = []
            clearSweep()
            // Round 16: and the roles, which were frozen by the same caller. A
            // stream with no steps still animates the hand-off - there is
            // nothing else moving, so it is the only thing to watch.
            syncRoles(to: RoleState(view), in: view, animated: true)
            if view.isOver { settleResults() }
            return
        }
        // Bug 9: claim the animator. Anything already running is now stale.
        let mySeq = claimAnimSequence()
        // Round-7 (invisible-deal fix): every hand-card slot this sequence OPENS
        // for an incoming deal/refill/pickup (openSlots, below). clearPreHidden()
        // on teardown CANNOT rescue these — openSlots pulled them back OUT of
        // preHidden so their slot would lay out — so a step whose flight never
        // got built (a landing frame that never published, a poll that timed
        // out, a supersede) leaves its card stuck in `hidden`: laid out, opacity
        // 0, its slot reserved but nothing ever drawn in it. That is precisely
        // the "no animation for our deal, then the cards move over and we have
        // invisible cards in our hand" report — the deal opened the fan but its
        // flight never landed and nothing took the veil back down. Tracked here
        // and force-revealed in the teardown so an open can NEVER end with a
        // card invisible, whatever went wrong mid-flight (`flyPlacement` already
        // does exactly this for a live placement; these two open/bout-end
        // teardowns were the ones still relying on clearPreHidden alone).
        var openedThisSeq = Set<String>()
        // THE CONFLICT MODEL's record: every motion this sequence actually
        // flies, group by group, so that if an arrival supersedes it mid-flight
        // the arrival can REVERSE what had already been shown (reversalDebt).
        // Only what genuinely flew is recorded - a step whose frames never
        // resolved moved nothing and owes nothing.
        var flownThisSeq: [[FlownMotion]] = []
        let hold = BoardAnimator.holdSequence()
        defer {
            hold.release()
            // ONLY the newest sequence may hand the veil and the counts back. A
            // superseded one doing it here is the double pickup (see
            // `animSequenceToken`): it un-hides cards the sequence that replaced
            // it has pre-hidden but not yet flown, so they appear in the fan and
            // are then flown into it a second time.
            if mySeq == animSequenceToken {
                ledger.write(.sequence) { l in
                    l.deck = nil; l.trump = nil; l.discard = nil; l.hand = [:]; l.battles = nil
                    l.out = nil
                }
                // A holdback that never got flown (a poll that timed out, a
                // stream cut short) must not survive as phantom cards in the
                // hand - this is the same rescue the veil gets a line below,
                // and for the same reason. Only the NEWEST sequence clears it:
                // a superseded one would be wiping the holdback its replacement
                // just armed. Round 43 moved that rule into `releaseHoldback`,
                // which states it as the veil epoch, and gave the same rescue to
                // the three teardowns that can supersede a replay without being
                // one (`flyUndoReturn`, `flyUndoRelease`, the genesis deal).
                releaseHoldback(raisedBy: veiledAt)
                animator.clearPreHidden(raisedBy: veiledAt)
                // The swept table has finished flying, so take down the pre-bout
                // grid (its cards now live in a hand / the discard / a badge). Both
                // paths - a live bout-end and an open-replay - lay it out now.
                dropSweep()
                let owed = Veil.teardown(opened: openedThisSeq,
                                         orphaned: orphanedOpens, isNewest: true)
                let stuck = owed.reveal.filter { animator.isHidden($0) }
                if !stuck.isEmpty {
                    AnimLog.say("stream#\(run) rescue-reveal \(stuck.count) opened-but-unflown [\(stuck.sorted().joined(separator: ","))]")
                    animator.reveal(stuck)
                }
                orphanedOpens = owed.carry
            } else {
                let owed = Veil.teardown(opened: openedThisSeq,
                                         orphaned: orphanedOpens, isNewest: false)
                orphanedOpens = owed.carry
                // THE CONFLICT MODEL: hand what this sequence had already flown
                // to the arrival that superseded it, so it can be reversed (red)
                // before the arrival plays. Deposited only while an arrival is
                // collecting - a supersede by a live move of my own is not a
                // conflict, and a deposit nobody consumes must not sit going
                // stale until some later arrival flies it back (see
                // `reversalDebt`).
                if reversalCollecting, !flownThisSeq.isEmpty {
                    reversalDebt.append(contentsOf: flownThisSeq)
                }
                AnimLog.say("stream#\(run) superseded by seq \(animSequenceToken) - teardown skipped, \(openedThisSeq.count) opens handed on, \(flownThisSeq.count) flown groups \(reversalCollecting ? "deposited for reversal" : "dropped")")
            }
        }
        // The counts are ALREADY frozen to the pre-move board by the caller —
        // synchronously, before this Task ever got to run (see `freezeCounts`).
        // They used to be frozen right here, one `await` in, which is a paint or
        // two too late: the board had already drawn every badge at its FINAL
        // count, and then this yanked them back down to start the sequence. That
        // is the twitch — "the other players' card display twitches briefly
        // while our pickup animation plays, as if it was making room for the
        // dealt card, but changed its mind."
        //
        // Now only the per-step advance happens here, each as its flight lands.
        //
        // Round-7 #2: this used to be a flat 120ms wait "let the rects publish"
        // before the FIRST step. For a bout-end discard that step's rects are
        // already captured (lastBattleCardFrames / discardFrame, from before the
        // table cleared), so the wait only bought a visible gap in which the real
        // table cards faded out BEFORE the overlay ghost appeared to fly them - the
        // "cards fade away, then identical cards appear and fly" the owner saw. With
        // the ghost spawning right away it lands on top of the still-present card
        // and masks the fade, reading as the card itself sliding to the pile.
        // playStep polls (45ms) for any step whose frames genuinely aren't ready
        // yet (a fresh open's first layout), so dropping the coarse pre-wait is
        // safe - readiness is still gated, just per-step instead of up front.
        //
        // Round-7 (first-open bunch): an OPEN-REPLAY builds from a COLD first
        // layout, so give it a beat to settle before the first flight - paired with
        // the SNAP-open below, that beat is enough for each incoming card's real
        // slot frame to publish, so the draw flies each card to its correct place
        // (what a warm reload already does). A live bout-end keeps the near-zero
        // wait so its discard ghost covers the fading table card at once (#2).
        try? await Task.sleep(nanoseconds: openReplay ? 100_000_000 : 16_000_000)

        // ROUND 21: A GOOD IS A MOVE, SO IT PLAYS FIRST.
        //
        // The owner, on replaying a round-ending good: "I don't see the sword to
        // good transition. It started out already in GOOD, then did the discard
        // animation and role switch animation… if we close and open to REPLAY
        // it, then for sure we should show our own good animation (rotation)."
        //
        // A `good` is the one action that emits no step of its own - the kernel
        // has no card to move, so the stream a bubble carries opens straight
        // onto the CONSEQUENCES (the transition, the discard, the refill). The
        // move itself lives entirely in the goodMask, and until now the only
        // thing that ever advanced it was the closing beat at the bottom of this
        // function - which is why it arrived after the discard instead of
        // causing it.
        //
        // ADDED goods only, never cleared ones, and that asymmetry is the whole
        // rule: a good being SET is somebody's move and belongs at the front,
        // while a good being taken away is a consequence of the attack that
        // reopened the bout and belongs with the other consequences at the back.
        // Flip it early and an attacker's check would snap to a sword before the
        // card that cleared it had even left their hand.
        //
        // It costs nothing on the board that STAGED the good: that board flipped
        // the mark when the move was staged (the owner: "we shouldn't show the
        // good animation as staging it should've already shown it"), so by the
        // time the settlement is released there is no difference left to find.
        // Every other board - a receiver watching it arrive, a cold open
        // replaying it - has one.
        let plan = AnimBeats(events)
        // The kernel refuses a stream it cannot hold whole rather than
        // truncating it, and a truncated shape would animate half a move. That
        // degrades to no flights at all - the closing beat below still settles
        // the board - so say so rather than leaving a silent still frame.
        if plan.beats.isEmpty, !events.isEmpty {
            AnimLog.say("stream#\(run) no beats for \(events.count) events - the kernel refused the stream")
        }
        if let opening = RoleBeat.goodsOpening(shown: ledger.roles,
                                               firstGoodMask: plan.firstGoodMask) {
            AnimLog.say("stream#\(run) good first: g\(ledger.roles?.goodMask ?? 0) -> g\(opening.goodMask)")
            // The seats do not change here, only what they are wearing, so
            // nothing flies: this is the coin flip each badge makes where it
            // stands, and `syncRoles` finds no hand-off to build.
            syncRoles(to: opening, in: view, animated: true)
            // Both halves of the coin, plus the beat the owner asked for between
            // a move and its consequences ("show the cover animation, then pause
            // then sweep" - the same shape).
            if !reduceMotion {
                try? await Task.sleep(nanoseconds: UInt64(roleFlipHalf * 2 * 1_000_000_000))
            }
        }

        for beat in plan.beats {
            // Every step below is written against ONE event; a beat of several
            // is a MULTI-CARD COVER, whose cards must fly together (see
            // `AnimBeats`). `ev` leads the beat for everything that reads one
            // event - the make-room, the deck override, the sweep marks - and
            // only the FLIGHTS are built from all of them, which is exactly the
            // difference between "at the same time" and "one after another".
            let group = Array(events[beat.range])
            let ev = group[0]
            // Bug 9: a newer sequence has taken over (a live bout-end played on
            // top of a replay still in flight). Stop stepping the stale one
            // rather than interleaving two sets of flights through one animator
            // and two sets of count overrides through one set of badges.
            guard mySeq == animSequenceToken else {
                AnimLog.say("stream#\(run) abandoned - seq \(animSequenceToken) took over")
                return
            }
            // Bug 10: cut THIS step's incoming hand slot(s) now — not at the
            // sequence's start — animated over the flight so my present cards
            // make room AS the deal arrives, never seconds early while some
            // other seat's deal or a pickup animates first. Must precede the
            // build below: `openReplayFlights` reads the landing slot's frame,
            // which only publishes once the slot is laid out.
            let landing = self.myHandLandingIds(ev)
            if !landing.isEmpty {
                openedThisSeq.formUnion(landing)   // rescue set — see the teardown defer
                // Open the fan for this step's incoming card(s). The make-room is
                // cut NOW (this step, not the sequence's start) so my present cards
                // shift exactly as the deal arrives, never seconds early (bug 10).
                //
                // Round-7 ("it should be at the same TIME"): ANIMATE the make-room
                // (present cards SLIDE over to let the new card in, no snap/"jump")
                // and let the flight run at the SAME time. There is no settle now:
                // the flight targets the card's ANALYTICAL final slot
                // (handLandingSlot), so it lands correctly WHILE the row is still
                // sliding - the make-room and the arrival play together, which is
                // exactly what "the same time" asks for. Same in live and replay.
                withAnimation(.timingCurve(0.25, 0.46, 0.45, 0.94, duration: flightTime)) {
                    self.animator.openSlots(landing)
                }
            }
            // …AND THE ROW GROWS AS THIS STEP'S CARDS COME DOWN ONTO IT, which
            // is the table's twin of the fan's make-room just above and is
            // timed identically: the same curve, the same `flightTime`, started
            // in the same breath as the flight. So a pile already on the table
            // slides over WHILE the new card is in the air and the two settle
            // together, which is what "the cards on the table should start as
            // they were before the move, and rearrange as the move comes in"
            // asks for.
            //
            // WITH A DURATION FROM THE PLAN, and that is the point of moving it
            // here at all. Left to SwiftUI's ambient transaction the reflow ran
            // ~200-270ms whatever the flight was doing - measured identical at
            // 1x and at HARNESS_SLOWMO=6, while flights scale 500ms to 3000ms -
            // so the row's motion was the one beat on this board that nobody
            // had written and nothing could slow down.
            //
            // Only while the ledger is holding the row (an addition being
            // replayed). A sweep leaves it nil and the grid is `sweepBattles`',
            // which is taken down by `dropSweep` when its cards have flown.
            if ledger.battles != nil, let s = group.last?.state ?? ev.state {
                // `_ =` because `ShownLedger.write` answers whether the write
                // LANDED, and a write inside `withAnimation` hands that answer
                // back through it. This site does not act on it - a refused
                // write means a newer sequence owns the row, which is exactly
                // the case this step has nothing more to do about.
                _ = withAnimation(reduceMotion ? nil
                                  : .timingCurve(0.25, 0.46, 0.45, 0.94, duration: flightTime)) {
                    ledger.write(.sequence) { $0.battles = s.battles }
                }
            }
            // Round-7: the DECK count drops as the cards LEAVE the deck (they start
            // flying NOW), not when they land - a full deck with cards visibly
            // flying out of it reads wrong. Deck only, and only for deck-sourced
            // draws; the discard pile and seat badges still tick up when THEIR cards
            // arrive (the per-step advance after the flight, below).
            if let s = ev.state, ev.kind == .deal || ev.kind == .refill {
                ledger.write(.sequence) {
                    $0.deck = s.deckCount
                    // …AND THE TRUMP LEAVES THE WELL WITH THE CARDS THAT TOOK
                    // IT, but it is not GONE yet. The stock is one thing drawn
                    // in two pieces and both release at departure: the step
                    // whose flight empties the deck is the step that took the
                    // card out from under it, so holding it one beat longer
                    // would draw five cards with four in the air.
                    //
                    // `.airborne`, NOT `.gone`, and that is the other half of
                    // the owner's rule: "if there still is a flipped card on
                    // board, don't show Trump indicator yet! Only after flipped
                    // is gone do you show it." `.gone` puts the bare glyph up,
                    // and the glyph means the card does not exist anywhere -
                    // which is not true of a card that is at this instant
                    // crossing the board. The glyph waits for the LANDING, in
                    // the per-group settle below.
                    $0.trump = Self.trumpAtDeparture(s)
                }
            }
            // ROUND 30, and the same rule one seat over. The owner: "when the
            // cards fly out of the hand to cover some cards on the table, the
            // card count in the player's hand doesn't update until the cards
            // LAND ... if two are currently flying, it should display 4 cards in
            // hand, right?" Right - and it is the deck's rule exactly: a count
            // drops as its cards LEAVE, because a badge reading 6 with two of
            // that seat's cards visibly in the air is claiming eight.
            //
            // ONLY the leaving direction. The other half of the owner's note is
            // the half already shipped: "if we have some move that puts cards
            // INTO a player's hand, the card count shouldn't update until the
            // cards GET TO THE HAND" - which is the per-group advance after the
            // flight, below, and is why this is a leaving-only rule rather than
            // a move of that advance.
            //
            // Off `group.last`, not `ev`: a multi-card cover flies as ONE group
            // and the count must land on the board after ALL of them left, not
            // after the first. My own seat is not a badge (my hand is the fan,
            // and the veil has already taken the played cards out of it), which
            // is the same reason `freezeCounts` skips it.
            if beat.dropsBadge,
               ev.seat != controller.mySeat,
               let s = group.last?.state,
               let leaving = s.players.first(where: { $0.seat == ev.seat }) {
                ledger.write(.sequence) { $0.hand[ev.seat] = leaving.handCount }
            }
            // A card leaving the TABLE (pickup / discard) hides its pre-bout grid
            // copy the instant its flight begins, so the overlay ghost is the only
            // copy in motion - no table copy fading beside it, no copy left behind
            // to reappear. Marked now, just before the flight plays.
            switch ev.kind {
            case .pickup, .discard, .cardsToTrash:
                sweptFlownIds.formUnion(ev.cards.compactMap { $0?.identity })
            default: break
            }
            // ROUND 28: AND THE BADGE OF ANYONE THIS GROUP PUTS OUT.
            //
            // Fired with the flights, not awaited: the collapse and the card
            // motion are one event (see `AnimBeats` for why the `out` notice that
            // follows a move cannot be the trigger on its own). Idempotent, so
            // the lookahead here and the fallback when the loop reaches the
            // `out` group itself cannot collapse a badge twice.
            let goingOut = beat.outs
            if !goingOut.isEmpty, !(ledger.out ?? []).isSuperset(of: goingOut) {
                AnimLog.say("stream#\(run) out badges collapse \(goingOut.sorted())")
                // `_ =` for the same reason as the row above: the ledger's
                // "did this land" comes back through `withAnimation`, and a
                // refusal here means a newer sequence owns the badges.
                _ = withAnimation(reduceMotion ? nil
                                  : .timingCurve(0.25, 0.46, 0.45, 0.94, duration: flightTime)) {
                    ledger.write(.sequence) { $0.out = ($0.out ?? []).union(goingOut) }
                }
            }
            // ROUND 28: THE GOODS THIS GROUP CLEARS TURN NOW, WITH ITS CARD.
            //
            // Fired here rather than awaited, and deliberately not `await`ed: the
            // badge flip and the card's flight are one event and start in the
            // same instant (the flip is the shorter of the two and simply
            // finishes first). Gated on the group actually PUTTING A CARD DOWN -
            // a throw-in is the only thing that clears a good this way, and a
            // goodMask that changes for any other reason still belongs to the
            // closing beat with the rest of the consequences.
            if beat.placedAny,
               let cleared = RoleBeat.goodsCleared(shown: ledger.roles,
                                                   stepGoodMask: beat.goodMask) {
                AnimLog.say("stream#\(run) goods clear with the card: g\(ledger.roles?.goodMask ?? 0) -> g\(cleared.goodMask)")
                syncRoles(to: cleared, in: view, animated: true)
            }
            // ROUND 29: AND A TRANSFER HANDS THE SHIELD OVER WITH ITS CARD.
            //
            // Owner, 1.0(28): "b both at once. (shield should always fly, my
            // sword should rotate in, and their next sword should rotate out)."
            // Fired here rather than awaited, exactly like the cleared goods
            // above and for the same reason - the shield's flight and the
            // card's are one event and start in the same instant, the shorter
            // of the two simply finishing first. The two swords need no line of
            // their own; `FRoleCoin` turns them for us off the departing /
            // arriving seats this sync publishes.
            //
            // Until now a transfer's hand-off waited for the CLOSING beat at
            // the bottom of this function, where a BOUT END's hand-off belongs
            // - so a pass read as two movements, the card and then the shield,
            // when the owner's whole point is that it is one move. That closing
            // beat still runs and is still right for everything else: by the
            // time it does, the ledger's roles already hold this state, so it finds
            // nothing left to hand over and flies nothing twice.
            //
            // Channel A never reaches here (a pass does not clear the table, so
            // a staged one is an ordinary placement and the `!sequenced` branch
            // of the view's `onChange` syncs its roles in the same tick as
            // `flyPlacement`). This is the same beat for the two channels that
            // DO replay a stream - a receiver opening the bubble cold, and an
            // arrival landing on an open board.
            if let handOff = RoleBeat.passHandOff(shown: ledger.roles, beat: beat,
                                                  finalDefender: view.defender) {
                AnimLog.say("stream#\(run) pass: the shield flies with the card d\(ledger.roles?.defender ?? -1) -> d\(handOff.defender)")
                syncRoles(to: handOff, in: view, animated: true)
            }
            // What this step actually flies, captured out of the builder for
            // the conflict record below - the builder's last successful answer
            // is exactly the flight list the animator played.
            var groupFlights: [Flight] = []
            await playStep { lastChance in
                // One builder call per event, one flight list for the group: the
                // animator runs a list in PARALLEL, so a two-card cover leaves
                // the hand as one movement. A builder that cannot resolve yet
                // returns nil for the whole group, so the step retries as a
                // unit and the pair can never split across two beats.
                var f: [Flight] = []
                for e in group {
                    guard let part = self.openReplayFlights(e, view: view, lastChance: lastChance)
                    else { return nil }
                    f.append(contentsOf: part)
                }
                groupFlights = f
                // THE HAND LETS GO, in the same breath the ghosts are created.
                // The flights above were built from the slots these cards still
                // hold, so the takeoff is already captured; dropping them now
                // starts the fan's re-close (FHandFan animates its layout over
                // `flightTime`, keyed on the laid-out set) at the instant the
                // cards leave, which is the whole of "we'll want the hand to
                // rearrange as a result of the cards leaving". Inside the
                // builder rather than before the poll: the poll can run for up
                // to a second waiting on a landing frame, and a hand that
                // closed then would finish long before anything moved.
                let left = Set(group.flatMap { $0.cards.compactMap { $0?.identity } })
                if !left.isEmpty, self.handHoldback.contains(where: { left.contains($0.identity) }) {
                    self.handHoldback.removeAll { left.contains($0.identity) }
                    AnimLog.say("stream#\(run)   hand lets go, \(self.handHoldback.count) still held")
                }
                AnimLog.say("stream#\(run) step \(ev.kind.map(String.init(describing:)) ?? "?")@\(ev.seat) n=\(group.count) flights=\(f.count) [\(f.map(\.id).joined(separator: ","))]")
                // ROUND 22: WHERE each card is actually being flown, against the
                // regions it could legitimately land in. "a deal animation go
                // from the draw pile TO THE TABLE. To the card I had just picked
                // up" (owner, 1.0(24)) is a destination rect question, and no
                // log could answer it - the ids above say WHAT flew, never
                // WHERE. Deduped to the flights whose target is outside the hand.
                for fl in f where handFrame != .zero && !handFrame.insetBy(dx: -40, dy: -40).contains(fl.to.origin) {
                    AnimLog.say("stream#\(run)   OFF-HAND flight \(fl.id) "
                        + "from=(\(Int(fl.from.midX)),\(Int(fl.from.midY))) "
                        + "to=(\(Int(fl.to.midX)),\(Int(fl.to.midY))) "
                        + "hand=(\(Int(handFrame.midX)),\(Int(handFrame.midY))) "
                        + "deck=(\(Int(deckFrame.midX)),\(Int(deckFrame.midY)))")
                }
                // ROUND 20: a card arriving onto the SWEEP grid is in the air
                // from this instant, so the attack under it starts rotating now
                // rather than snapping once the cover lands (`sweepArriving`).
                // Set here, in the builder, and not before the poll: the poll
                // may run for up to a second waiting on a frame, and a tilt
                // that started then would finish long before the card did.
                let onSweep = Set(f.compactMap { $0.card?.identity }).intersection(self.sweepUnplaced)
                if !onSweep.isEmpty { self.sweepArriving.formUnion(onSweep) }
                return f
            }
            // THE CONFLICT MODEL's record: this group's motions, now that they
            // have flown. The dest kind is the verdict's side of the flight
            // (the kernel's `anim_conflict_dest`), taken from the group's lead
            // event - a group is one kernel move, so its cards share a
            // destination kind.
            if !groupFlights.isEmpty {
                let dest = ConflictDest(of: ev.kind, seat: ev.seat,
                                        mySeat: controller.mySeat)
                flownThisSeq.append(groupFlights.map { FlownMotion(flight: $0, dest: dest) })
            }
            // ROUND 20: whatever this step just flew ONTO the pre-bout grid has
            // arrived - hand it to the grid to draw, in the same tick the ghost
            // is taken down. It keeps the tilt it flew in with: it is no longer
            // hidden, which is `coverTilted`'s other way of being true.
            if !sweepUnplaced.isEmpty {
                sweepUnplaced.subtract(beat.placed)
                sweepArriving.subtract(beat.placed)
            }
            // ROUND 17: A NEWER SEQUENCE MAY HAVE TAKEN OVER WHILE THAT FLIGHT
            // PLAYED, and the counts below belong to whoever is newest.
            //
            // The loop already checks this at its TOP, which is enough while a
            // board only ever animates its own moves: nothing arrives mid-flight
            // on a board that is driving itself. A bubble arriving on an OPEN
            // board does exactly that (the extension hands it to the live
            // controller rather than rebuilding - MessagesRootView.seatOnBoard),
            // and the supersede then lands in the middle of an iteration, past
            // the guard. This stream would write its own pre-move counts on top
            // of the newer stream's, and they can be two moves old: the owner's
            // "it seems to be a bit behind", seen as the deck badge thrashing
            // 9 -> 12 -> 9 while nothing about the deck changed.
            //
            // Filmed and traced with HARNESS_SCENARIO=arrival, which is the rig
            // this needed and did not have - every other way a chain reaches
            // this board rebuilds it, so the live path had never been driven.
            guard mySeq == animSequenceToken else {
                AnimLog.say("stream#\(run) abandoned mid-step - seq \(animSequenceToken) owns the counts now")
                return
            }
            // The board settles to the LAST event of the group: the intermediate
            // states inside one move are boards nobody was ever shown.
            if let s = group.last?.state ?? ev.state {
                ledger.write(.sequence) { l in
                    l.deck = s.deckCount
                    // THE LANDING, and where `.airborne` turns into `.gone`.
                    // This block runs after `playStep` has awaited the flight,
                    // which is exactly when the bare trump glyph becomes true.
                    l.trump = Self.trumpAtLanding(s)
                    l.discard = s.discardCount
                    for p in s.players where p.seat != controller.mySeat { l.hand[p.seat] = p.handCount }
                }
            }
            // ROUND 16: a cover that ended the bout HOLDS before the sweep takes
            // the table away. See `boutEndHold` for why this one beat is unlike
            // every other gap in a sequence. Placed here rather than at either
            // call site because both sides reach it: the defender's own board
            // arrives with the landing flight already flown (its cover step is a
            // no-op - the card is not in the final view to fly to), and every
            // receiver replays the same stream from the top.
            if beat.holds {
                AnimLog.say("stream#\(run) hold \(Int(boutEndHold * 1000))ms - bout-ending cover")
                try? await Task.sleep(nanoseconds: UInt64(boutEndHold * 1_000_000_000))
            }
        }
        // ROUND 16: THE CLOSING BEAT. The roles were frozen for the whole
        // sequence (`freezeCounts`), so the marks have been sitting on the seats
        // that held them while the table was swept and the hands refilled. NOW
        // they change hands, with nothing else moving - the shield sails to the
        // next defender and the sword is handed to whoever opens next.
        //
        // Awaited rather than fired and forgotten, so it happens INSIDE the
        // sequence: `sequenceDepth` still covers it (the staged-send flow waits
        // on that), and the results screen cannot cut in over a shield in mid
        // air. A stale sequence skips it - the one that replaced it owns the
        // roles now, exactly as it owns the veil and the counts.
        if mySeq == animSequenceToken {
            // Take the swept table down FIRST. `showsSword` asks whether there
            // are cards on the table, and it has to count the pre-bout grid
            // (that is what keeps every attacker's sword up while the sweep
            // plays) - so if the grid were still standing when the roles change,
            // the seats that just said good would each flash a sword for the
            // length of the hand-off before the empty table took it away again.
            // Every card in it has landed by now; the teardown below repeats
            // this harmlessly for the paths that never reach here.
            dropSweep()
            if syncRoles(to: RoleState(view), in: view, animated: true) {
                try? await Task.sleep(nanoseconds: UInt64((roleFlightTime + 0.05) * 1_000_000_000))
            }
        }
        AnimLog.say("stream#\(run) end")
        // Bug 9: a stale sequence must not settle the results screen either —
        // the sequence that replaced it owns when the board gives way.
        if view.isOver, mySeq == animSequenceToken { settleResults() }
    }

    /// note 39: hold the board on-screen a beat after whatever's animating
    /// (a bout-end sequence, or an open-delta replay) has visibly finished,
    /// THEN swap to the results screen. The only place `showResults` is ever
    /// set true. A short guard against re-scheduling once it's already flipped.
    ///
    /// ROUND 28: the beat is `gameOverHold` (1.0s) and no longer a bare 500ms.
    /// See that constant for why the last board of a game earns a longer look
    /// than any other, and note that it now scales with HARNESS_SLOWMO - a
    /// filmed game-over used to lose its hold as the flights around it stretched.
    func settleResults() {
        guard !showResults else { return }
        Task {
            try? await Task.sleep(nanoseconds: UInt64(gameOverHold * 1_000_000_000))
            withAnimation(.easeOut) { showResults = true }
        }
    }

    // THE SHAPE OF A SEQUENCE is the kernel's answer now - `AnimBeats` over
    // c/src/anim_plan.c's anim_build_beats. It groups the stream into beats
    // (only consecutive covers by one seat merge, because the kernel spends one
    // COVER event per card and a two-card cover must still fly as one movement),
    // and each beat comes back knowing what it puts on the table, which seats it
    // takes out with it, whether the sequence rests after it and which way its
    // badge counts. What is left in this file is the playing.

    /// Wait for the host to finish moving the sheet.
    ///
    /// `CollapseTween.isPresenting` is the honest signal - the extension sets it
    /// between `willTransition` and `didTransition`, so this waits exactly as
    /// long as the slide actually takes rather than guessing at it.
    ///
    /// IT USED TO WAIT A BEAT LONGER, on the theory that `didTransition` fires
    /// on the first frame the board is fully up and starting a rotation on that
    /// exact frame would still read as starting during the arrival. Measured on
    /// device, that theory was wrong in a useful way: `didTransition` lands well
    /// AFTER the sheet has visibly settled, so the beat was insurance against
    /// something that was not happening and the owner could see it as dead air -
    /// "about .5s of empty time between the thing fully opening and the first
    /// animation". The flag alone already holds long enough; the extra 200ms was
    /// pure wait.
    ///
    /// Bounded, because a board that never animates is far worse than one that
    /// animates a beat early: the flag is a static set by a view controller this
    /// view cannot see, and a build where nothing ever clears it (the harness,
    /// which fakes presentation, or a future host that skips the callback) must
    /// degrade to starting immediately rather than to silence.
    private static func awaitSheetSettled() async {
        let deadline = 12   // x 50ms = 600ms, comfortably past Messages' own slide
        var waited = 0
        while CollapseTween.isPresenting && waited < deadline {
            try? await Task.sleep(nanoseconds: 50_000_000)
            waited += 1
        }
    }

    /// Poll (up to ~1.2s) for a step's frames to be ready, then play it and await
    /// the animation. `build` returns nil (frames not ready - retry), [] (nothing
    /// to animate), or the flights.
    func playStep(_ build: (_ lastChance: Bool) -> [Flight]?) async {
        for i in 0..<26 {
            // ROUND 30: never AIM at a board that is still moving under the
            // collapse tween - see `CollapseTween.isTweening` for the whole
            // finding and the arithmetic. Deliberately inside the existing poll
            // rather than a sleep of its own: this is the same "not ready yet,
            // ask again" every builder below already answers with nil, and it
            // inherits the same bound - `lastChance` still fires on the final
            // pass, so a tween that never ends degrades to today's behaviour
            // rather than swallowing the animation. A board at rest never
            // enters this branch at all, so nothing that was working pays for
            // it.
            if CollapseTween.isTweening && i < 25 {
                try? await Task.sleep(nanoseconds: 45_000_000)
                continue
            }
            // Round-7 #1: the final poll passes `lastChance` so a builder that
            // still can't resolve an exact landing frame flies to an APPROXIMATE
            // one instead of returning nil forever, which is what leaves the card
            // to be hard-revealed (it "just suddenly appears in hand"). A rough
            // deck->hand flight reads far better than a pop-in.
            if let f = build(i == 25) {
                if !f.isEmpty { await animator.play([f]) }
                return
            }
            try? await Task.sleep(nanoseconds: 45_000_000)
        }
    }

    /// Wait (bounded) for every OTHER animated sequence to finish the flight it
    /// already has in the air and exit. The caller has claimed
    /// `animSequenceToken` first, so the superseded sequence stops at its next
    /// guard; what this waits out is the one step that was mid-flight when the
    /// claim landed - a card is never cut down in the air, it lands and is then
    /// dealt with (reversed, kept, or handed to the arriving replay). `floor`
    /// is how much of `sequenceDepth` is the caller's own. The deadline covers
    /// the longest single beat a sequence can be inside (the bout-end hold plus
    /// a flight) with slack; on timeout we proceed anyway, which is today's
    /// overlap behaviour and strictly no worse.
    func drainOtherSequences(floor: Int = 1) async {
        let deadline = Date().addingTimeInterval(boutEndHold + flightTime * 2 + 1.0)
        while BoardAnimator.sequenceDepth > floor, Date() < deadline {
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
    }
}
