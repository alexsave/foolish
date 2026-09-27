// PLAYING A MOVE - the selection, the kernel probe that resolves it, and the
// flight that carries the cards out of the fan.
//
// The board is a dumb selection: the KERNEL turns (selection, target) into one
// legal move (`fio_play_probe` via PlayWire), exactly like the app's TableView.
// What is this file's is the TIMING - every veil a play raises is raised
// SYNCHRONOUSLY, before `apply` can publish, because the onChange that would
// otherwise do it fires a paint too late.

import SwiftUI
import Foundation

extension MessageTableView {

    /// The cards a play would use right now: the whole selection if the dragged (or
    /// tapped) card is part of it, else just that one card (web selected-or-single).
    func playCards(for card: Card, _ view: GameView) -> [Card] {
        selection.contains(card.identity) ? selectedCards(view) : [card]
    }

    // MARK: bout-end flight builders (each returns nil until its frames are ready)

    /// note 17: the cover-that-ends-the-bout's own landing step — its cards
    /// fly from their hand rects AT PLAY TIME (snapshotted before `apply`,
    /// since the hand has already moved on by the time this plays) to the
    /// battle they covered. A pure snapshot, so no retry: what was measured
    /// is what there is.
    func pendingCoverLandingFlights(_ pc: PendingCover) -> [Flight]? {
        // The snapshot moves into TODAY's board first - see `rebased`. The
        // LANDING rects need no such treatment: they were measured off the live
        // `battleFrames`, and it is only the hand side that is frozen from
        // before the apply.
        Self.coverLandingFlights(cards: pc.cards, landing: pc.landing,
                                 fromRects: rebased(pc.fromRects,
                                                    measuredIn: pc.handFrameAtPlay))
    }

    /// Where each card of a cover is going to LAND, measured before the apply
    /// (the table it lands on is about to be swept away, so there is nothing to
    /// measure afterwards).
    ///
    /// A cover pairs its cards with its targets POSITIONALLY - `cards[i]`
    /// answers `attackCards[i]` - which is the shape `PackedAction.encode`
    /// writes to the wire and `calc_cover_moves_greedy` builds. So a three-card
    /// cover has three landing slots, one per attack it answers, and this walks
    /// that pairing rather than reusing the slot the gesture happened to name.
    ///
    /// `frames` is the live measurement and `fallback` the last non-empty one,
    /// the same pair the single-rect version used. A card whose battle cannot be
    /// located is dropped, not defaulted: a wrong rect is a card flying to the
    /// wrong place, which is worse than the sweep carrying it off from rest.
    static func coverLandingRects(move: Move, battles: [BattleView],
                                  frames: [Int: CGRect],
                                  fallback: [Int: CGRect]) -> [String: CGRect] {
        guard move.type == .cover else { return [:] }
        let targets = move.attackCards ?? []
        guard targets.count == move.cards.count else { return [:] }
        var out: [String: CGRect] = [:]
        for (card, attack) in zip(move.cards, targets) {
            guard let idx = battles.firstIndex(where: { $0.attack == attack }),
                  let rect = frames[idx] ?? fallback[idx] else { continue }
            out[card.identity] = rect
        }
        return out
    }

    /// note 17's landing step: each covering card from its hand rect AT PLAY
    /// TIME to its own battle. A pure snapshot, so no retry - what was measured
    /// is what there is.
    ///
    /// `angle` is set here for the same reason `placementFlights` and
    /// `openReplayFlights` set it: a cover lies across, so its ghost rotates
    /// into the tilt over the flight instead of arriving flat and snapping the
    /// instant the real card takes its place.
    static func coverLandingFlights(cards: [Card], landing: [String: CGRect],
                                    fromRects: [String: CGRect]) -> [Flight] {
        cards.compactMap { c in
            guard let from = fromRects[c.identity], let to = landing[c.identity] else { return nil }
            return Flight(id: "coverland-\(c.identity)", card: c, from: from, to: to,
                          angle: FBattleGrid.coverAngle)
        }
    }

    /// Round-6 bug 13: fly a play of MINE onto the table - from the release point
    /// (or the hand slot, for a tap) to the slot it landed in. The cards are
    /// already hidden (`playAt` pre-hid them before the apply), so what the
    /// viewer sees is one continuous movement out of the fingertip.
    ///
    /// Holds `sequenceDepth` like every other animated sequence, which is
    /// what makes the extension's auto-collapse (note 8's `waitForSettle`) hold
    /// off until the card has actually landed instead of yanking the drawer down
    /// mid-flight.
    func flyPlacement(_ pp: PendingPlacement, to view: GameView) {
        let ids = Set(pp.cards.map(\.identity))
        Task {
            let hold = BoardAnimator.holdSequence()
            defer {
                hold.release()
                // The card must end up visible whatever happened. If it really
                // flew, `BoardAnimator.play` already un-hid it and this is a
                // no-op; if its landing slot never published and `playStep` gave
                // up, this is the safety net. Targeted at THESE ids rather than
                // `clearPreHidden`, which would also reveal whatever a newer
                // sequence has pre-hidden but not yet flown (round-6 bug 9).
                animator.reveal(ids)
            }
            await playStep { _ in self.placementFlights(pp, view: view) }
        }
    }

    /// The flights for one placement: each card from where it left to the battle
    /// slot it landed in, in the FINAL view. nil (retry) while a landing slot's
    /// rect has not published yet - an attack or a pass CREATES its slot, so the
    /// frame only exists a paint or two after the apply. A card that is not on
    /// the table at all any more is skipped rather than retried (best-effort,
    /// like `openReplayFlights`), so a lost card can never wedge the poll.
    private func placementFlights(_ pp: PendingPlacement, view: GameView) -> [Flight]? {
        var out: [Flight] = []
        // Play-time rects, moved into the board this flight actually plays in -
        // see `rebased`. Same snapshot, same hazard as the cover landing above.
        let sources = rebased(pp.fromRects, measuredIn: pp.handFrameAtPlay)
        for c in pp.cards {
            guard let idx = view.battles.firstIndex(where: { $0.attack == c || $0.defense == c })
            else { continue }
            guard let rect = battleFrames[idx] else { return nil }
            let from = sources[c.identity] ?? handCardFrames[c.identity] ?? rect
            // Round-6 bug 1 (batch 3) parity: a card landing as the DEFENSE lies
            // across, so its ghost rotates INTO that tilt over the flight rather
            // than arriving flat and snapping tilted when the ghost is removed.
            let landedAngle = view.battles[idx].defense == c ? FBattleGrid.coverAngle : 0
            out.append(Flight(id: "place-\(c.identity)", card: c, from: from, to: rect,
                              angle: landedAngle))
        }
        return out
    }

    // MARK: interaction (mirrors TableView — every branch reads the kernel menu)

    func play(_ move: Move) {
        // ROUND 20: nothing is played on a board branching off an old bubble.
        // The buttons are already gone (`acting` reads `iCanAct`, which stands
        // down), and `MessageTurnController.apply` refuses too - this is the
        // middle of the three, and the one that matters for a DRAG, which
        // reaches the kernel without ever asking a button whether it was
        // enabled. Silent, deliberately: the bar above the board has already
        // said why, and a "move not allowed" toast on top of it would read as a
        // rule about the move rather than about the bubble.
        guard !controller.superseded else { releaseLivePlayVeil(); return }
        // In the same turn as the selection clearing - otherwise that paint
        // shows the bar for an attacker with nothing selected: "Good".
        playInFlight = ActionPillSlot.holdsWhilePlaying
        selection.removeAll()
        // The veil, live half (round-4 note 5). Both of these are the state as
        // it is RIGHT NOW, captured before `apply` can publish a new view —
        // which is the only moment early enough, since the onChange that would
        // otherwise do it runs a paint after the board has already drawn the
        // result. Without them a pickup drew its cards into the fan, then hid
        // them and flew them into the fan again (the "double pickup animation"),
        // and every opponent's badge jumped to its final count before counting
        // there (the twitch).
        if let view = controller.view {
            handBeforeMyMove = Set((view.me?.hand ?? []).map(\.identity))
            freezeCounts(to: view)
            // Round-7 (live fade): a move that CLEARS the table (I take the cards, or
            // I say good and the covered table goes to the discard) must show those
            // cards SITTING on the table the very paint `view.battles` empties - not a
            // frame of blank table (grid torn down -> cards fade out) followed by the
            // sweep grid fading them back in. Capturing the table NOW, synchronously
            // before `apply` publishes the empty view, is the only moment early
            // enough (the onChange that re-sets this fires a paint too late, exactly
            // like `handBeforeMyMove`). `flyBoutEndToDiscard` re-sets it from
            // `old.battles` (identical) and owns the teardown; a move that does NOT
            // clear the table renders `view.battles` and ignores this.
            if (move.type == .pickup || move.type == .good), !view.battles.isEmpty {
                setSweep(view.battles)
            }
        }
        Task {
            // ROUND 40: A REFUSED MOVE GIVES THE VEIL BACK.
            //
            // `playAt` veiled these cards synchronously, before this Task
            // existed, because that is the only moment early enough to beat the
            // paint. Everything that takes the veil down again hangs off what
            // happens NEXT - the view change (`flyBoutEndToDiscard`, which
            // consumes `pendingPlacement` on every call) or `rejectTick`. When
            // the kernel never saw the move there is neither, and until this
            // round the cards simply stayed pre-hidden: excluded from a CENTRED
            // fan for the life of the board, which is the owner's
            // `laid=1 hand=4 ... deferred=3 ... seq=0` breadcrumb exactly.
            //
            // `apply` now says so rather than returning in silence, and false
            // is unambiguous: no view change is coming, so there is no race
            // with the onChange that would otherwise own these cards.
            let applied = await controller.apply(move)
            if !applied { releaseLivePlayVeil() }
            await stageNow()
            playInFlight = false
            passHeldAt = nil
        }
    }

    /// Compose + stage the bubble the instant a move is applied (§11.4 flow, B4
    /// feedback: "too many buttons before you can send"). Playing an attack /
    /// cover / pickup / pass / good drops you straight to the staged message, so
    /// the only remaining action is the send arrow. Re-staging replaces the input
    /// bubble, so throwing in more cards just updates it. No-op until something is
    /// staged (a 0-action genesis body is unsealable).
    func stageNow() async {
        guard controller.canStage else { return }
        // `lastChangeWasUndo` is the one signal that separates a re-stage after Undo
        // (keep expanded) from a fresh move's stage (collapse to Send). It is set by
        // undo() and reset by apply()/markSent, so it is true here ONLY when this
        // stage follows an undo.
        if let payload = try? await controller.stagedPayload() {
            await onSend(payload, controller.lastChangeWasUndo)
        }
    }

    /// Undo-to-empty on a continuation (1.0(4)): re-seal the base (received) state
    /// and stage it, REPLACING the stale move bubble the host still holds. This is
    /// the closest to "cancel the staged move" the Messages API allows — there is
    /// no call to remove an inserted bubble, so the move is overwritten with a
    /// bubble that carries nothing new (sending it just re-shares the same board).
    ///
    /// ROUND 16: and it now SAYS SO. The kernel sees that nothing was applied
    /// since it adopted the chain and seals msg_wire.h's MSG_NEW_NOTHING, so a
    /// recipient who opens this bubble animates nothing and its clock does not
    /// restart the pickup hold. Before that it claimed a delta of one and every
    /// recipient replayed the PREVIOUS player's move - the owner's "you can
    /// still send a message and it will look weird for the other players.
    /// Sometimes even play a weird undo animation."
    func stageBaseNow() async {
        guard controller.isContinuation else { return }
        // Always a consequence of undo-to-empty (the only caller is onUndo), so keep
        // the board expanded - never collapse on an undo.
        if let payload = try? await controller.stagedPayload() {
            await onSend(payload, controller.lastChangeWasUndo)
        }
    }


    // Dumb selection; the KERNEL resolves (selection, target) into one legal move
    // (fio_play_probe via PlayWire), exactly like the app's TableView - both ask
    // about the menu they were published, never about a rule of their own.

    /// One kernel answer about the current selection, for every question this
    /// board asks about it. `controller.legalPacked` is the PUBLISHED menu, which
    /// is deliberately empty while a bout settlement is held back.
    func probe(_ view: GameView, _ cards: [Card], _ target: PlayTarget) -> PlayProbe {
        PlayWire.probe(menu: controller.legalPacked, battles: view.battles,
                       powerSuit: view.powerSuit, isDefender: view.defender == controller.mySeat,
                       selection: cards, target: target)
    }

    func toggle(_ card: Card) {
        selection = Veil.selectionAfterTap(selection, card: card,
                                           hand: controller.view?.me?.hand ?? [])
    }

    /// ROUND 43: THE SELECTION MAY ONLY EVER NAME CARDS THAT ARE IN MY HAND.
    ///
    /// It used to be a plain toggle, and that was true by accident for as long
    /// as the only tappable cards were hand cards. Round 42's `handHoldback`
    /// broke it: a card I have already played stays DRAWN in the fan while its
    /// replay flies, so it could be tapped, and the identity it inserted was one
    /// the kernel hand no longer contained. `selectedCards` filters through
    /// `view.me.hand`, so the move was never playable - but the identity stayed
    /// in `selection` for the life of the board, and `actionBar` gates BOTH
    /// `canPickup` and `canDone` on `cards.isEmpty`. Pick that table up and the
    /// card comes home already selected: Take and Good silently gone, with
    /// nothing on screen to explain it and no way to deselect a card that is not
    /// there. The lock in `hand` stops the tap arriving; this stops the state
    /// from being representable at all, which is the invariant the bug was
    /// really about.
    ///
    /// Pure and static so the rule can be asserted without a board.
    static func selectionAfterTap(_ selection: Set<String>, card: Card, hand: [Card]) -> Set<String> {
        let mine = Set(hand.map(\.identity))
        // Sweep first: an identity that has since left my hand (played, swept,
        // discarded) is dropped whatever this tap was for. A stale one can only
        // ever go on to disable the action bar.
        var next = selection.intersection(mine)
        guard mine.contains(card.identity) else { return next }
        if next.contains(card.identity) { next.remove(card.identity) }
        else { next.insert(card.identity) }
        return next
    }

    func selectedCards(_ view: GameView) -> [Card] {
        (view.me?.hand ?? []).filter { selection.contains($0.identity) }
    }

    /// Tap an uncovered attack while cards are selected → cover it (two-tap cover).
    func tapBattle(_ index: Int, _ view: GameView) {
        playAt(.battle(index), selectedCards(view), view)
    }

    /// `released` (round-6 bug 13) is the card the finger was holding and the
    /// board-space centre it was let go at, when this play came from a drag; nil
    /// for a tap/button play, which starts from the hand slot as it always has.
    func playAt(_ target: PlayTarget, _ cards: [Card], _ view: GameView,
                        released: (card: Card, centre: CGPoint)? = nil) {
        guard !controller.superseded else { return }   // round 20 - see `play`
        guard let move = probe(view, cards, target).move else {
            Haptics.fire(.reject); toast = FStrings.t("ios.reject"); return
        }
        // Bug 13: where each card of this play leaves from. ONE answer, used by
        // both landing animations below - the ordinary placement flight and note
        // 17's cover-that-ended-the-bout - so a dragged card cannot start from
        // the release point in one and from the hand in the other.
        // TAKE OFF FROM THE SETTLED SLOT, NOT THE LAST PUBLISHED ONE.
        //
        // `handCardFrames` is a PREFERENCE, and a preference is one layout pass
        // behind whatever moved the view it describes. Collapsing the drawer
        // moves the whole board, and the rig catches the gap in the act:
        //
        //     stage follow geo=667->261 armed=false
        //     SLOTCHECK MISMATCH n=11 worst=391.0pt @3-12
        //
        // 391 is exactly the distance the hand travelled (mid y 580 expanded,
        // 189 compact). `handFrame` had already re-published; the per-card
        // frames had not. A cover played in that window snapshots hand rects
        // from the EXPANDED board, so the ghost is planted ~391pt below a
        // 261pt drawer - off-screen - and then flies up into the table. Owner:
        // "the cards just vanish from my hand, and then fly in from the bottom
        // of the screen to cover the cards. I suspect this is some geometry
        // coordinate mishap for the collapsed mode. seen this a lot in many
        // forms."
        //
        // The cure is the one round 7 already applied to the LANDING side and
        // never to this one: compute the slot, do not read it. `handSlotsNow`
        // derives every card's resting rect from `handFrame` (which is fresh)
        // and the fan's own pure geometry, so a takeoff is right on the first
        // frame after any resize instead of one paint later. The release point
        // of a DRAGGED card still comes from the gesture, which is live by
        // construction - see `playSourceRects`.
        let fromRects = Self.playSourceRects(cards: cards, handRects: handSlotsNow(view),
                                             released: released.map { ($0.card.identity, $0.centre) })
        // note 17: a cover might end the bout in the SAME kernel apply as the
        // cover itself (the defender's hand empties) — stash enough, BEFORE
        // applying, for flyBoutEndToDiscard to synthesize the landing step it
        // would otherwise have no rendered state to animate from.
        //
        // Read off the MOVE, not off `target`: the move is the kernel's own
        // answer for which attack each card covers, and it is the only one that
        // holds for a multicover (where the gesture names one slot but the play
        // lands on several). It also covers the cover a `.table` drop resolves
        // to, which the old `case .battle` guard silently skipped - that one
        // reached a bout end with no landing flight at all.
        let landing = Self.coverLandingRects(move: move, battles: view.battles,
                                             frames: battleFrames, fallback: lastBattleFrames)
        pendingCover = landing.isEmpty ? nil
            : PendingCover(cards: cards, landing: landing, fromRects: fromRects,
                           handFrameAtPlay: handFrame)
        // Bug 13: hand the placement to `flyBoutEndToDiscard`, and hide the cards
        // NOW - synchronously, before `apply` can publish a view with them
        // already sitting on the table. This is the same veil trick as
        // `handBeforeMyMove` and for the same reason (an onChange fires a paint
        // too late): without it the card would paint landed, vanish, and only
        // then fly. `animator.preHide` is the one hiding mechanism the board
        // already has, so the handoff to the flight's own `hidden` set is
        // seamless, and any path that ends up not flying them reveals them again.
        //
        // Read BEFORE the slot is overwritten - this is the play (if any) whose
        // veil is about to be disowned; see `Veil.handover`.
        let standingVeil = pendingPlacement.map { Set($0.cards.map(\.identity)) } ?? []
        pendingPlacement = PendingPlacement(cards: cards, fromRects: fromRects,
                                            handFrameAtPlay: handFrame)
        // Round-8 (atomic takeoff): the same instant the hand copy is veiled above,
        // put a resting ghost where each card WAS, so the swap is seamless - no
        // frame where the card is neither in the hand nor in the overlay. The real
        // `place-<id>` flight (`placementFlights` -> `animator.play`) reuses these
        // ids and simply starts moving them once the kernel publishes the table
        // slot. `fromRects` is the card's own hand slot (or drag-release point).
        //
        // AND EVERY VEILED CARD MUST GET ONE. `preHide` above hides ALL of
        // them; this plants a ghost only for the ones with a source rect, so a
        // card that has none is veiled into thin air - no hand copy, no
        // overlay, nothing. It then flies from whatever `placementFlights`
        // falls back to, which is the destination itself. That is the owner's
        // "the cards just vanish from my hand, and then fly in from the bottom
        // of the screen to cover the cards", and the asymmetry between these
        // two lines is the whole of it.
        //
        // Computing `fromRects` (`handSlotsNow`) was not enough on its own: the
        // analytical slots are cut for the cards the fan LAYS OUT, and a card
        // that is already veiled by an earlier move is not one of them. The rig
        // reached it inside a minute -
        //
        //     held ghost at source [0-10]
        //     NO GHOST for [0-10] - they will vanish
        //
        // the same card played twice, the second time with its slot already
        // given up. So the veil and the ghost are now decided TOGETHER, from one
        // list, and a card that cannot be placed is not veiled either. The
        // fallbacks descend from exact to approximate and only ever run out at
        // a hand that has never been measured at all, which is a board with
        // nothing on screen to vanish from.
        let held: [Flight] = cards.compactMap { c in
            // The release point of a DRAGGED card first - it is live by
            // construction and is where the finger actually let go - then the
            // ordinary landing chain for everything that never moved.
            let at = fromRects[c.identity] ?? handLanding(c, laidOut: laidOutHandNow(view))
            return at.map { Flight(id: "place-\(c.identity)", card: c, from: $0, to: $0) }
        }
        let placed = Set(held.compactMap { $0.card?.identity })
        if placed.count != cards.count {
            let lost = cards.map(\.identity).filter { !placed.contains($0) }
            FlightRecorder.note("no-ghost", "unplaceable, left visible: \(lost.joined(separator: ","))")
            AnimLog.say("NO GHOST for [\(lost.joined(separator: ","))] - left in the hand rather than veiled")
        }
        // ONLY what has somewhere to be. A card veiled with no ghost is gone from
        // the screen entirely - no hand copy, no overlay - and that is the
        // owner's "the cards just vanish from my hand". Leaving it visible for a
        // beat is the strictly better failure: the worst case is a card that
        // appears twice for one frame, against one that disappears completely.
        // ONE LEDGER, ONE VEIL (round 40). `pendingPlacement` holds exactly one
        // placement, so a second play landing before the first's view change
        // silently DISOWNS the first's cards - nothing is left holding their
        // ids, and `flyBoutEndToDiscard`'s reveal only ever sees the survivor.
        // Those cards then sit in `preHidden` for the life of the board, laid
        // out nowhere. Cheap to reach: `apply` is awaited, so the whole kernel
        // round trip is a window a second tap fits inside. So the veil is
        // handed over with the ledger, in one place - see `Veil.handover`.
        let handover = Veil.handover(standing: standingVeil, placing: placed)
        if !handover.reveal.isEmpty {
            AnimLog.say("veil handover: a second play disowned [\(handover.reveal.sorted().joined(separator: ","))]")
            animator.cancelHeld(handover.reveal)
            animator.reveal(handover.reveal)
        }
        animator.preHide(handover.veil)
        animator.showHeld(held)
        play(move)
    }

    /// Bug 13: the rect each card of a play LEAVES FROM. Everything starts at its
    /// own resting hand slot - which is where the untouched rest of a multi-card
    /// selection, and every tap-played card, genuinely is - except the one card a
    /// finger was dragging, which starts centred on wherever it was let go.
    /// Flights are positioned by their rect's CENTRE (FlyingCardsLayer), so the
    /// synthesized release rect only has to be a 50x70 card box around that
    /// point, the same shape `approximateTableCenter` builds. Pure + static so
    /// the release-point substitution can be asserted without a live drag.
    static func playSourceRects(cards: [Card], handRects: [String: CGRect],
                                released: (id: String, centre: CGPoint)?) -> [String: CGRect] {
        var out = handRects.filter { pair in cards.contains { $0.identity == pair.key } }
        if let r = released, cards.contains(where: { $0.identity == r.id }) {
            out[r.id] = CGRect(x: r.centre.x - 25, y: r.centre.y - 35, width: 50, height: 70)
        }
        return out
    }

    /// Cover button: cover the BIGGEST uncovered attack the selection can beat
    /// (round 16 - the kernel's `play_best_cover_target`; the drag path names
    /// its own target and is untouched).
    func playCover(_ cards: [Card], _ view: GameView) {
        guard let i = probe(view, cards, .table).bestCover else {
            Haptics.fire(.reject); return
        }
        playAt(.battle(i), cards, view)
    }

    private func coverableBattles(_ view: GameView) -> Set<Int> {
        probe(view, selectedCards(view), .table).coverable
    }
}
