// THE OPEN REPLAY - what a tapped bubble plays before it lets you look at it.
//
// An open replay renders the FINAL board, so everything the move touched is
// already where it ended up. The whole of this file is putting that back:
// pre-hide what moved, hold my own played cards in the fan, lay out the
// pre-bout table, seed the counts and the marks from the board the bubble
// FOUND - all synchronously, then fly the kernel's stream forward over it.

import SwiftUI
import Foundation

extension MessageTableView {

    /// One open-replay event's flights, straight from the KERNEL's evwire stream
    /// (a GameEvent). The event ALREADY carries viewer-correct cards - my own
    /// draws/pickups as real identities, opponents' as nil (a back) - so unlike
    /// the old diff path there is no reconstructed "my cards" argument, and no
    /// case where my own cards silently go missing (the round-2 #9 bug). Returns
    /// nil to ask `playStep` to retry (a needed frame isn't published yet), or a
    /// (possibly empty) flight list. `view` is the FINAL board, for locating a
    /// card still on the table.
    func openReplayFlights(_ ev: GameEvent, view: GameView, lastChance: Bool = false) -> [Flight]? {
        let mine = !isSpectating && ev.seat == controller.mySeat
        switch ev.kind {
        case .attackPass, .defenderMove, .cover:
            // A card placed on the table (hand -> its battle). Best-effort, no
            // retry: a card already swept onward to a discard/pickup later in
            // this same open is simply skipped (that event flies it).
            //
            // ROUND 20: except the one card that is not "swept onward" but
            // swept BY THIS SAME STREAM - the cover that ended the bout. Its
            // battle is missing from `view` for the same reason the whole table
            // is (there is no table any more), so the lookup above found
            // nothing and the cover simply never animated: the owner's "I wasn't
            // seeing the cover animation on a replay". The pre-bout grid IS on
            // screen at that moment and knows exactly where the card goes, so
            // ask it (`sweepLandingRect`) before giving up.
            // MY OWN placement leaves FROM THE SLOT EACH CARD HELD, not from the
            // hand container's origin. `handSlotsNow` reads the fan that is on
            // screen, and for the length of this step that fan still holds these
            // cards (`handHoldback`), so the slots it returns are the PRE-move
            // ones. Everyone else's cards leave their seat badge, unchanged.
            let mySlots = mine ? handSlotsNow(view) : [:]
            let source = mine ? handFrame : (seatFrames[ev.seat] ?? .zero)
            var out: [Flight] = []
            for case let card? in ev.cards {
                var landed: (rect: CGRect, angle: Double)?
                if let idx = view.battles.firstIndex(where: { $0.attack == card || $0.defense == card }) {
                    // …AND ITS SLOT MAY NOT HAVE BEEN LAID OUT YET, which is now
                    // the NORMAL case rather than a rare one and is polled for
                    // rather than skipped.
                    //
                    // The grid opens on the row BEFORE this move (ShownLedger's
                    // `battles`), so the cell this card is flying into does not
                    // exist until the step advances the row - one paint before
                    // this builder first runs. Skipped, as this used to be, the
                    // whole step came back `flights=0` and the card teleported
                    // into place. The sweep branch below has polled for exactly
                    // this since round 20, for exactly this reason; the two are
                    // one situation and now answer the same way.
                    //
                    // `continue` is still the answer for a card that is not on
                    // the final table AT ALL - swept onward by a later event of
                    // this same open, which flies it itself.
                    guard let rect = battleFrames[idx] else {
                        if lastChance { continue }
                        return nil
                    }
                    // Bug 1: a card that lands as the DEFENSE (cover) lies across
                    // at +coverAngle - see the angle note below.
                    landed = (rect, view.battles[idx].defense == card ? FBattleGrid.coverAngle : 0)
                } else if sweepUnplaced.contains(card.identity) {
                    // Onto the pre-bout grid. `sweepUnplaced` is the whole gate:
                    // it is seeded ONLY by the open-replay path, so the player
                    // who MADE this cover - whose board already flew it, from
                    // the hand rects it measured before the apply
                    // (`pendingCoverLandingFlights`) - falls through to the
                    // `continue` below and does not fly it a second time.
                    guard let onSweep = sweepLandingRect(card) else {
                        // The grid may simply not have measured yet on a cold
                        // open. Worth polling for: this card has nowhere else
                        // to come from, and a `lastChance` build still beats a
                        // cover that never animates.
                        if lastChance { continue }
                        return nil
                    }
                    landed = onSweep
                } else {
                    continue
                }
                guard let dst = landed else { continue }
                let rect = dst.rect
                let landedAngle = dst.angle
                // Bug 1: a card that lands as the DEFENSE (cover) lies across at
                // +coverAngle, so its ghost rotates into that tilt as it flies. An
                // attack lands upright (0); its own later tilt, once ITS cover
                // lands, is the battle grid's job, not this flight's.
                // ONE EVENT CAN PUT SEVERAL CARDS DOWN - the owner's "double 7
                // attack" - and they all leave the same seat, so without a
                // stagger they spawn as a single stacked ghost and only
                // separate once they are already in the air. The 3pt step is
                // the same one the deal and pickup branches below already use
                // for a seat's backs, for exactly this reason.
                // A card with its own slot needs no stagger - it already has a
                // place of its own to leave from, and nudging it would take it
                // off the card the player is watching. The 3pt step stays for
                // the seat-badge case, where every card of the move genuinely
                // does share one point.
                let slot = mySlots[card.identity]
                let base = slot ?? (source != .zero ? source : rect.offsetBy(dx: 0, dy: -220))
                let from = slot != nil ? base : base.offsetBy(dx: CGFloat(out.count) * 3, dy: 0)
                out.append(Flight(id: "open-\(card.identity)-\(ev.type)", card: card,
                                  from: from, to: rect, angle: landedAngle))
            }
            return out

        case .refill, .deal:
            // Deck -> hand (mine, real cards) or a seat's badge (backs, by count).
            if mine {
                let cards = ev.cards.compactMap { $0 }
                if cards.isEmpty { return [] }
                guard deckFrame != .zero, handFrame != .zero else { return nil }
                // Fly to each card's ANALYTICAL final slot so the make-room can
                // animate at the same time (handLandingSlot); no wait for a live
                // frame that is still mid-slide.
                let laid = laidOutHandNow(view)
                return cards.enumerated().compactMap { i, c in
                    handLanding(c, laidOut: laid, index: i, of: cards.count).map {
                        Flight(id: "opendraw-\(c.identity)", card: c, from: deckFrame, to: $0) } }
            }
            guard let badge = seatFrames[ev.seat], badge != .zero, deckFrame != .zero else { return nil }
            let n = max(ev.cards.count, 1)
            return (0..<n).map { k in
                Flight(id: "opendraw-\(ev.seat)-\(n)-\(k)", card: nil, from: deckFrame,
                      to: badge.offsetBy(dx: CGFloat(k) * 3, dy: 0)) }

        case .pickup:
            // Table -> hand (mine) or a seat's badge (theirs) - FACE UP either way.
            // These are the cards that were lying face up on the table a moment
            // ago; turning them into backs mid-flight because someone else is
            // taking them is a lie the viewer can disprove by looking at the
            // board they were just shown (the web plays them face up for the
            // same reason). The kernel agrees: evwire masks DEAL/REFILL, never
            // PICKUP, so `ev.cards` carries real identities to every viewer -
            // this branch was throwing them away.
            let cards = ev.cards.compactMap { $0 }
            if mine {
                if cards.isEmpty { return [] }
                guard handFrame != .zero else { return nil }
                // Round-7 (replay bunch): wait for the invisible pre-bout grid to
                // publish each card's real slot before flying, so a reopened
                // pickup starts from the laid-out table, not the centre fallback.
                if !tableSourceReady(cards) && !lastChance { return nil }
                // Fly each card from its OWN table rect (so the ghost covers the
                // real card) to its ANALYTICAL final hand slot (so the make-room
                // animates at the SAME time, no mid-slide target).
                let laid = laidOutHandNow(view)
                return cards.enumerated().compactMap { i, c in
                    guard let from = tableCardSource(c, fallbackIndex: i) else { return nil }
                    return handLanding(c, laidOut: laid, index: i, of: cards.count).map {
                        // `fromAngle`: a cover was lying across its attack a
                        // moment ago, so its ghost lifts off still tilted and
                        // flattens on the way to the hand.
                        Flight(id: "openpick-\(c.identity)", card: c, from: from.rect, to: $0,
                               angle: 0, fromAngle: from.tilt) } }
            }
            guard let badge = seatFrames[ev.seat], badge != .zero else { return nil }
            if cards.isEmpty {
                // Only if the kernel really did withhold them (it doesn't today).
                guard let center = approximateTableCenter() else { return nil }
                let n = max(ev.cards.count, 1)
                return (0..<n).map { k in
                    Flight(id: "openpick-\(ev.seat)-\(k)", card: nil, from: center,
                          to: badge.offsetBy(dx: CGFloat(k) * 3, dy: 0)) }
            }
            // Opponent pickup: each face-up card sweeps from its own table rect to
            // their badge (same per-card source as mine, so wait for it to publish).
            if !tableSourceReady(cards) && !lastChance { return nil }
            return cards.enumerated().compactMap { i, c in
                guard let from = tableCardSource(c, fallbackIndex: i) else { return nil }
                return Flight(id: "openpick-\(ev.seat)-\(c.identity)", card: c, from: from.rect,
                              to: badge.offsetBy(dx: CGFloat(i) * 3, dy: 0),
                              angle: 0, fromAngle: from.tilt) }

        case .discard, .cardsToTrash:
            // Table -> discard. Discard cards are public (the kernel does not mask
            // them), so fly the real faces when we have them; fall back to backs by
            // count. Source is the approximate table centre - live, that resolves to
            // the just-cleared battles' centroid (see approximateTableCenter).
            guard discardFrame != .zero else { return nil }
            let cards = ev.cards.compactMap { $0 }
            if cards.isEmpty {
                guard let center = approximateTableCenter() else { return nil }
                let n = max(ev.cards.count, 1)
                return (0..<n).map { i in
                    Flight(id: "opendiscard-\(ev.type)-\(i)", card: nil, from: center, to: discardFrame) }
            }
            // Round-7 (replay bunch): wait for the invisible pre-bout grid to
            // publish the trashed cards' real slots, so a reopened discard sweeps
            // each card off the laid-out table instead of the centre fallback.
            if !tableSourceReady(cards) && !lastChance { return nil }
            // Round-7 #2 ("just make them fly to discard - the simpler solution is
            // better"): each trashed card flies from its OWN real on-table rect
            // (`lastBattleCardFrames`, published per card by FBattleGrid), so the
            // overlay ghost appears exactly where the card was and slides to the
            // pile as one clean motion — no collapsing into a bunched stack at the
            // table centre first (the "identical cards appear very close together"
            // the owner saw). The two fallbacks only fire when a card never
            // rendered on the table (a cover that ended the bout in the same apply,
            // so its slot was never laid out): the old tilt reconstruction, then a
            // staggered table centre.
            return cards.enumerated().map { i, c in
                let src = tableCardSource(c, fallbackIndex: i)
                    ?? (rect: approximateTableCenter() ?? discardFrame, tilt: 0)
                return Flight(id: "opendiscard-\(c.identity)", card: c, from: src.rect,
                              to: discardFrame, angle: 0, fromAngle: src.tilt)
            }

        default:
            return []   // out / flipped / magic-transition: no flight.
        }
    }

    /// On opening a delivered bubble, replay everything that happened since I
    /// last looked (notes 4/9/38), as ORDERED sequential animator steps — one
    /// per log entry, using the same `playStep`/`animator.play` machinery the
    /// interactive bout-end sequence uses (so HARNESS_AUTOGAME's
    /// `BoardAnimator.isSequencing` wait still covers it).
    func replayLastMoveOnOpen(_ view: GameView) {
        AnimLog.say("openReplay events=\(controller.openReplayEvents.count) genesis=\(controller.isGenesis)")
        // The whole open-replay is now the KERNEL's evwire for the last move
        // (controller.openReplayEvents, resolved in begin()). A genesis deal's
        // last move IS the deal, so the same stream drives it - no special case.
        let events = controller.openReplayEvents
        guard !events.isEmpty else {
            // Nothing the kernel gave us to animate. One safety net: a genesis
            // whose chain could not encode a v6 code (empty stream) still flies
            // its opening hand, the web's deal.
            if controller.isGenesis, let hand = view.me?.hand, !hand.isEmpty {
                let ids = Set(hand.map(\.identity))
                animator.preHide(ids)
                let veiledAt = animator.veilEpoch      // round 40 - see playBoutEnd
                // Bug 9: this deal is a sequence like any other - claim the
                // animator so a live move started on top of it supersedes it,
                // and so ITS teardown can never clear a newer sequence's veil.
                let mySeq = claimAnimSequence()
                Task {
                    let hold = BoardAnimator.holdSequence()
                    defer {
                        hold.release()
                        if mySeq == animSequenceToken {
                            animator.clearPreHidden(raisedBy: veiledAt)
                            // Round 43: and the holdback, for the same reason it
                            // is rescued in the two undo teardowns - a genesis
                            // fallback that supersedes a replay is the last
                            // thing that will ever run over those cards.
                            releaseHoldback(raisedBy: veiledAt)
                            // Same rescue as runEventStream's teardown: openSlots
                            // pulled the opening hand OUT of preHidden, so
                            // clearPreHidden can't reveal it if myDrawFlights
                            // never built (frames not ready). Force it visible so
                            // a genesis deal can never end as invisible cards.
                            let stuck = ids.union(orphanedOpens).filter { animator.isHidden($0) }
                            if !stuck.isEmpty {
                                AnimLog.say("genesis rescue-reveal \(stuck.count) opened-but-unflown")
                                animator.reveal(stuck)
                            }
                            orphanedOpens = []
                        } else {
                            orphanedOpens.formUnion(ids)
                        }
                    }
                    // Bug 10: the opening hand has no present cards to pre-shift,
                    // but its slots are still deferred by `preHide` above, so open
                    // them before the deal flight builds (it needs their landing
                    // frames). Round-7 (first-open bunch): SNAP them open (no
                    // animation) so the frames are final at once and each dealt card
                    // flies to its own slot instead of a bunched mid-spread spot.
                    self.animator.openSlots(ids)
                    // A beat for the opened layout to publish, then fly each card to
                    // its analytical final slot (handLandingSlot, via myDrawFlights).
                    try? await Task.sleep(nanoseconds: 100_000_000)
                    await playStep { lastChance in
                        self.myDrawFlights(hand, laidOut: self.laidOutHandNow(view), lastChance: lastChance) }
                    if view.isOver, mySeq == animSequenceToken { settleResults() }
                }
                return
            }
            if view.isOver { showResults = true }   // note 39c: nothing to animate
            return
        }

        // ROUND 21: TAKE THE MARKS OFF `pendingRoles` AND ONTO STATE, here and
        // synchronously - before the Task below is even scheduled.
        //
        // `pendingRoles` only answers while the controller says a replay is
        // outstanding, and `viewChanged` clears that flag on its way out of this
        // call (`consumeReplayPending`), which is several paints before the
        // sequence starts. Without this line the marks would fall back to the
        // FINAL view for that gap and flip twice on their way to being right.
        // Same reasoning as `freezeCounts`: the freeze has to be synchronous
        // with the change that needs it, not one onChange behind.
        //
        // ROUND 44: `.arming`, NOT `.bystander`, and the difference is a real
        // regression rather than a label. This function is seeding the ledger
        // for a sequence it is ABOUT to start, and that sequence does not claim
        // `animSequenceToken` until `runEventStream` runs a Task hop later - so
        // at this instant a PREVIOUS stream may still be in flight (an arrival
        // landing on an open board is exactly that, and is the normal case).
        // Refused here, the replay would open against that stream's mid-state:
        // the wrong numbers and the wrong marks, silently, with no twitch to
        // give it away. See ShownLedger.swift.
        ledger.write(.arming) { $0.seedMarks(from: controller.openReplayPriorState, outs: false) }

        // notes 6/12: hand every real card this open moves - onto the table
        // (attacks/covers/passes) OR into my hand (my own draws/pickups) - to
        // the animator, so it keeps hiding them once `settled` drops the veil.
        // These are the SAME ids `veiledCardIds` has been showing as absent
        // since the first paint (`pendingOpen`), so the handoff is invisible;
        // doing it only here is what used to leave one paint with the cover
        // already landed and rotated - the "starts rotated, un-rotates,
        // re-rotates" (note 6) and "all covers show landed at once, then
        // animate one at a time" (note 12) bugs.
        let touchedIds = controller.openReplayTouchedCardIds
        if !touchedIds.isEmpty { animator.preHide(touchedIds) }
        let veiledAt = animator.veilEpoch              // round 40 - see playBoutEnd

        // …and the counts, likewise taking over from the veil's own
        // `pendingOpen.counts`, which is this same freeze (`AnimPlan.pre`, the
        // kernel's - c/src/anim_plan.c) asked in `body` because there is no
        // prior view on an open: this board IS the first paint.
        let pre = AnimPlan(events, finalView: view).pre
        ledger.write(.arming) { l in
            l.deck = pre.deck
            l.trump = pre.flipped.map(TrumpSlot.card) ?? .gone
            l.discard = pre.discard
            var counts: [Int: Int] = [:]
            for (seat, c) in pre.hand where seat != controller.mySeat { counts[seat] = c }
            l.hand = counts
        }

        // …and MY OWN hand, which the counts above deliberately skip (a seat
        // badge is a number; my hand is the cards). Only my placements, and only
        // when I am not spectating.
        //
        // 1.0(43): this line is the HANDOVER, not the first word. It used to be
        // described as landing before the first paint, and structurally it
        // cannot - this whole function runs from an `onChange`. The veil has
        // been answering the same set since the board's first paint
        // (`fanHoldback`), and what is armed here is what carries it on once the
        // window shuts; the two ask the same kernel rule of the same stream, so
        // nothing moves as it changes hands.
        handHoldback = isSpectating ? []
            : HandLayout.myPlacedCards(events, mySeat: controller.mySeat)
        // Stamped with the veil this open raised, so only a teardown at or after
        // that veil may take it down again - see `handHoldbackAt`.
        handHoldbackAt = veiledAt
        if !handHoldback.isEmpty {
            AnimLog.say("openReplay holds \(handHoldback.count) of my cards in the fan "
                + "[\(handHoldback.map(\.identity).joined(separator: ","))]")
        }

        // The pre-bout table this open sweeps (a pickup or discard). Rendered
        // VISIBLE by `battlesArea` so the cards sit on the table and then fly off
        // it - see `sweepBattles`. Set BEFORE the stream starts so the grid lays
        // out on the next paint, in time for the flight to measure and fly from it.
        // Empty for a plain attack/cover replay (those cards are still on the table
        // in `view`).
        // ROUND 20: whatever this stream PLACES onto that table has not been seen
        // arriving yet, so it starts absent from the grid and flies in - see
        // `sweepUnplaced`. For all but a bout-ending cover this set is empty
        // (nothing is placed and swept in one bubble), and `setSweep` drops
        // anything the grid does not hold a slot for.
        let swept = sweepTableForReplay()
        setSweep(swept, unplaced: AnimBeats(events).placed)

        // …and for the OTHER direction, the row this replay opens on, taken
        // over from `pendingOpen.counts.battles` exactly as the counts above
        // are - same kernel answer, same stream, so nothing on screen changes
        // as the window shuts and the ledger takes it on.
        //
        // ONLY WHEN NOTHING IS BEING SWEPT. A sweep already has a grid of its
        // own that has to OUTLIVE the view emptying the row rather than lag it,
        // and arming the ledger too would hand `battlesArea` a non-empty live
        // table for the whole sweep - which is `shownTable` choosing LIVE over
        // the sweep grid, and the swept cards never drawn at all.
        let opening = Self.replayOpening(sweep: swept, planRow: pre.battles)
        if opening.held { ledger.write(.arming) { $0.battles = opening.row } }

        // The SAME animator the live bout-end uses - one path, the kernel's events.
        // `openReplay: true` opens the fan for a COLD first open so each drawn card
        // flies to its correct slot instead of bunching.
        //
        // THE CONFLICT MODEL (1.0(28)): an arrival landing mid-animation of the
        // previous one must not cut to its own footage - it REVERSES what was
        // in flight first (red, verdict-filtered - see ConflictModel.swift) and
        // only then plays. The pipeline is claimed by `arrivalEpoch` the way a
        // sequence claims `animSequenceToken`: a burst of arrivals each bump it
        // synchronously, so a superseded pipeline hands its collected debt on
        // and never starts its forward replay - "undo whatever is animating,
        // then play ONLY the last", explicitly not a queue.
        arrivalEpoch += 1
        let myEpoch = arrivalEpoch
        // The verdict inputs, captured synchronously with the seed above: what
        // this arrival's stream moves, and the board it opens on. Reading them
        // inside the Task would read whatever a NEWER arrival had published.
        let facts = ConflictFacts(events: events, prior: controller.openReplayPriorState)
        Task {
            let debt = await drainSupersededForReversal()
            guard myEpoch == arrivalEpoch else {
                // A newer arrival owns the pipeline: what we collected is its
                // to reverse, ahead of whatever it collects itself.
                reversalDebt.insert(contentsOf: debt, at: 0)
                return
            }
            await playConflictReversal(debt, facts: facts)
            guard myEpoch == arrivalEpoch else { return }
            await runEventStream(events, finalView: view, openReplay: true, veiledAt: veiledAt)
        }
    }
}
