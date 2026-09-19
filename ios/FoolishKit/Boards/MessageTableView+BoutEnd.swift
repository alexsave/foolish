// THE END OF A BOUT - the one view change that becomes an animated sequence,
// and the sweep that carries the table off.
//
// `flyBoutEndToDiscard` is the board's whole view-change router: it decides
// which of the shapes below a change is (an arrival, an undo, a released
// settlement, an ordinary placement, a bout end) and hands off. No GameView
// diff decides what FLIES any more - the kernel's events are the only source,
// live and on open alike - and the one thing still read off old/new is the
// TRIGGER and which of MY cards to hide before they fly.

import SwiftUI
import Foundation

extension MessageTableView {

    /// A bout closed (the table cleared): animate its end - the discard or pickup
    /// sweep, then every refill - from the KERNEL's evwire for that move, the SAME
    /// runEventStream the open-replay uses. No GameView diff decides what flies any
    /// more (the old takerSeat / beaten / myNewCards / lastBoutDraws reconstruction
    /// is gone): the kernel's events are the only source, live and on open alike.
    /// The one thing still read off old/new is the TRIGGER (did battles go
    /// non-empty -> empty) and which of MY cards to hide before they fly - both UI
    /// timing, not animation derivation.
    /// Returns true when it handed the change to an ANIMATED SEQUENCE (an
    /// open-replay or a bout end). Round 16: the caller needs to know, because a
    /// sequence owns the role hand-off too - it plays it as its closing beat -
    /// while everything else has to have the roles released for it on the spot.
    @discardableResult
    func flyBoutEndToDiscard(to newView: GameView?) -> Bool {
        // A bubble that ARRIVED while this board was open (the controller
        // re-adopted rather than being replaced - see MessageTurnController.
        // adopt). There is no meaningful "board before this move" to diff
        // against: the chain jumped, possibly by several actions. So the arrival
        // is played exactly the way a cold open plays one, by dropping `prior`
        // and letting the `prior == nil` branch below run the kernel's own event
        // stream for the last move.
        let arrived = controller.arrivalTick != seenArrivalTick
        if arrived { seenArrivalTick = controller.arrivalTick }
        let prior = arrived ? nil : lastView
        lastView = newView
        // Bug 6: remember the last table that actually had cards on it, so the
        // discard sweep below can map each trashed card back to the slot it sat
        // in. By the time the bout-end (empty-table) view arrives this still
        // holds the pre-clear battles.
        if let nv = newView, !nv.battles.isEmpty { lastBattles = nv.battles }
        AnimLog.say("viewChanged seat=\(controller.mySeat) prior=\(prior == nil ? "nil" : "\(prior!.battles.count)b") new=\(newView.map { "\($0.battles.count)b hand=\($0.me?.handCount ?? -1)" } ?? "nil") undo=\(controller.lastChangeWasUndo)")
        // note 17: consumed and cleared on EVERY call. `playAt` sets it right
        // before the apply whose resulting view change is the next one we see.
        let cover = pendingCover
        pendingCover = nil
        // Round-6 bug 13: same contract as `cover` - consumed on EVERY call, so
        // a placement can never be replayed against a later, unrelated view
        // change. Whichever branch below flies it owns un-hiding its cards; any
        // branch that does not fly it must give them straight back, or a card I
        // just played would stay invisible on the table for good. That is what
        // the flag + defer pair enforces, rather than a `reveal` call bolted
        // onto each of the six early returns below.
        let placement = pendingPlacement
        pendingPlacement = nil
        var placementFlown = false
        // THE HELD SETTLEMENT (MessageTurnController). Same one-shot contract as
        // `cover` and `placement` above - consumed on EVERY view change, so a
        // half-turn can never be replayed against a later, unrelated one.
        //  - `staged` is the half of a staged bout-ender that may be shown now:
        //    the cover landing, the table being taken. Empty for a good, which
        //    has no step of its own.
        //  - `released` is the half Send just let go of: the discard, the deal,
        //    the roles. It is played whatever the diff below would have said,
        //    because for a released pickup there is no diff to read - the table
        //    was already empty on the board being replaced.
        let staged = controller.takeStagedAnimation()
        // CONSIDERED AND REJECTED: withholding this from the branches that drop
        // it. Two of the branches below discard whatever they are handed
        // (`lastChangeWasUndo`, and `prior == nil`), and a released settlement
        // is the only account there will ever be of its discard and its deal -
        // so consuming it into one of those looks like a bout end that never
        // animates. It cannot actually happen: `adopt` and the conflict
        // retraction both call `dropHold`, which clears the release along with
        // the hold, so by the time either branch runs there is nothing left to
        // eat. Making it survive instead would leave a stale settlement on the
        // controller to be replayed against some later, unrelated view - a real
        // bug traded for an impossible one.
        let released = controller.takeReleasedSettlement()
        defer {
            if !placementFlown, let p = placement {
                animator.reveal(Set(p.cards.map(\.identity)))
            }
        }
        // The live veil is consumed here too, and only here: every path below
        // either pre-hides the same cards for real (synchronously, before this
        // returns) or has nothing of mine to hide, so there is no paint between
        // dropping it and the animator picking it up.
        handBeforeMyMove = nil
        guard let new = newView else { return false }
        // Once this returns, `animator.hidden` and the count overrides are the
        // whole truth — so this is exactly where the board stops veiling.
        defer { controller.consumeReplayPending() }
        // …and any path that returns WITHOUT starting a sequence has to hand the
        // counts back, because `play` now freezes them before every move, not
        // just the ones that end a bout. Leaving them frozen after a plain
        // attack would pin every badge at its pre-move value for good.
        func releaseCounts() {
            // NOT WHILE SOMEBODY ELSE OWNS THEM.
            //
            // These overrides are a SEQUENCE's property: whoever froze them
            // advances them step by step as its flights land, and hands them
            // back in its own teardown. A live play that takes one of the
            // branches below starts no sequence of its own and does NOT claim
            // `animSequenceToken` - there is nothing to claim until the kernel
            // publishes a table slot - so a stream that is still running is
            // still the newest, and its teardown is still coming. Releasing
            // here snapped every seat badge, the deck and the discard to their
            // FINAL counts in the middle of that stream's flight, and its next
            // step then set them back: a count twitch with no move behind it,
            // and the one kind of twitch the hand's own machinery cannot
            // explain.
            //
            // Deferring is safe in the direction that matters. The owner of the
            // freeze is by construction the newest sequence, so `runEventStream`
            // 's teardown WILL release them - this only declines to do it early.
            // A board with nothing running (every other caller, and the common
            // case) is unchanged: nothing is sequencing and this releases
            // exactly as it always did.
            ledger.write(.bystander, by: "releaseCounts",
                         note: "badges stay "
                            + (controller.view?.players ?? []).map { "s\($0.seat)=\(shownHandCount($0))" }
                                .joined(separator: " ")) { l in
                l.deck = nil; l.trump = nil; l.discard = nil; l.hand = [:]; l.battles = nil
                l.out = nil
            }
        }
        if reduceMotion {
            releaseCounts(); clearSweep()
            // THE CONFLICT MODEL under reduce-motion: the retraction is a snap
            // (no flights anywhere on this branch), so the latched arrival must
            // be released NOW rather than waiting out the failsafe.
            if controller.conflictRetracting {
                Task { await controller.finishConflictAdopt() }
            }
            if new.isOver { showResults = true }   // note 39b
            return false
        }
        // note 10: undo can legally take battles -> empty; never a bout end.
        // Round-8: an undo is the EXACT REVERSE of the play it undoes - the card
        // flies FROM the table back to its hand slot while the present cards slide
        // apart to make room, never fading on the table and teleporting into the
        // hand. `flyUndoReturn` runs that reverse flight and returns true when it
        // owns the animation; a non-return undo (nothing came back to MY hand -
        // e.g. undoing a pickup, or someone else's move) falls through to the snap.
        // THE CONFLICT MODEL rides this same branch: a staged move being
        // retracted ahead of an arrival IS an undo-all
        // (MessageTurnController.offerArrival publishes the base view with
        // `lastChangeWasUndo` up), flown by the same two reverse shapes. The
        // only differences are the RED on the ghost, the verdict filter (a card
        // the arriving chain itself animates must not fly home first - see
        // ConflictModel.swift), and the hand-off: when the flight lands, the
        // board tells the controller to adopt the latched arrival.
        if controller.lastChangeWasUndo {
            releaseCounts()
            let conflict = retractionFacts()
            if let old = prior, flyUndoReturn(old: old, new: new, conflict: conflict) { return false }
            // ROUND 28: and the other direction - undoing a PICKUP sends the
            // cards back OUT of my hand onto the table. Tried second because the
            // two are mutually exclusive by construction (one undo moves cards
            // one way) and `flyUndoReturn` is the commoner shape.
            if let old = prior, flyUndoRelease(old: old, new: new, conflict: conflict) { return false }
            clearSweep()
            // Nothing to fly (no prior view, or a shape neither reverse knows):
            // the retraction is a snap here too, so release the arrival now.
            if conflict != nil { Task { await controller.finishConflictAdopt() } }
            return false
        }
        // First appear with a delivered game: the open-replay (same event path).
        if prior == nil { AnimLog.say("-> openReplay"); replayLastMoveOnOpen(new); return true }
        // Send released the bout end this board had been withholding. `prior` is
        // the pre-settlement board the player has been looking at since they
        // staged the move, which is exactly the "board before this move" the
        // sweep, the pre-hide and the frozen counts all want.
        if let released, let old = prior {
            AnimLog.say("-> settlement released n=\(released.count)")
            playBoutEnd(events: released, old: old, new: new, cover: nil)
            return true
        }
        guard let old = prior, !old.battles.isEmpty, new.battles.isEmpty else {
            // The table did not just clear, so this is an ordinary placement (or
            // someone else's move arriving). Round-6 bug 13: a card I placed
            // myself now flies from where I let go of it to the slot it landed
            // in - see `flyPlacement`. It used to be left to matchedGeometry,
            // whose only possible source was the card's resting HAND slot, which
            // is exactly what the bug reports seeing. A move that ended the game
            // without clearing the table just settles to results.
            releaseCounts()
            if let pp = placement {
                placementFlown = true
                flyPlacement(pp, to: new)
            }
            if new.isOver { settleResults() }
            return false
        }

        // note 17: a cover that ended the bout in the SAME apply still needs its
        // landing flown first - the kernel jumped straight to a cleared table, so
        // there is no rendered intermediate state for the discard sweep to carry it
        // from. Its battle rect must have been part of the table we just cleared.
        let boutFrames = lastBattleFrames
        var matchedCover: PendingCover?
        // ANY of its landing slots being part of the table we just cleared is
        // enough - a multicover's slots all belong to the same bout, so they
        // stand or fall together.
        if let pc = cover, pc.landing.values.contains(where: { boutFrames.values.contains($0) }) {
            matchedCover = pc
        }
        playBoutEnd(events: staged, old: old, new: new, cover: matchedCover)
        return true
    }

    /// The bout end, as one sequence: pre-hide what is about to land in my hand,
    /// keep the swept table on screen, freeze every count to the board before it,
    /// then play the kernel's steps.
    ///
    /// `events` nil means "ask the kernel for this turn's stream" - the ordinary
    /// case, where the whole turn animates at once. A value is a HALF of a turn
    /// that was split at its settlement (MessageTurnController): the action half
    /// as it is staged, the settlement half when Send releases it. One
    /// implementation for all three, because they differ only in which steps are
    /// being played, never in how.
    private func playBoutEnd(events: [GameEvent]?, old: GameView, new: GameView,
                             cover matchedCover: PendingCover?) {
        // Pre-hide the cards about to land in MY hand so they fly from the deck
        // rather than popping in. This is the ONE view diff that remains, and only
        // to choose which of my cards to hide before the first paint - the events
        // that DECIDE the animation need an async kernel call we cannot make
        // synchronously here. Everything that actually flies is the kernel's.
        let oldHandIds = Set((old.me?.hand ?? []).map(\.identity))
        let myNewIds = Set((new.me?.hand ?? []).map(\.identity)).subtracting(oldHandIds)
        if !myNewIds.isEmpty { animator.preHide(myNewIds) }
        // ROUND 40: the mark this sequence's OWN veil went up at, read
        // SYNCHRONOUSLY here rather than inside the Task below. Everything
        // veiled after this instant belongs to somebody else - most often a
        // card the player taps while this sequence is still finishing - and the
        // teardown may not hand it back (see `BoardAnimator.veilEpoch`). Read
        // here and not in `runEventStream` because the Task can be a paint or
        // two away, and a tap fits in that gap.
        let veiledAt = animator.veilEpoch
        // The table just cleared in the view, so render the cards it HELD (old
        // battles) as the pre-bout table - they sit where they were and fly off,
        // instead of vanishing (a fade) the instant the view empties. Same grid the
        // open-replay uses; the flight hides each card as it lifts (sweptFlownIds).
        setSweep(old.battles)
        // …and freeze every count to the board BEFORE this move. `play` already
        // did this for a move I made (which is the only way to be early enough
        // — see `settled`); repeating it from `old` costs nothing and keeps the
        // sequence correct on any view change that did not come through `play`.
        freezeCounts(to: old)

        AnimLog.say("-> boutEnd preHide=\(myNewIds.count)")
        Task {
            // Fetched BEFORE the cover's landing flight, not after: the swept
            // table has to change the instant that flight ends (see below), and
            // a kernel round-trip in between is a paint the cover spends
            // nowhere. Nothing is visible during the fetch either way - the
            // held ghost from `playAt` is already resting at the card's source.
            // Asked of the CONTROLLER, not of the kernel: it rebuilds its own
            // chain before reading, so an arrival decoding underneath this
            // cannot swap the stream for another game's (round 22 -
            // MessageTurnController.turnEvents).
            var fetched = events
            if fetched == nil { fetched = await controller.turnEvents() }
            let stream = fetched ?? []
            if let pc = matchedCover {
                // Held for the cover's landing flight and the swap that
                // follows it, and NOT for the rest of this Task - the stream
                // below takes its own hold. `BoardAnimator.holdSequence` is
                // where the reason a hold is always given back in a `defer`
                // (and never in a bare pair) is written down.
                let hold = BoardAnimator.holdSequence()
                defer { hold.release() }
                await playStep { _ in self.pendingCoverLandingFlights(pc) }
                // ROUND 16: the cover has LANDED, and the next beat is the sweep
                // that carries the whole table off. Swap the swept table for the
                // kernel's own COVERED one now, in the same MainActor tick the
                // ghost is removed in (no await between, so SwiftUI paints them
                // together and there is no blink), so the card takes the ghost's
                // place on the table instead of vanishing with it.
                //
                // Without this the hold in `runEventStream` would hold on a table
                // with a hole in it: `setSweep(old.battles)` above is the board
                // BEFORE this apply, which is the attack still uncovered. The
                // card also now sweeps from its OWN rendered slot rather than the
                // centre fallback `tableCardSource` documents for exactly this
                // case - it finally has a slot, because it is finally on a table.
                if let covered = PreBoutTable.coveredSweep(stream, current: sweepBattles) {
                    setSweep(covered)
                }
            }
            await runEventStream(stream, finalView: new, veiledAt: veiledAt)
        }
    }
}
