// THE TABLE AND ITS BATTLES - which row the grid paints, and where a card on it
// starts and ends its flight.
//
// There is only ever ONE grid (`battlesArea`): a bout-end sequence clears
// `view.battles` before the sweep animates, so the pre-bout table is rendered
// THROUGH THE SAME `FBattleGrid` the live table used and the cards keep their
// identity - they sit still and then fly off, instead of one grid fading out
// while another fades in. `setSweep` / `dropSweep` are the only two writers of
// that table, which is what `sweepTableIds` rests on.

import SwiftUI
import Foundation

extension MessageTableView {

    /// Every identity in `sweepBattles`, so a bout-end flight can POLL (via
    /// `playStep`) until its source slot has been measured rather than firing off
    /// the centre fallback before the grid has laid out.
    ///
    /// ROUND 45: COMPUTED, NOT STORED - the one piece of the veil's state that
    /// turned out to be a second copy of another piece rather than a fact of its
    /// own. It was `@State`, written in lockstep with `sweepBattles` at the only
    /// two places either may be written (`setSweep` / `dropSweep`), which is a
    /// cache kept correct by hand. The value is identical by construction, so
    /// nothing observable moves; what goes away is the way a THIRD writer of
    /// `sweepBattles` would leave this stale without a symptom at the write. A
    /// stale entry here is a flight that polls for a rect that will never
    /// publish (`tableSourceReady` blocks on a card the grid does not hold) or
    /// one that refuses a landing the grid has room for (`sweepLandingRect`) -
    /// a cover that simply never animates, which is the round-20 report.
    ///
    /// The rest of the group did NOT collapse, and the reasons are written at
    /// `Veil.grid` and at `sweepUnplaced`.
    private var sweepTableIds: Set<String> { PreBoutTable.cardIds(sweepBattles) }

    func battlesArea(_ view: GameView) -> some View {
        // ONE grid, never two. A bout-end sequence clears `view.battles` before the
        // sweep animates, so during a sweep we render the pre-bout table
        // (`sweepBattles`) THROUGH THE SAME `FBattleGrid` the live table used - the
        // cards keep their identity, so they SIT still and then fly off, instead of
        // the old table grid being torn down (fading out under the board spring) and
        // a separate sweep grid fading in (the owner's "cards fade away while new
        // ones appear and move"). `sweepBattles` is set SYNCHRONOUSLY in `play`
        // before `apply` publishes the empty table, so `shown` never blinks empty
        // for a frame. Each swept card stays visible until its OWN flight starts,
        // when `sweptFlownIds` snaps it hidden (FBattleGrid's `.animation(nil)`) and
        // the overlay ghost carries it the rest of the way - no fade, no gap, no
        // reappear. A settled empty table (nothing sweeping) renders nothing.
        //
        // …EXCEPT ON AN ARRIVAL, WHERE IT DID BLINK EMPTY. That guarantee holds
        // only for a move made HERE: `play` sets the sweep synchronously before
        // `apply` publishes. A bubble that lands while the board is open takes
        // the other road - the arriving view is published first, and the sweep
        // is set inside `replayLastMoveOnOpen`, which runs from the `onChange`
        // AFTER the body has already been evaluated with an empty table. The rig
        // catches it in the act on an opponent's bout-ending good:
        //
        //     grid sweeping=false cells=2 pairs=2 visible=4 hidden=0
        //     grid sweeping=false cells=0 pairs=0 visible=0 hidden=4   <-- blink
        //     grid sweeping=true  cells=2 pairs=2 visible=4 hidden=0
        //
        // Four cards torn off the table and put straight back. Until this round
        // the grid had no explicit transition, so SwiftUI cross-faded both ends
        // of that: the owner's "super annoying glitch with ghost cards fading
        // halfway in quickly and immediatley out", and - because the grid wraps
        // every three battles and its cell count changes with it - the layout
        // "changed its mind mid transition" too.
        //
        // `pendingOpen` is exactly the right window: it is non-nil only between
        // the arriving view landing and `flyBoutEndToDiscard` consuming it,
        // which is the same `onChange` that sets the sweep. And the table it
        // shows is `sweepTableForReplay()` - the one the sweep itself is about
        // to use - so the cards carry the same identities across the handoff and
        // nothing is created or destroyed at all.
        //
        // …AND IT IS NOT ONLY THE EMPTYING MOVES. `view.battles.isEmpty` used
        // to be a third guard on this line, and it is where the pre-bump lived:
        // a pass or a throw-in arrives with a NON-empty table, so this window
        // was skipped and the grid laid out the arrived row on the board's very
        // first painted frame - the pile already down drawn 36pt to the side and
        // never moving, because a cold open has no previous layout to
        // interpolate away from. Only a move that emptied the table could reach
        // the pre-move row. The row a replay opens on is now asked for EVERY
        // stream (`replayOpening`), and which of the three the grid paints
        // is the kernel's (`anim_shown_table`) rather than an emptiness test
        // written out here.
        let pendingTable = sweepBattles.isEmpty && pendingOpen != nil
            ? Self.replayOpening(sweep: sweepTableForReplay(),
                                 planRow: pendingOpen?.counts.battles ?? []).row
            : []
        let table = Self.gridRow(live: view.battles, held: ledger.battles,
                                 sweep: sweepBattles, pending: pendingTable)
        let sweeping = table.sweeping
        let shown = table.shown
        // ROUND 20: the sweep grid hides BOTH ends of the sequence - what has
        // already flown off it, and what has not yet flown onto it (a
        // bout-ending cover being replayed; see `sweepUnplaced`).
        // ROUND 45: …as one value, so the trace below and the grid below THAT
        // cannot answer the question differently. They were two copies of the
        // same pair of ternaries, and the trace is the only thing that says
        // which cards the table is withholding.
        // …AND WHAT IT HAS NOT ARRIVED YET, on that same first paint: while the
        // pending table is the one being drawn, `sweepUnplaced` is still empty
        // (see `pendingSweepUnplaced`). Unioned rather than switched, because
        // the two windows abut: the state is written by the very `onChange`
        // that closes the pending one.
        let pendingUnplacedNow = sweepUnplaced.union(
            Self.pendingSweepUnplaced(placed: pendingOpen?.placed ?? [], table: pendingTable))
        let grid = Veil.grid(sweeping: sweeping, veiled: veiledCardIds,
                             sweptFlown: sweptFlownIds,
                             sweepUnplaced: pendingUnplacedNow,
                             sweepArriving: sweepArriving,
                             flying: Veil.flying(hidden: animator.hidden,
                                                 preHidden: animator.preHidden))
        #if DEBUG
        // What the table is actually PAINTING, logged only when it changes - so
        // a re-layout mid-sweep shows up as a line instead of having to be read
        // off a video frame. Added for the round-12 pickup sweep and kept: it is
        // what caught #11, where a card sat `hidden` on a table nothing was ever
        // going to un-hide (`visible=0 hidden=1` with no flight after it).
        Self.traceGrid(sweeping: sweeping, shown: shown,
                       hidden: grid.hidden,
                       atRest: !BoardAnimator.isSequencing && settled,
                       preHidden: animator.preHidden, veil: animator.hidden,
                       flyingNow: grid.flyingNow,
                       // What is coming DOWN onto this grid: on a sweep the
                       // un-arrived set, on a live table everything the veil is
                       // still holding (nothing ever leaves that grid - a card
                       // on its way off goes through the sweep one).
                       arriving: sweeping ? pendingUnplacedNow : grid.hidden)
        #endif
        // note 34: a pass preview shows the ghost slot instead of a cover highlight.
        // Never while sweeping (the cards are leaving, not a drop target).
        let passPreview = sweeping ? false : passSlotShown(view)
        return Group {
            if !shown.isEmpty {
                FBattleGrid(battles: shown, trumpSuit: view.trumpSuit,
                            coverable: sweeping || passPreview ? [] : highlightBattles(view),
                            onTapBattle: sweeping ? { _ in } : { idx in tapBattle(idx, view) },
                            // Sweeping cards are hidden per-flight (`sweptFlownIds`),
                            // NOT by the hand veil (`veiledCardIds`, which also hides
                            // a picked-up card's HAND copy) - the table copy must
                            // stay up until its own flight lifts it. See `Veil.grid`.
                            hidden: grid.hidden,
                            showGhostSlot: sweeping ? false : passPreview,
                            // Round-7 #7: the covers whose flight is playing this
                            // instant, so the attack beneath one tilts WITH it (same
                            // set that drives `handSlotDeferred`'s "flying now").
                            // ROUND 20: on the sweep grid the only thing that can
                            // be flying ONTO the table is a bout-ending cover
                            // being replayed (`sweepArriving`), and that is
                            // exactly when the attack under it should start
                            // rotating. Everything else a sweep flies is leaving,
                            // with nothing left to tilt onto - hence the empty
                            // set this used to pass unconditionally.
                            flyingNow: grid.flyingNow,
                            marks: true,
                            slides: FBattleGrid.slidesLive,
                            slidesPreview: FBattleGrid.slidesPreviewLive)
            } else {
                // Empty table: render nothing (web parity). A "no battle" label
                // just tells the player what they can already see (owner's call).
                Color.clear
            }
        }
        // The verb hint is NOT attached here any more — round-4 note 4 moved it
        // to follow the fingertip; see the `dragPoint` branch in boardContent.
        .frame(maxWidth: .infinity)   // boardContent's call site adds maxHeight
    }

    /// Battles to highlight — what the in-flight drag (or the current selection)
    /// could cover.
    private func highlightBattles(_ view: GameView) -> Set<Int> {
        let cards = dragCard.map { playCards(for: $0, view) } ?? selectedCards(view)
        return probe(view, cards, .table).coverable
    }


    /// The table a replayed pickup/discard should be shown sweeping off.
    ///
    /// ROUND 12 ("pickup animation sometimes quickly rearranges into grid before
    /// moving to hand for many players"). `controller.openReplayPreBattles` is
    /// the kernel's pre-bout table, which is a REAL board wherever the stream or
    /// the prior board carries one and the flat one-cell-per-card reading only
    /// where neither does. That reading has the right cards in the wrong shape,
    /// and the grid animates every card into its new cell before anything flies
    /// off it - the report.
    ///
    /// So: prefer the REAL table when this board has one. `lastBattles` is the
    /// last table that actually had cards on it (kept by `flyBoutEndToDiscard`),
    /// and it is nearer to hand than anything the kernel can reconstruct. It is
    /// only used when it accounts for every card the sweep is about to move;
    /// otherwise it is a stale table from an earlier bout and the kernel's
    /// answer - which is at least about the right cards - wins.
    func sweepTableForReplay() -> [BattleView] {
        let reconstructed = controller.openReplayPreBattles
        guard !reconstructed.isEmpty else { return [] }
        // Through the kernel's one subset test (`PreBoutTable.covers`, i.e.
        // anim_table_covers), because "accounts for every card" has to mean the
        // same thing here as it does in `coveredSweep` next door.
        return PreBoutTable.covers(lastBattles, reconstructed) ? lastBattles : reconstructed
    }

    /// THE ROW A REPLAY OPENS ON - the whole question, not the half of it that
    /// empties the table - and WHO HOLDS IT.
    ///
    /// `sweepTableForReplay` above answers for a stream that TAKES the row away
    /// and returns nothing for one that puts cards ON it, because the kernel
    /// rule behind it (`anim_pre_bout_table`) was written as "what table is
    /// about to be swept". That decline was the pre-bump: a replayed pass or
    /// throw-in had no pre-move row, so `battlesArea` fell through to the
    /// arrived one and the first painted frame was already rearranged.
    ///
    /// The other half is the plan's (`AnimPlan.pre.battles`, from the kernel's
    /// `anim_pre_stream_table`): the first event's own row with its placement
    /// taken back off. Composed rather than merged, because the sweep half has
    /// something the plan cannot know - `lastBattles`, the real table this board
    /// was showing a moment ago, which round 12 says to prefer over any
    /// reconstruction. A stream is one or the other and never both: a move that
    /// sweeps ends with an empty table, a move that adds does not.
    ///
    /// `held` is which machinery carries it once the sequence starts. An
    /// ADDITION's row goes on the ledger and is walked forward a step at a time
    /// like every other lagging count; a SWEEP's is `sweepBattles`, which has to
    /// OUTLIVE the view that empties the row rather than lag it, and is taken
    /// down by `dropSweep` when its cards have flown. Arming both would hand
    /// `battlesArea` a non-empty live table for the whole sweep, which is
    /// `shownTable` choosing LIVE over the sweep grid and the swept cards never
    /// drawn at all.
    ///
    /// STATIC, and asked at BOTH call sites rather than written out at each:
    /// `battlesArea` renders this row a paint before `replayLastMoveOnOpen`
    /// arms the state that carries it on, and the two answering differently is
    /// a visible handoff - the family `pendingSweepUnplaced` is already in.
    static func replayOpening(sweep: [BattleView], planRow: [BattleView])
        -> (row: [BattleView], held: Bool) {
        sweep.isEmpty ? (planRow, !planRow.isEmpty) : (sweep, false)
    }

    /// WHICH ROW THE GRID PAINTS, over everything that can claim it. The choice
    /// itself is the kernel's (`anim_shown_table`); what is here is which value
    /// plays the part of "live" - the LEDGER's row while a sequence is walking
    /// it forward, the kernel's the moment nothing is animating. Exactly
    /// `shownDeck`'s shape, one field over.
    static func gridRow(live: [BattleView], held: [BattleView]?,
                        sweep: [BattleView], pending: [BattleView])
        -> (shown: [BattleView], sweeping: Bool) {
        PreBoutTable.shownTable(live: held ?? live, sweep: sweep, pending: pending, holdLeaving: UndoFlightSource.holdsLeaving)
    }

    /// note 4: an approximate source rect for a pickup/discard flight replayed
    /// on open — the pre-bout table itself is never rendered (the game is
    /// already past it by the time we open), so there is no real per-battle
    /// rect to fly from. Prefers the last REAL battle rects seen this session
    /// (rare on a fresh open); otherwise a small rect at the board's visual
    /// centre, between the deck/discard corners and the hand, so the flight
    /// still reads as "from the table" rather than from nowhere. nil only
    /// when even the hand hasn't rendered yet (poll again via playStep).
    func approximateTableCenter() -> CGRect? {
        if !lastBattleFrames.isEmpty {
            let rects = Array(lastBattleFrames.values)
            let midX = rects.map(\.midX).reduce(0, +) / CGFloat(rects.count)
            let midY = rects.map(\.midY).reduce(0, +) / CGFloat(rects.count)
            return CGRect(x: midX - 25, y: midY - 35, width: 50, height: 70)
        }
        guard handFrame != .zero else { return nil }
        let y = deckFrame != .zero ? (deckFrame.midY + handFrame.minY) / 2 : handFrame.minY - 140
        return CGRect(x: handFrame.midX - 25, y: y - 35, width: 50, height: 70)
    }

    /// Bug 6: where a specific trashed card ACTUALLY SAT, so it can sweep to the
    /// pile from there instead of from a shared centroid. Two corrections on top
    /// of the raw slot rect from the last table that had cards (`lastBattles` /
    /// `lastBattleFrames`, captured together in `flyBoutEndToDiscard`):
    ///
    /// 1. The card is bottom-aligned in FBattleGrid's taller 62x84 slot, so its
    ///    own centre - which is what a flight is positioned by - sits below the
    ///    slot's centre.
    /// 2. An attack and its cover lie ACROSS each other, pivoting on that same
    ///    bottom edge in opposite directions. Un-nudged they would leave the
    ///    table from exactly the same point and read as one card, which is the
    ///    "bunched stack" all over again just per-battle. A card rotated by
    ///    `tilt` about the bottom edge has its centre swung sideways by
    ///    sin(tilt)·halfHeight and down by (1-cos(tilt))·halfHeight.
    ///
    /// The tilt comes back too, so the ghost flattens out of it in flight
    /// (Flight.fromAngle). nil when there is genuinely no per-card rect on
    /// record (a cold open that never rendered the pre-bout table), so the
    /// caller can fall back to the shared table centre.
    private func discardSource(for card: Card) -> (rect: CGRect, tilt: Double)? {
        guard let idx = lastBattles.firstIndex(where: { $0.attack == card || $0.defense == card }),
              let slot = lastBattleFrames[idx] else { return nil }
        let battle = lastBattles[idx]
        // Only a COVERED attack lies across; an uncovered one stands upright.
        let tilt: Double = battle.defense == card
            ? FBattleGrid.coverAngle
            : (battle.defense != nil ? -FBattleGrid.coverAngle : 0)
        // Slot -> CARD: the two cards sit bottom-aligned in a slot that is taller
        // than they are, so the card's centre is `half` up from the slot's
        // bottom edge. That conversion is all this does. The TILT is not baked
        // in - the ghost applies it (see `sweptTilt`).
        let half = 35.0                      // half of FBattleGrid's 70pt card height
        let rect = slot.offsetBy(dx: 0, dy: slot.height / 2 - CGFloat(half))
        return (rect, tilt)
    }

    /// Round-7 (pickup bunch): where a card currently on the table ACTUALLY sits -
    /// its own per-card rect, published live by FBattleGrid (`lastBattleCardFrames`).
    /// Used by BOTH bout-end sweeps, pickup AND discard, so each card flies from its
    /// own position and the overlay ghost spawns on top of the real card (masking
    /// its fade) instead of every card sharing one table centroid where they pile up
    /// into a "grouped up" stack that then fades in at the middle. Falls back to the
    /// tilt reconstruction, then a staggered centre, only for a card that never
    /// rendered on the table (a cover that ended the bout in the same apply, so its
    /// slot was never laid out).
    func tableCardSource(_ card: Card, fallbackIndex i: Int) -> (rect: CGRect, tilt: Double)? {
        if let src = Self.tableSource(card, battles: sweepBattles.isEmpty ? lastBattles : sweepBattles,
                                      frames: lastBattleCardFrames) { return src }
        if let src = discardSource(for: card) { return src }
        guard let center = approximateTableCenter() else { return nil }
        return (center.offsetBy(dx: CGFloat(i) * 6, dy: CGFloat(i) * 4), 0)
    }

    /// WHERE A CARD ON THE TABLE STARTS ITS FLIGHT - as a pure function, because
    /// this is the whole of the 1.0(33) jump and the whole of the round-12 one,
    /// and they are opposite mistakes about the same line.
    ///
    /// The rect is the card's own published frame, UNCHANGED, and the tilt rides
    /// beside it for the ghost to rotate by. A battle's two cards publish the
    /// SAME frame - `FBattleGrid` stacks them bottom-aligned and separates them
    /// only with `.rotationEffect(anchor: .bottom)`, which is a render transform
    /// and moves no layout frame - so this returning one rect for both is not a
    /// bug being tolerated, it is the fact the ghost's own rotation then acts on.
    /// Rotate once and the ghost lands exactly on the card; rotate twice and the
    /// pair springs apart the instant it lifts (see `sweptTilt`); rotate never
    /// and the pair collapses into a stack (round 12).
    static func tableSource(_ card: Card, battles: [BattleView],
                            frames: [String: CGRect]) -> (rect: CGRect, tilt: Double)? {
        guard let rect = frames[card.identity] else { return nil }
        guard let b = battles.first(where: { $0.attack == card || $0.defense == card })
        else { return (rect, 0) }
        if b.defense == card { return (rect, FBattleGrid.coverAngle) }
        return (rect, b.defense != nil ? -FBattleGrid.coverAngle : 0)
    }

    /// ROUND 20: where a card ARRIVING onto the pre-bout grid is going to land,
    /// and at what tilt - the destination half of `tableCardSource`.
    ///
    /// Only ever answers for a card the grid is actually holding a slot for
    /// (`sweepTableIds`), and only once that slot has published its rect, so a
    /// caller polling through `playStep` waits for a real measurement instead of
    /// flying to a guess. The rect is the RAW slot: unlike a card lifting OFF the
    /// table, a card flying onto it rotates into its tilt over the flight
    /// (`Flight.angle`), so the ghost and the slot agree at the moment it lands
    /// and no bottom-edge swing correction belongs here.
    func sweepLandingRect(_ card: Card) -> (rect: CGRect, angle: Double)? {
        guard sweepTableIds.contains(card.identity),
              let rect = lastBattleCardFrames[card.identity] else { return nil }
        return (rect, sweptTilt(of: card))
    }

    /// How far over a card on the swept table is lying: +`coverAngle` for a
    /// cover, -`coverAngle` for the attack under one, 0 for an uncovered attack.
    /// Mirrors what `FBattleGrid` actually draws (its attack takes the negative
    /// tilt, its defense the positive one).
    ///
    /// THE TILT IS APPLIED ONCE, BY THE GHOST. Round 12 corrected a source rect
    /// by the sideways swing a bottom-anchored rotation produces
    /// (sin(tilt)*halfHeight), because a battle's two cards publish the SAME
    /// layout rect - rotation is a render transform and moves no frame - and
    /// flying both ghosts from it collapsed every pair into a stack. Since then
    /// the ghost rotates itself (`Flight.fromAngle`, about `.bottom`, the same
    /// pivot the grid uses), so that correction became the SECOND application of
    /// one swing: the cover started ~7pt right of where it was drawn and the
    /// attack ~7pt left, then flew to the discard from there. The owner, 1.0(33):
    /// "the cards very slightly jump, cover card to the right and attack card to
    /// the left, before then animating to discard."
    ///
    /// So sources hand over the RAW rect and the tilt beside it, and the ghost -
    /// the one thing that actually draws the card - does the rotating. That is
    /// also what `sweepLandingRect` has always done on the landing side ("no
    /// bottom-edge swing correction belongs here"); the two sides simply
    /// disagreed, and the source side was the one that was wrong.
    private func sweptTilt(of card: Card) -> Double {
        let table = sweepBattles.isEmpty ? lastBattles : sweepBattles
        guard let b = table.first(where: { $0.attack == card || $0.defense == card })
        else { return 0 }
        if b.defense == card { return FBattleGrid.coverAngle }
        return b.defense != nil ? -FBattleGrid.coverAngle : 0
    }

    /// Round-7 (replay bunch): are the on-table SOURCE slots for these swept
    /// cards measured yet? On an open-replay the pre-bout table is laid out
    /// invisibly (`replayPreBattles`) a paint after the stream starts, so a
    /// bout-end flight built on the very first poll would read the centre
    /// fallback and bunch. Gate the build on this (except on the final poll,
    /// `lastChance`, where a rough centre still beats a card that never flies).
    /// Cards NOT in `replayTableIds` (the live board, or an opponent whose cards
    /// aren't reconstructed) never block - they were never going to publish here.
    func tableSourceReady(_ cards: [Card]) -> Bool {
        cards.allSatisfy { !sweepTableIds.contains($0.identity) || lastBattleCardFrames[$0.identity] != nil }
    }

    /// Lay out `battles` as the pre-bout table a bout-end sequence sweeps - the
    /// cards sit VISIBLE on the table (via `battlesArea`) until each flies. One
    /// setter for both the live bout-end (prior view's battles) and the open-replay
    /// (reconstructed). Resets `sweptFlownIds` so nothing is pre-hidden.
    /// `unplaced` are cards this grid holds that have not ARRIVED yet - a
    /// bout-ending cover being replayed, which has to be seen landing before the
    /// table it landed on is carried off (see `sweepUnplaced`). Empty for every
    /// other sweep, where the whole table was already on screen.
    /// Take the pre-bout grid down, marks and all. One function rather than the
    /// four hand-repeated assignments it replaces: round 20 added two more sets
    /// to the group (`sweepUnplaced` / `sweepArriving`), and a grid left standing
    /// with a card marked un-arrived is a card that never comes back.
    ///
    /// THESE TWO ARE THE ONLY WRITERS OF `sweepBattles`, and round 45 turned
    /// that from a convention into the thing `sweepTableIds` rests on - it is
    /// now derived from the table rather than assigned beside it. Pinned by
    /// `VeilOutsTests.testOnlyTheSweepSettersEverWriteTheSweptTable`.
    func dropSweep() {
        sweepBattles = []; sweptFlownIds = []
        sweepUnplaced = []; sweepArriving = []
    }

    func setSweep(_ battles: [BattleView], unplaced: Set<String> = []) {
        sweepBattles = battles
        sweptFlownIds = []
        // Off the ARGUMENT, not off `sweepTableIds`, though the two are now the
        // same value: a setter reading back the state it wrote one line earlier
        // is a habit that only works while the write is synchronous.
        //
        // The SAME kernel rule `battlesArea` already applied to the pending
        // table one paint ago (`pendingSweepUnplaced`). Two intersections
        // written out twice is how those two paints came to disagree.
        sweepUnplaced = Self.pendingSweepUnplaced(placed: unplaced, table: battles)
        sweepArriving = []
    }

    /// WHAT THE GRID IS TOLD once the caller knows WHICH table it is drawing is
    /// `Veil.grid` (c/src/anim_plan.c's anim_veil_grid): the pair of sets
    /// `battlesArea` hands `FBattleGrid`, a straight switch on the same
    /// `sweeping` flag, with the two branches answering off DIFFERENT STATE
    /// rather than a different filter over one. See VeilOutsTests for the pickup
    /// that makes that necessary - the same identity hidden on one grid and
    /// drawn on the other, in the same paint.

    /// Drop the pre-bout table. Called on any view change that empties the table
    /// WITHOUT running a bout-end sequence (reduce-motion, an undo), so a sweep
    /// captured synchronously by `play` can never linger as phantom cards on a
    /// table that isn't actually mid-animation. A no-op when nothing is swept.
    func clearSweep() {
        guard !sweepBattles.isEmpty else { return }
        dropSweep()
    }
}
