// UNDO, AND THE RETRACTION THAT IS ALSO ONE.
//
// An undo is the EXACT REVERSE of the play it undoes - the cards fly back the
// way they came while the hand opens for them - and there are two directions:
// `flyUndoReturn` for a play coming home, `flyUndoRelease` for a pickup going
// back out. THE CONFLICT MODEL rides the same two shapes: an arrival over a
// staged move IS an undo-all, just not one the player asked for, which is
// exactly what the red on the ghosts says.

import SwiftUI
import Foundation

extension MessageTableView {

    /// Rig oracles for the conflict model (HarnessScenario `arrival` prints
    /// them): how many red retraction/reversal flights have been played, and
    /// how many staged-move retractions ran. Levels since launch, like the
    /// stale-paint counters below.
    public private(set) static var redRevertFlights = 0
    public private(set) static var conflictRetractions = 0

    /// IS THIS UNDO A RETRACTION? - the one question that separates the two,
    /// asked where the counter that records it lives.
    ///
    /// An arrival over a staged move publishes the base view with
    /// `lastChangeWasUndo` up (MessageTurnController.offerArrival), so a
    /// retraction and a chosen undo are the SAME view change and only the
    /// controller can tell them apart. nil is a chosen undo; a value is the
    /// retraction, and the facts come with it because the verdict filter needs
    /// them (`.unknown` when the controller has none, which reverts everything).
    func retractionFacts() -> ConflictFacts? {
        guard controller.conflictRetracting else { return nil }
        Self.conflictRetractions += 1
        AnimLog.say("-> conflictRetract (arrival over a staged move)")
        return controller.conflictFacts ?? .unknown
    }

    /// Round-8: the EXACT REVERSE of `flyPlacement`. Undoing a play flies each
    /// card that came back to MY hand FROM the table slot it sat in TO its hand
    /// slot, while the present cards slide apart to make room (openSlots, animated
    /// over the flight) - mirroring the play's gap-close. The table copy is SWEPT
    /// (kept rendered, then snapped hidden the instant its own flight lifts it) so
    /// it never fades out on the table and the card never teleports into the hand.
    /// Returns false when nothing came back to my hand (a non-return undo - undoing
    /// a pickup, or someone else's move), so the caller falls back to a plain snap.
    /// `conflict` non-nil makes this the CONFLICT MODEL's retraction (an
    /// arrival overriding my staged move) instead of a chosen undo: the ghosts
    /// fly RED, cards the arriving chain itself vouches for do not fly at all
    /// (the verdict filter - flying those home only for the adopt to put them
    /// straight back is the web's clear-flicker), and when the flight lands the
    /// latched arrival is released (`finishConflictAdopt`). The choreography is
    /// otherwise the chosen undo's, deliberately: a retraction IS an undo, just
    /// not one the player asked for - which is exactly what the red says.
    func flyUndoReturn(old: GameView, new: GameView,
                               conflict: ConflictFacts? = nil) -> Bool {
        let oldHand = Set((old.me?.hand ?? []).map(\.identity))
        let returned = (new.me?.hand ?? []).filter { !oldHand.contains($0.identity) }
        guard !returned.isEmpty, handFrame != .zero else { return false }
        let ids = Set(returned.map(\.identity))
        // The verdict split. A chosen undo reverts everything (there is no
        // arriving chain to defer to). The staged cards went ONTO THE TABLE, so
        // the dest is `.table`; a CLEAR/KEEP card keeps its sweep-grid copy
        // rendered where it stands and its hand copy veiled until the adopt
        // publishes the arriving board - which shows it in the very place the
        // grid was holding it, so the hand-off is invisible.
        let flying = conflict.map { facts in
            returned.filter { facts.verdict($0, dest: .table) == .revert }
        } ?? returned
        let flyIds = Set(flying.map(\.identity))
        let isConflict = conflict != nil
        AnimLog.say("-> undoReturn\(isConflict ? " (conflict, red)" : "") "
            + "[\(ids.sorted().joined(separator: ","))] fly=[\(flyIds.sorted().joined(separator: ","))]")
        // Veil the returning cards in the hand AND defer their fan slots - the hand
        // opens for each only as its flight arrives (the mirror of the play's veil).
        // THE TABLE AS IT WAS, taken NOW. The undo has already published the new
        // board, and its re-laid-out table replaces these frames within a pass -
        // before the Task below has waited its beat. See UndoFlightSource.
        let fromCardFrames = lastBattleCardFrames
        let fromSlotFrames = lastBattleFrames
        animator.preHide(ids)
        let veiledAt = animator.veilEpoch          // round 40 - see playBoutEnd
        // Keep the pre-undo table rendered so each card flies FROM where it sat,
        // rather than the table copy fading out (its FBattleGrid removal) as the
        // view empties. The sweep hides each copy as its own flight lifts it.
        setSweep(old.battles)
        let mySeq = claimAnimSequence()
        #if DEBUG
        if isConflict { Self.redRevertFlights += flyIds.count }
        #endif
        Task {
            let hold = BoardAnimator.holdSequence()
            defer {
                hold.release()
                if mySeq == animSequenceToken {
                    animator.clearPreHidden(raisedBy: veiledAt)
                    // Round 43: this retraction has superseded whatever replay
                    // was holding cards in the fan, and nothing else will hand
                    // them back - see `handHoldbackAt`.
                    releaseHoldback(raisedBy: veiledAt)
                    dropSweep()
                    let stuck = ids.filter { animator.isHidden($0) }
                    if !stuck.isEmpty { animator.reveal(stuck) }
                }
            }
            // A retraction may land on a staged animation still in the air (I
            // played the card 300ms ago). The token bump above already stops
            // that sequence at its next guard; this waits its current flight
            // out, so the card visibly LANDS and then flies back - two moves in
            // opposite directions, in order, never two sequences interleaved
            // through one animator. Its deposit into the reversal ledger (if
            // any) is discarded unconsumed, and correctly so: THIS flight is
            // that motion's reversal.
            if isConflict { await drainOtherSequences() }
            // A beat for the swept table and the (deferred) hand to publish frames.
            try? await Task.sleep(nanoseconds: 16_000_000)
            if !flying.isEmpty {
                // Make room for the returning cards (present cards slide apart),
                // animated over the flight - the reverse of the play's gap-close.
                withAnimation(.timingCurve(0.25, 0.46, 0.45, 0.94, duration: flightTime)) {
                    self.animator.openSlots(flyIds)
                }
                // Lift the table copies: snap them hidden (no fade) as the flight
                // starts. With the kernel's hold the held table itself is let go
                // in the builder below, in the turn the flight is handed over;
                // hiding them HERE hid them for as long as `playStep` polled.
                if !UndoFlightSource.holdsLeaving { self.sweptFlownIds.formUnion(flyIds) }
                await playStep { lastChance in
                    let laid = self.laidOutHandNow(new)
                    var flights: [Flight] = []
                    for c in flying {
                        let ownSlot = UndoFlightSource.ownSlot
                        guard let from = UndoFlightSource.rect(for: c, in: old.battles,
                                cardFrames: ownSlot ? fromCardFrames : self.lastBattleCardFrames,
                                slotFrames: ownSlot ? fromSlotFrames : self.lastBattleFrames,
                                ownSlotOnly: ownSlot) else { continue }
                        // NOT `handLanding`, and the difference is deliberate.
                        // These cards are coming BACK into the hand, so the fan
                        // has not laid them out yet and `handCardFrames` cannot
                        // hold a frame for them - the middle rung of the shared
                        // chain would answer nil here every time. And the rough
                        // spread is gated on `lastChance` rather than taken
                        // straight away: an undo has a real slot waiting for it
                        // the moment the fan opens, so this polls for the exact
                        // answer and only approximates once out of retries.
                        guard let to = self.handLandingSlot(c, laidOut: laid)
                                ?? (lastChance ? self.handApproxLanding() : nil) else { return nil }
                        flights.append(Flight(id: "undo-\(c.identity)", card: c, from: from, to: to,
                                              fromAngle: UndoFlightSource.keepsTilt ? UndoFlightSource.tilt(for: c, in: old.battles) : 0,
                                              revert: isConflict))
                    }
                    // The same turn `playStep` gives these to the animator: the
                    // table lets go of the cards as their ghosts appear.
                    if UndoFlightSource.holdsLeaving, !flights.isEmpty { self.dropSweep() }
                    return flights.isEmpty ? (lastChance ? [] : nil) : flights
                }
            }
            // The reversal has landed: the latched arrival may take the board.
            // Ahead of the defer above on purpose, so the arriving view is
            // published before the sweep drops and the veil clears - no paint
            // between them shows the base board with a held card nowhere at all.
            if isConflict { await controller.finishConflictAdopt() }
        }
        return true
    }

    /// ROUND 28: THE OTHER REVERSE - undoing a PICKUP, where cards leave my hand
    /// and go back onto the table they came from.
    ///
    /// `flyUndoReturn` above only knows the direction that ENDS in my hand, and
    /// every other undo fell through to a snap. That made a pickup the one
    /// retraction with no motion at all: the biggest board change in the game -
    /// a whole table's worth of cards - happening between two frames, with the
    /// player left to work out from the card count that anything moved. Owner,
    /// on the 1.0(28) walk: "yes it should fly back out of my hand."
    ///
    /// The mirror of `flyUndoReturn` in every part:
    ///  - the SOURCE is analytical, not measured. The cards are already out of
    ///    `new`'s hand by the time this runs, so their fan slots are gone from
    ///    `handCardFrames`; we re-lay the OLD hand out (`FHandFan.slotRects` via
    ///    `handLandingSlot`) and read the slot each card occupied. Deliberately
    ///    computed BEFORE the veil goes up, because `handSlotDeferred` excludes
    ///    pre-hidden cards from the fan and a veiled card would lay out at
    ///    somebody else's slot.
    ///  - the DESTINATION is measured, and polled. The table only just came back,
    ///    so its battle rects publish a paint or two after this apply - the same
    ///    wait `placementFlights` already handles by returning nil to retry.
    ///  - the TABLE copies are veiled synchronously (`preHide`, which the live
    ///    grid honours through `veiledCardIds`) so the cards do not appear on the
    ///    table a frame before their flight puts them there.
    ///
    /// Returns false when nothing left my hand for the table, so an undo of some
    /// other shape still falls through to the snap.
    /// `conflict` as in `flyUndoReturn`: an arrival retracting my staged PICKUP
    /// flies the cards back out of my hand in RED, verdict-filtered - a card
    /// the arriving chain's own replay moves (the arrival is my own sent pickup
    /// coming back) stays veiled instead of flying, because the forward replay
    /// owns its motion - and releases the latched arrival when it lands. The
    /// picked-up cards were staged INTO MY HAND, so the verdict's dest is
    /// `.myHand`.
    func flyUndoRelease(old: GameView, new: GameView,
                                conflict: ConflictFacts? = nil) -> Bool {
        let newHand = Set((new.me?.hand ?? []).map(\.identity))
        let left = (old.me?.hand ?? []).filter { !newHand.contains($0.identity) }
        let allTargets = Self.undoReleaseTargets(left, in: new.battles)
        guard !allTargets.isEmpty, handFrame != .zero else { return false }
        let isConflict = conflict != nil
        let targets = conflict.map { facts in
            allTargets.filter { facts.verdict($0.0, dest: .myHand) == .revert }
        } ?? allTargets
        // The old hand's own layout, read BEFORE the veil (see above). Nothing is
        // deferred in it: the board this undo is leaving was settled.
        let wasLaidOut = HandLayout.laidOut(hand: old.me?.hand ?? [], deferred: [],
                                            order: MessageGameStore.shared.handOrder(gameId: controller.gameIdString))
        var sources: [String: CGRect] = [:]
        for (card, _) in targets {
            if let r = handLandingSlot(card, laidOut: wasLaidOut) { sources[card.identity] = r }
        }
        guard isConflict || !sources.isEmpty else { return false }
        // ALL the leaving cards' table copies are veiled - including the
        // held-back CLEAR/KEEP ones, whose motion belongs to the arriving
        // replay; revealing those on the base board for the retraction's length
        // would be the very snap this model exists to kill.
        let ids = Set(allTargets.map { $0.0.identity })
        let flyIds = Set(targets.map { $0.0.identity })
        AnimLog.say("-> undoRelease\(isConflict ? " (conflict, red)" : "") "
            + "[\(ids.sorted().joined(separator: ","))] fly=[\(flyIds.sorted().joined(separator: ","))]")
        // Hide the table copies NOW, in the same synchronous breath as the view
        // change that put them there - the ghost is the only copy in motion.
        animator.preHide(ids)
        let veiledAt = animator.veilEpoch          // round 40 - see playBoutEnd
        // THE LEAVING CARDS STAY IN THE HAND until their flights exist - the
        // hand-side twin of the table hold (UndoReleaseHandHoldTests). The undo
        // has already taken them out of the kernel hand, and the fan draws a
        // held-back card even though it is veiled for the table.
        if UndoFlightSource.holdsLeaving { handHoldback = targets.map(\.0); handHoldbackAt = veiledAt }
        let mySeq = claimAnimSequence()
        #if DEBUG
        if isConflict { Self.redRevertFlights += flyIds.count }
        #endif
        Task {
            let hold = BoardAnimator.holdSequence()
            defer {
                hold.release()
                if mySeq == animSequenceToken {
                    animator.clearPreHidden(raisedBy: veiledAt)
                    releaseHoldback(raisedBy: veiledAt)   // round 43 - as in `flyUndoReturn`
                    let stuck = ids.filter { animator.isHidden($0) }
                    if !stuck.isEmpty { animator.reveal(stuck) }
                }
            }
            // Same drain as `flyUndoReturn`: a staged pickup's sweep may still
            // be carrying cards into my hand when the arrival lands.
            if isConflict { await drainOtherSequences() }
            // A beat for the restored table to lay out and publish its rects.
            try? await Task.sleep(nanoseconds: 16_000_000)
            if !targets.isEmpty {
                await playStep { lastChance in
                    var flights: [Flight] = []
                    for (card, idx) in targets {
                        guard let from = sources[card.identity] else { continue }
                        guard let to = self.battleFrames[idx] else {
                            if lastChance { continue }
                            return nil
                        }
                        // A card going back onto a battle it DEFENDED lands tilted,
                        // exactly as it was lying before the pickup lifted it.
                        let covering = new.battles[idx].defense == card
                        flights.append(Flight(id: "undorelease-\(card.identity)", card: card,
                                              from: from, to: to,
                                              angle: covering ? FBattleGrid.coverAngle : 0,
                                              revert: isConflict))
                    }
                    // Let the fan go of them in the turn the animator gets their flights.
                    if UndoFlightSource.holdsLeaving, !flights.isEmpty {
                        let flying = Set(flights.compactMap { $0.card?.identity })
                        self.handHoldback.removeAll { flying.contains($0.identity) }
                    }
                    return flights.isEmpty ? (lastChance ? [] : nil) : flights
                }
            }
            // See flyUndoReturn: ahead of the defer, so the arriving view is up
            // before the veil clears.
            if isConflict { await controller.finishConflictAdopt() }
        }
        return true
    }

    /// THE CONFLICT MODEL's collection half for an ARRIVAL: stop whatever
    /// sequence is animating, wait its in-air step out, and take whatever it
    /// had already flown as the debt to reverse. `reversalCollecting` is up for
    /// exactly this window so the superseded teardown knows its motions have a
    /// consumer (see `reversalDebt`).
    func drainSupersededForReversal() async -> [[FlownMotion]] {
        reversalCollecting = true
        // A claim with no `mySeq`, and the only one: this supersedes whatever
        // is running and then starts nothing of its own, so there is no later
        // guard to check a receipt against (see `claimAnimSequence`).
        claimAnimSequence()
        await drainOtherSequences(floor: 0)
        reversalCollecting = false
        let debt = reversalDebt
        reversalDebt = []
        return debt
    }

    /// Play the red reversal for a superseded sequence's collected motions:
    /// verdict-filter and flip them (`reversalSteps` - the pure half), then fly
    /// the result, last motion first, each group as one parallel step. The
    /// ghosts fly over whatever the board shows; a REVERT card is by definition
    /// one the arriving board does not hold at its destination, so there is no
    /// model copy to veil - the ghost lifts from where the card landed and
    /// vanishes into where it came from, red the whole way.
    func playConflictReversal(_ debt: [[FlownMotion]], facts: ConflictFacts) async {
        let steps = Self.reversalSteps(debt: debt, facts: facts)
        guard !steps.isEmpty else { return }
        Self.redRevertFlights += steps.reduce(0) { $0 + $1.count }
        AnimLog.say("conflict reversal: \(steps.count) step(s) "
            + "red=[\(steps.flatMap { $0 }.map(\.id).sorted().joined(separator: ","))]")
        FlightRecorder.note("conflict-reverse", "\(steps.count) steps, "
            + "\(steps.reduce(0) { $0 + $1.count }) flights")
        let hold = BoardAnimator.holdSequence()
        defer { hold.release() }
        await animator.play(steps)
    }

    /// Which battle each card leaving my hand is going back to, as indices into
    /// `battles`. Pure so the pairing can be asserted without a board: a card
    /// that is not on the restored table at all is dropped rather than flown
    /// somewhere arbitrary, which is what makes this safe to call for EVERY undo
    /// and let the empty result mean "not this shape".
    static func undoReleaseTargets(_ leaving: [Card], in battles: [BattleView]) -> [(Card, Int)] {
        var out: [(Card, Int)] = []
        for c in leaving {
            if let idx = battles.firstIndex(where: { $0.attack == c || $0.defense == c }) {
                out.append((c, idx))
            }
        }
        return out
    }

    /// Un-stage the staged move. Shared by the Undo pill (below) and, for the
    /// offline board, FActionBar's own Undo.
    ///
    /// Undo that still has staged moves re-stages the shorter chain (replaces
    /// the input bubble). Undo that empties `pending` must CANCEL the staged
    /// move: Apple offers no API to remove an inserted bubble, so on a
    /// continuation we overwrite it with the base (received) state - the undone
    /// move can then no longer be sent. A genesis with no move left is not
    /// sealable, so there we can only retract our own bookkeeping (`onUnstage`).
    func undoAction() {
        // HOLD THE TABLE BEFORE THE UNDO PUBLISHES - `play`'s rule for a move that
        // empties the table, for the same reason: the onChange that sets the sweep
        // fires a paint too late. Undoing a first attack painted an empty table in
        // between, its collapse layer sized to nothing, and the card came back at
        // the bottom of the drawer for two frames (UndoHoldsTableTests). An undo
        // that moves no table card drops the sweep again in the onChange
        // (`clearSweep`), and a sweep identical to the live table is not held.
        if UndoFlightSource.holdsLeaving, let table = controller.view?.battles, !table.isEmpty { setSweep(table) }
        Task {
            await controller.undo()
            if controller.canStage { await stageNow() }
            else if controller.isContinuation { await stageBaseNow() }
            else { onUnstage() }
        }
    }

    /// The human pressed the X on the staged bubble in Messages' input field.
    ///
    /// Owner: "X-ing the staged bubble should be the SAME as hitting the undo
    /// button. If the player has ALREADY undone via the button, then X-ing the
    /// bubble is a NO-OP." So this runs `undoAction`'s undo and not a second
    /// walk back of its own - `controller.cancelStage()` IS `undo()`, gated by
    /// the kernel (msg_turn_cancel), and what differs is only what the INPUT
    /// FIELD is owed afterwards:
    ///
    /// - `.restage` - a throw-in came off an attack that is still staged, and
    ///   that shorter chain needs a bubble. Same as the pill's.
    /// - `.clear` - nothing is staged now. The pill has to call `stageBaseNow`
    ///   here (Apple offers no way to REMOVE an inserted bubble, so undo-to-
    ///   empty can only overwrite it with the base state); a cancel must not,
    ///   because the human already removed it and re-inserting one puts back
    ///   the very bubble - and the send-hint arrow over it - they just deleted.
    /// - `.noop` - they had already undone, and the bubble they deleted carried
    ///   the base state. The game is not touched.
    ///
    /// The hint arrow needs nothing else from here: it is drawn off
    /// `controller.canSend`, which the undo turns off, and off the surface's
    /// `alsoStaged`, which the surface clears on the same token.
    func cancelStagedBubble() {
        Task {
            switch await controller.cancelStage() {
            case .noop:    break
            case .restage: await stageNow()
            case .clear:   onUnstage()
            }
        }
    }
}
