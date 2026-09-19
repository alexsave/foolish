// MY HAND - the fan, and where a card lands in it.
//
// Every landing in this file is COMPUTED, never read: `handCardFrames` is a
// preference, so it lags any change that moves the hand by a layout pass, and a
// drawer collapse moves it by most of a screen. The analytical slot is the
// settled answer from the first instant, which is what lets the make-room
// animate WHILE the card flies into it - round 7's "at the same time".

import SwiftUI
import Foundation

extension MessageTableView {

    /// Bug 10: the REAL card identities THIS event lands in MY hand (a deal /
    /// refill / pickup to my own seat). Empty for everything else — an opponent's
    /// draw (masked backs to a badge), a table placement, a discard sweep. These
    /// are exactly the cards whose fan slot `openSlots` cuts as this step begins,
    /// so the fan opens for them then instead of at the whole sequence's start.
    func myHandLandingIds(_ ev: GameEvent) -> Set<String> {
        guard !isSpectating, ev.seat == controller.mySeat else { return [] }
        switch ev.kind {
        case .deal, .refill, .pickup: return Set(ev.cards.compactMap { $0?.identity })
        default: return []
        }
    }


    func myDrawFlights(_ cards: [Card], laidOut: [Card], lastChance: Bool = false) -> [Flight]? {
        if cards.isEmpty { return [] }
        guard deckFrame != .zero, handFrame != .zero else { return nil }
        return cards.enumerated().compactMap { i, c in
            handLanding(c, laidOut: laidOut, index: i, of: cards.count)
                .map { Flight(id: "draw-\(c.identity)", card: c, from: deckFrame, to: $0) }
        }
    }

    /// Round-7 #1: a rough card-sized landing rect for a draw whose exact per-card
    /// slot frame never published in time. SPREAD across the hand fan by index, so
    /// several unresolved cards fly to separate places rather than piling onto one
    /// point and then snapping apart (the first-open "bunch then ungroup"). A
    /// deck->hand flight to about-the-right-place reads far better than the card
    /// silently appearing; nil only if the hand itself hasn't rendered yet.
    /// WHERE A CARD LANDS IN MY HAND, in falling order of authority.
    ///
    /// Three answers, and the order between them is the whole of round 7 plus
    /// round 12. This chain was written out verbatim at four call sites - the
    /// live draw, the replayed deal, the replayed pickup, and the resting ghost
    /// a play leaves behind - which is four chances to get the order wrong, and
    /// the order is the part that matters:
    ///
    ///  1. THE ANALYTICAL SLOT (`handLandingSlot`). The settled answer from the
    ///     first instant, computed off the fan's own geometry rather than read.
    ///     This is what lets the make-room ANIMATE while the card flies into it:
    ///     the published frame is a moving target while the row re-centres, so
    ///     aiming at it lands the card where the hand USED to be going.
    ///  2. THE PUBLISHED FRAME. Only when the analytical route cannot answer -
    ///     a card the fan is not laying out. A preference lags a layout pass, so
    ///     it is second, never first.
    ///  3. A ROUGH SPREAD (`handApproxLanding`), staggered by index so several
    ///     unresolved cards do not pile onto one point and then snap apart. A
    ///     deck-to-hand flight to about the right place reads far better than a
    ///     card that silently appears.
    ///
    /// nil only when the hand has never been measured at all, which is a board
    /// with nothing on screen to fly to.
    func handLanding(_ card: Card, laidOut: [Card],
                             index: Int = 0, of count: Int = 1) -> CGRect? {
        handLandingSlot(card, laidOut: laidOut)
            ?? handCardFrames[card.identity]
            ?? handApproxLanding(index: index, of: count)
    }

    func handApproxLanding(index: Int = 0, of count: Int = 1) -> CGRect? {
        guard handFrame != .zero, count > 0 else { return nil }
        let x = handFrame.minX + handFrame.width * (CGFloat(index) + 0.5) / CGFloat(count)
        return CGRect(x: x - 25, y: handFrame.midY - 35, width: 50, height: 70)
    }

    /// Round-7 ("at the same time"): a card's FINAL resting slot in the hand, in
    /// `boardSpace`, computed analytically (FHandFan.slotRects) rather than read
    /// off the live `handCardFrames`. THIS is what lets the make-room ANIMATE and
    /// the card fly to its true place SIMULTANEOUSLY: the published frame is a
    /// moving target while the row re-centres, but the analytical slot is the
    /// settled one from the first instant. `laidOut` is the set the fan lays out
    /// right now (present cards + whatever this step just opened), so the incoming
    /// card sits at the end exactly where the fan will drop it. nil only before
    /// the hand frame has been measured at all (then the caller falls back to the
    /// live frame / a rough spread).
    func handLandingSlot(_ card: Card, laidOut: [Card]) -> CGRect? {
        guard handFrame != .zero else { return nil }
        let rects = FHandFan.slotRects(cards: laidOut, width: handFrame.width)
        guard let local = rects[card.identity] else { return nil }
        // ANCHOR ON THE FAN'S BOTTOM EDGE, NEVER ITS TOP.
        //
        // The whole point of an analytical slot is that it is the SETTLED
        // answer from the first instant, while the published frame is still
        // mid-slide (see this function's doc above). That held for `laidOut`
        // and for the width, and then leaked straight back in through the one
        // input nobody re-derived: `handFrame.minY`. The fan is BOTTOM-anchored
        // in the board (`boardContent` places it `alignment: .bottom`) and its
        // own box height IS a function of the row count (FHandFan's outer
        // `.frame(height:)`, taken off the laid-out cards) - so a pickup that
        // takes the hand from one row to two moves `minY` UP by a whole row
        // while `maxY` does not move at all.
        //
        // A pickup builds its flight in the SAME MainActor turn as `openSlots`
        // (runEventStream calls playStep, which runs `build` with no suspension
        // in between), so `handFrame` is still the ONE-ROW box when this is
        // asked; and because the row change is animated over `flightTime`,
        // `minY` then keeps moving for exactly as long as the flight lasts. At
        // the shipping metrics that is 86pt of error (a two-row box is 166,
        // a one-row box 80): the top row lands flush on the drawer's bottom
        // edge and the bottom row lands entirely below it. Owner, on a
        // collapsed five-card pickup (Q-hearts, J-clubs, Q-clubs, 7-hearts,
        // J-spades): "the cards on the table kinda started flying a bit down,
        // then literally just diappeared. Vanieshed. No idea where they went.
        // Then they only popped back in once the cards already in my hand
        // stopped rearranging, into the two rows." They popped back in at the
        // right place because the REAL hand copy is un-hidden when the flight
        // lands, at the true slot; only the flight was ever aimed wrong.
        //
        // So derive the top the container WILL have from the bottom edge it
        // already has. `maxY` is the one number a row-count change cannot
        // touch, which makes this right on the first poll and right again on
        // every frame of the make-room, with no waiting for a preference to
        // catch up. Tried and rejected before: waiting a paint before building
        // (that is the "make-room THEN flight" the owner rejected) and snapping
        // the fan open (the "cards jump" they rejected before that).
        return Self.inBoardSpace(local, laidOutCount: laidOut.count, handFrame: handFrame)
    }

    /// The pure half of `handLandingSlot`: a container-local slot rect placed
    /// into `boardSpace` against a hand frame that may still be the WRONG HEIGHT.
    ///
    /// Static and pure in the house style so the anchoring rule can be asserted
    /// without a board - which matters here more than usual, because the two
    /// readings differ by a whole row and both look equally plausible in a diff.
    static func inBoardSpace(_ local: CGRect, laidOutCount: Int,
                             handFrame: CGRect) -> CGRect {
        let willBe = FHandFan.height(count: laidOutCount,
                                     availableWidth: handFrame.width)
        return local.offsetBy(dx: handFrame.minX, dy: handFrame.maxY - willBe)
    }

    /// A rect measured at PLAY TIME, moved into the board it is about to fly in.
    ///
    /// A play's source rects are a snapshot: `playAt` measures each card's hand
    /// slot BEFORE the apply, because by the time the flight builds the hand has
    /// closed over the gap and there is nothing left to measure. That snapshot is
    /// only meaningful against the board box it was taken in, and in the compact
    /// drawer that box can move between the two moments: `MessagesRootView.follow`
    /// holds it at the EXPANDED height for the collapse tween and then hands it
    /// back to the model box in one turn, flipping top-anchored to
    /// bottom-anchored. The board box is `boxHeight > 0 ? boxHeight :
    /// geo.size.height`, so the tween is the ONLY thing that can make it taller
    /// than the drawer, and `playStep`'s own tween gate is what can stall a
    /// build across the flip.
    ///
    /// MEASURED, AND ONE THEORY STRUCK. This used to also claim that "staging
    /// while already compact resizes the compose field and with it the drawer".
    /// It does not: driven on a device with a staged bubble inserted while
    /// compact, the transcript shrinks and the drawer does not move - no `stage
    /// follow` line fires at all. A multicover played while already compact is
    /// clean end to end (ghosts at y=288 in a 332pt board, covers landing
    /// correctly, no `flight-rebase`). What the same run DID show is that the
    /// host reports real noise even on a plain compact open (854 -> 332 -> 298
    /// -> 332), which is why this stays.
    ///
    /// So its scope is narrow and honest: a play whose flight is built across a
    /// box change, which is the collapse tween and nothing else. It is a
    /// mathematical no-op otherwise - `playStep` builds one runloop hop after
    /// the snapshot with the battle rect already published, so no new preference
    /// has arrived and both deltas are zero.
    ///
    /// IT IS NOT THE CURE FOR THE "VANISH". That half was `showHeld` being
    /// planted only for cards WITH a source rect while `preHide` veiled them
    /// all - see `playAt`. Owner, on a double cover of J-hearts and 9-hearts:
    /// "instead of flying over smoothly, the cards just vanish from my hand, and
    /// then fly in from the bottom of the screen to cover the cards."
    ///
    /// THIS IS THE SAME RULE AS `inBoardSpace`, on the other side of the flight:
    /// anchor on the hand's BOTTOM edge, the one number neither a row-count
    /// change nor a drawer resize moves. Anchoring on `minY` would smear a row
    /// change (a cover takes cards OUT of the hand, which can collapse two rows
    /// to one) into the correction and break the case it is meant to fix.
    ///
    /// A no-op on a board that did not move, which is every expanded board and
    /// most collapsed ones - so nothing that works today pays for this. It
    /// leaves a breadcrumb whenever it actually corrects anything: if a report
    /// of a mis-aimed flight ever arrives with no `flight-rebase` line beside
    /// it, the cause is somewhere else and this can come out.
    func rebased(_ rects: [String: CGRect], measuredIn was: CGRect) -> [String: CGRect] {
        guard was != .zero, handFrame != .zero else { return rects }
        let dx = handFrame.minX - was.minX
        let dy = handFrame.maxY - was.maxY
        guard dx != 0 || dy != 0 else { return rects }
        FlightRecorder.note("flight-rebase", "play-time rects by (\(Int(dx)),\(Int(dy)))")
        return rects.mapValues { $0.offsetBy(dx: dx, dy: dy) }
    }

    /// EVERY hand card's settled rect in `boardSpace`, computed rather than read.
    ///
    /// The plural of `handLandingSlot`, and it exists for the takeoff side of a
    /// flight for the same reason that one exists for the landing side: the
    /// published `handCardFrames` is a preference, so it lags any change that
    /// moves the hand by a layout pass, and a drawer collapse moves it by most
    /// of a screen. Falls back to the published frames only when the hand has
    /// never been measured at all, which is the one case the analytical route
    /// cannot answer.
    func handSlotsNow(_ view: GameView) -> [String: CGRect] {
        guard handFrame != .zero else { return handCardFrames }
        let laid = laidOutHandNow(view)
        let local = FHandFan.slotRects(cards: laid, width: handFrame.width)
        guard !local.isEmpty else { return handCardFrames }
        return local.mapValues {
            Self.inBoardSpace($0, laidOutCount: laid.count, handFrame: handFrame)
        }
    }

    /// The hand cards the fan actually lays out at this instant: the whole hand
    /// minus any deal still deferring its slot (the same rule `boardContent` uses
    /// for `laidOutHand`, recomputed here for the flight builders). The incoming
    /// card whose flight is playing now is NOT deferred (its slot is open), so it
    /// is included - which is why `handLandingSlot` can find it.
    ///
    /// IN DISPLAY ORDER, which is the whole point: `handLandingSlot` turns this
    /// array into slot rects BY INDEX, so an array in kernel order describes a
    /// hand nobody is looking at. Round-8 #4 gave the fan a persisted per-game
    /// arrangement but left these flight builders reading the kernel's order, so
    /// on a reopen every dealt card flew to the slot it would have had in an
    /// unsorted hand - the right-hand end - and then snapped into the sorted
    /// hand a frame later ("the deal animation will give the rearranged card to
    /// the right regardless, then suddenly jump to the preferred order"). The
    /// DEBUG SLOTCHECK above already compared against the display order, which
    /// is why it never flagged this: the check and the flights disagreed about
    /// which array they were describing.
    func laidOutHandNow(_ view: GameView) -> [Card] {
        // THE SAME ARRAY THE FAN IS GIVEN, held-back cards included - these
        // rects have to describe the hand that is actually on screen, or a
        // takeoff slot would be computed against a layout nobody is looking at.
        HandLayout.laidOut(hand: HandLayout.fanCards(view.me?.hand ?? [], holding: fanHoldback),
                           deferred: handSlotDeferred,
                           order: MessageGameStore.shared.handOrder(gameId: controller.gameIdString))
    }

    /// - `crop` (round-5 M5b, made continuous in round-6): how much of each hand
    ///   card to hide off the bottom — 0 the whole card (expanded), 1 the top
    ///   half (fully compact drawer), any value between as the drawer collapses.
    ///   See `boardContent`'s `collapse`.
    func hand(_ view: GameView, reserveNoSlot: Set<String>) -> some View {
        // A held-back card is veiled (its TABLE copy must stay invisible until
        // its ghost lands), so the hand has to un-veil its own copy or the fan
        // would reserve the slot and draw nothing - a gap where the card is.
        FHandFan(cards: HandLayout.fanCards(view.me?.hand ?? [], holding: fanHoldback),
                 trumpSuit: view.trumpSuit,
                 // ROUND 43: the held-back cards are drawn as ordinary hand
                 // cards (that is the point of the holdback), so they are also
                 // TAPPABLE and DRAGGABLE unless something says otherwise -
                 // hence `locked`, which gates the gesture and paints nothing.
                 // `disabled` would have done the gating and dimmed them to
                 // 0.5, which is the one thing the holdback exists to prevent.
                 locked: Set(fanHoldback.map(\.identity)),
                 selection: $selection, onTap: { toggle($0) },
                 onDragChanged: { card, point in onDragChanged(card, at: point) },
                 onDragEnded: { card, point in onDragEnded(card, at: point, view) },
                 hidden: Veil.fan(veiled: veiledCardIds, holdback: fanHoldback),
                 onDragCardMoved: { center in dragCardCenter = center },
                 reserveNoSlot: reserveNoSlot, instantExit: true,
                 // Round-8 #4: a sorted hand survives closing and reopening the
                 // game. Seed the fan's cosmetic order from this game's stored
                 // arrangement (the kernel hand stays canonical; the fan only
                 // renders it in this order) and persist every reorder back.
                 // The rows die with the game (MessageTurnController clears
                 // them when a chain finishes).
                 initialOrder: MessageGameStore.shared.handOrder(gameId: controller.gameIdString),
                 onOrderChanged: { [gameId = controller.gameIdString] in
                     MessageGameStore.shared.setHandOrder($0, gameId: gameId)
                 })
            .padding(.horizontal, FSpace.s)
    }
}
