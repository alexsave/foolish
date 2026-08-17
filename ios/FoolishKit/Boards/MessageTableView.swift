// MessageTableView — the INTERACTIVE expanded bubble (design §10). Same tap
// grammar as the app's TableView (tap a card to attack, select-then-tap-a-battle
// to cover, the wooden bar for pass/pickup/done) but driven by a
// MessageTurnController: a turn here STAGES a chain rather than committing to a
// live game, and the human presses Send (§11.4), never the code.
//
// Every enable state is the kernel's legal menu (`controller.legal`), never a
// hand-rolled "is it my turn" (§17.16). When my seat has no legal move I am a
// spectator on someone else's staged bubble — read-only, with a hint who is up.

import SwiftUI
import Foundation   // sin/cos for the ring placement

public struct MessageTableView: View {
    @ObservedObject private var controller: MessageTurnController
    /// Seal the staged chain and hand it to the extension to compose + insert.
    /// The view never touches MSMessage; it only produces the payload.
    private let onSend: (Data) async -> Void
    /// Start a fresh game in this thread. Offered on the board only once the game
    /// is over (the fool is decided) — any player, out or not, can deal the next
    /// one. Routes through the host's New game (§5.2), same as the chrome button.
    private let onNewGame: () -> Void
    /// Retract a bubble already staged with the host (§10 undo). Undo alone can't
    /// do this: rebuilding the base + replaying `pending` minus the last action is
    /// a LOCAL replay, but the host (harness `staged`, or the real extension's
    /// already-inserted input-field bubble) is a separate piece of state that only
    /// the host can clear. Called when an undo empties `pending` entirely.
    private let onUnstage: () -> Void

    @State private var selection: Set<String> = []
    @State private var toast: String?
    // 1.0(4): the left Settings/Help squares present these.
    @State private var showSettings = false
    @State private var showRules = false
    // Drag-to-play state (frames published by FBattleGrid/FHandFan in `boardSpace`).
    @State private var battleFrames: [Int: CGRect] = [:]
    @State private var handFrame: CGRect = .zero
    @State private var dragCard: Card?
    /// notes 33/34: the drag's live point in `boardSpace`, kept (FHandFan
    /// already delivers it on every `onDragChanged`, previously discarded)
    /// so the verb hint and the pass ghost-slot preview can resolve the SAME
    /// drop target `onDragEnded` will use. nil whenever no drag is active.
    @State private var dragPoint: CGPoint?
    /// The dragged card's own LIVE visual centre in `boardSpace`, as reported by
    /// FHandFan's `onDragCardMoved` on every `onDragChanged`. Round-5 finding 5
    /// used it to re-centre `dragHint` HORIZONTALLY on the card; round-6 bug 5
    /// anchors the pill to it on BOTH axes (a fingertip is not where the card
    /// is), and round-6 bug 13 reads it one last time at release as the point a
    /// played card flies FROM. nil whenever no drag is active (set and cleared
    /// alongside `dragPoint`, in `onDragChanged`/`onDragEnded`).
    @State private var dragCardCenter: CGPoint?
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    // Round-8: this board has NO card-flight matchedGeometry namespace (unlike the
    // offline TableView, where matchedGeometry IS the primary flight). Here the
    // overlay (`BoardAnimator`) owns EVERY flight - attacks/covers via
    // flyPlacement, deals/pickups/discards via the event stream - so a shared
    // namespace would only DOUBLE-animate: SwiftUI would fly a card hand↔table on
    // its own, cross-fading between the two matched copies, at the same time the
    // overlay flies it. That cross-fade is an opacity animation (the owner's hard
    // rule: a card is 1.0 or 0.0, never a fade), and it is exactly the "solid card
    // + a ghost that fades in at the destination" seen on UNDO - the one move the
    // overlay does NOT own, so the card returns table→hand purely by
    // matchedGeometry. With no namespace, undo (and any non-overlay move) SNAPS the
    // card home instantly, which is what an instantaneous swap should look like.
    private var cardNS: Namespace.ID? { nil }
    // Overlay flights to the discard pile (bout end), where matchedGeometry has no
    // target view to match against.
    @StateObject private var animator = BoardAnimator()
    @State private var lastView: GameView?
    @State private var discardFrame: CGRect = .zero
    /// The most recent NON-empty battle rects, so a bout-end flight still has the
    /// source positions after the table cleared (preference vs onChange can race).
    @State private var lastBattleFrames: [Int: CGRect] = [:]
    /// Round-6 bug 6: the most recent NON-empty battle LAYOUT (which card sat in
    /// which slot), captured alongside `lastBattleFrames`, so a discard sweep can
    /// fly each trashed card FROM ITS OWN battle rect instead of every card
    /// sharing one centroid (which read as a single stack sliding to the pile).
    /// Keyed by the same battle index as `lastBattleFrames`.
    @State private var lastBattles: [BattleView] = []
    /// Round-7 #2: each battle CARD's real on-table rect (identity -> rect),
    /// kept at its last non-empty value so a bout-end discard sweep can fly each
    /// trashed card from exactly where it sat, not a shared table centroid.
    @State private var lastBattleCardFrames: [String: CGRect] = [:]
    /// The pre-bout TABLE a bout-end sequence is about to sweep - the cards that
    /// were on the table right before a pickup/discard cleared it. Rendered in the
    /// battle area (VISIBLE) while `view.battles` is empty, so the swept cards SIT
    /// on the table and then fly off it - exactly what you watch live - instead of
    /// the table going empty and a ghost spawning out of nowhere. Each card is
    /// hidden the instant ITS flight starts (`sweptFlownIds`), so the overlay ghost
    /// takes over seamlessly (no fade, no gap, no reappear).
    ///
    /// Used by BOTH paths:
    ///  - live bout-end: the prior view's battles (the table I just cleared).
    ///  - open-replay: `controller.openReplayPreBattles` (reconstructed, since the
    ///    pre-bout table was never otherwise rendered on open).
    /// Set as the sequence begins, cleared as it ends.
    @State private var sweepBattles: [BattleView] = []
    /// Every identity in `sweepBattles`, so a bout-end flight can POLL (via
    /// `playStep`) until its source slot has been measured rather than firing off
    /// the centre fallback before the grid has laid out.
    @State private var sweepTableIds: Set<String> = []
    /// Swept cards whose flight has STARTED - hidden in the pre-bout grid from that
    /// instant on (the overlay ghost is now the only copy). Grows through the
    /// sequence, cleared with `sweepBattles`. Kept separate from `animator.hidden`
    /// because that set also hides the card's HAND copy (a pickup card lives in
    /// both places); the table copy must stay VISIBLE until its own flight, which
    /// only this set governs.
    @State private var sweptFlownIds: Set<String> = []
    /// The board's live collapse fraction (0 expanded, 1 compact), mirrored from
    /// `boardContent` so a flight builder - which runs OUTSIDE the geometry reader
    /// - can compute a hand card's final slot at the current crop (see
    /// `handLandingSlot`). Updated as the drawer height changes.
    @State private var currentCollapse: CGFloat = 0
    /// Round-7 ("buttons should NEVER float"): the hand's reserved height, MIRRORED
    /// out of `boardContent` into plain @State via `.onChange`. The action bar and
    /// self-role mark float a fixed gap above the hand, so their bottom padding is
    /// driven by THIS, not the `handHeight` local computed inside `boardContent`.
    ///
    /// Why the mirror: `handHeight` is computed inside the `.animation(cardMotion,
    /// value: controller.view)` scope, so when a pickup grows the hand past the
    /// two-row threshold and `handHeight` jumps one row -> two, that jump rode the
    /// board's card spring and the buttons FLOATED up with it. `.transaction { nil }`
    /// on the buttons did not stop it - a scoped `.animation(_:value:)` is not
    /// reliably overridden by a descendant transaction. An `.onChange` callback runs
    /// in its OWN transaction (no ambient animation), so a value the buttons read
    /// from here changes with a SNAP, never a spring - the buttons hold still and
    /// only ever jump instantly to their final spot. `-1` marks "not measured yet"
    /// so the first real height wins immediately (see `boardContent`'s onChange).
    @State private var buttonLift: CGFloat = -1
    @State private var deckFrame: CGRect = .zero
    @State private var handCardFrames: [String: CGRect] = [:]
    @State private var seatFrames: [Int: CGRect] = [:]
    // Displayed counts LAG the game state during a bout-end sequence: a badge/deck/
    // discard count holds its old value until that card's flight lands, then bumps
    // (so a hand count never jumps before the deck→player draw animation plays).
    @State private var seatCountOverride: [Int: Int] = [:]
    @State private var deckCountOverride: Int?
    @State private var discardCountOverride: Int?
    /// note 39: the board keeps its stage until whatever's animating (a
    /// bout-end sequence, or an open-delta replay) visibly finishes — the
    /// end screen only swaps in once this flips. Starts false; `.task` resets
    /// it defensively in case this view ever survives a controller swap
    /// (structurally it doesn't today — GameSurface always nils `controller`
    /// before assigning a new one — but nothing here should rely on that).
    @State private var showResults = false
    /// note 17: a cover that's about to empty the defender's hand ends the
    /// bout in the SAME kernel apply as the cover itself, so
    /// `flyBoutEndToDiscard` never sees an intermediate "covered" table to
    /// animate from — the card would just vanish straight to the discard
    /// pile. Stashed by `playAt` right before `controller.apply`, consumed
    /// (and always cleared) by the very next `flyBoutEndToDiscard` call.
    private struct PendingCover {
        let cards: [Card]
        let battleRect: CGRect
        let fromRects: [String: CGRect]   // card.identity -> hand rect AT PLAY TIME
    }
    @State private var pendingCover: PendingCover?
    /// Round-6 bug 13 ("when we drag a card then let go, it should animate from
    /// where we let go to the table, not back from its original position in hand
    /// to the table"): the cards a play of MINE just put on the table, and the
    /// rect each one LEFT FROM - the release point for the card the finger was
    /// actually holding, its resting hand slot for the rest of a multi-card
    /// selection (they never moved) and for a play made by tapping. Captured by
    /// `playAt` BEFORE `controller.apply`, since by the time the new view lands
    /// the hand has already closed over the gap. Consumed by the very next
    /// `flyBoutEndToDiscard`, exactly like `pendingCover` above.
    private struct PendingPlacement {
        let cards: [Card]
        let fromRects: [String: CGRect]   // card.identity -> where it left from
    }
    @State private var pendingPlacement: PendingPlacement?
    /// THE VEIL (round-4 notes 3 and 5). False until the board has handed its
    /// pending animation over to `animator`; while false the board renders the
    /// game as it was BEFORE that animation, not after.
    ///
    /// Why this exists at all: `.onChange(of: controller.view)` is where the
    /// board pre-hides what is about to fly and freezes the counts — and
    /// onChange runs AFTER body, i.e. one paint too late. So the board painted
    /// once at the post-move state and only then jumped back to the pre-move
    /// one to animate forward again. Every symptom of that is the same bug:
    /// a cover flashing tilted before it lands, a picked-up card appearing in
    /// the fan and then flying into the fan a second time ("double pickup
    /// animation"), and an opponent's badge counting up, back down and up again
    /// ("twitches as if it were making room for the deal card, but changed its
    /// mind"). Comments in `freezeCounts` and `replayLastMoveOnOpen` claimed
    /// their synchronous work landed before the first paint; structurally it
    /// cannot.
    ///
    /// @State initialized at CONSTRUCTION is the fix, because that is the one
    /// moment guaranteed to precede every paint. While it is false the board
    /// derives the veil PURELY from the controller (`veiledCardIds`,
    /// `veiledCounts`) — no mutation, so it is legal to do in `body` — and once
    /// `flyBoutEndToDiscard` has pre-hidden and frozen for real, this flips and
    /// `animator.hidden` + the count overrides take over unchanged. The handoff
    /// is invisible because both sides name the same cards.
    @State private var settled = false
    /// My hand as it was the instant I played a move, captured SYNCHRONOUSLY in
    /// `play` — before `controller.apply`, so before the view can publish. Any
    /// card in my hand that is not in here is one this move just gave me
    /// (a pickup's table cards, a bout-end refill), and it stays veiled until
    /// the animator hides it for its flight. nil when no move of mine is
    /// in flight. The live-play twin of the open-replay veil above.
    @State private var handBeforeMyMove: Set<String>?
    /// Round-6 bug 9 ("STILL seeing double animation for pickup"). Two animated
    /// sequences CAN overlap: an open-delta replay runs for as long as the move
    /// it replays takes, and nothing stops the human tapping Take (or Good) part
    /// way through it — that starts a live bout-end sequence on top of the replay
    /// still in flight. Traced (ANIMLOG): `stream#2 begin ... depth=1` while
    /// `stream#1 end` only lands LATER, in the middle of stream #2.
    ///
    /// The damage is the OLDER sequence's teardown, which unconditionally handed
    /// the veil and the counts back (`animator.clearPreHidden()` + clearing every
    /// count override). Run inside a newer sequence, that REVEALS the cards the
    /// newer one has pre-hidden but not yet flown: they pop into the fan, and the
    /// newer sequence then hides them again and flies them in — the picked-up
    /// card animating into the hand twice. Intermittent, because it only bites
    /// when the older sequence happens to end inside the newer one's window.
    ///
    /// So sequences are numbered and the NEWEST one wins: a superseded sequence
    /// stops issuing steps and, crucially, does not tear down shared state that
    /// no longer belongs to it. Bumped by `runEventStream` (and the genesis deal
    /// fallback) as each claims the animator.
    @State private var animSequenceToken = 0

    public init(controller: MessageTurnController, onSend: @escaping (Data) async -> Void,
                onNewGame: @escaping () -> Void = {}, onUnstage: @escaping () -> Void = {}) {
        self.controller = controller
        self.onSend = onSend
        self.onNewGame = onNewGame
        self.onUnstage = onUnstage
    }

    public var body: some View {
        VStack(spacing: 8) {
            if let view = controller.view {
                // Game over: the board gives way to the ranked results screen (web
                // WinScreen parity) - finish order first-out down to the fool, with
                // New game there. Gated on `showResults` too (note 39): the board
                // stays the stage until the last flight (a bout-end sequence, or an
                // open-delta replay of someone else's final move) has visibly
                // landed, so the end screen never just cuts in mid-animation.
                if controller.isOver && showResults {
                    FGameOverList(rows: finishRows(view), onNewGame: onNewGame)
                } else {
                    // The card-motion spring is scoped to the LIVE board only (note
                    // 18/40): it used to sit on this whole VStack, so the swap TO
                    // this branch's FGameOverList animated implicitly too — the
                    // WoodFill plank grew in under the spring, reading as the
                    // background "zooming" (worse with more rows; invisible in 2p,
                    // hence note 40). Attaching it here instead of on the root means
                    // in-board changes (deck count, hand, seat badges) still animate,
                    // but the board→results swap does not.
                    boardContent(view)
                        .animation(FMotion.cardMotion(reduceMotion: reduceMotion), value: controller.view)
                        // Round-4 note 2 ("the animation seems to replay when
                        // the screen collapses") is NOT fixable from in here,
                        // and it was worth finding that out before writing a
                        // plausible-looking modifier that does nothing.
                        // Stripping the ambient transaction on this board was
                        // tried and screenshotted mid-collapse: no change,
                        // because the reflow is not a SwiftUI animation of
                        // ours. The host resizes the hosting controller's view
                        // over the transition, and the board is simply laid out
                        // afresh at each intermediate size. It belongs to
                        // whoever owns the drawer — see ExtensionStage.
                }
            } else {
                ProgressView()
            }
        }
        .padding(.horizontal, 8).padding(.top, 14).padding(.bottom, 4)   // top margin so the ring isn't clipped in the compact drawer
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .overlay { FlyingCardsLayer(animator: animator) }
        .coordinateSpace(name: boardSpace)
        .onPreferenceChange(BattleFramesKey.self) { fr in
            battleFrames = fr
            if !fr.isEmpty { lastBattleFrames = fr }
        }
        .onPreferenceChange(BattleCardFramesKey.self) { fr in
            if !fr.isEmpty { lastBattleCardFrames = fr }
        }
        .onPreferenceChange(HandFrameKey.self) { handFrame = $0 }
        .onPreferenceChange(DiscardFrameKey.self) { discardFrame = $0 }
        .onPreferenceChange(DeckFrameKey.self) { deckFrame = $0 }
        .onPreferenceChange(SeatFramesKey.self) { seatFrames = $0 }
        .onPreferenceChange(HandCardFramesKey.self) {
            handCardFrames = $0
            #if DEBUG
            // slotRects self-check: at rest, the analytical landing slot must
            // match the MEASURED frame for every hand card, or a flight lands off
            // and snaps. Silent when they agree; logs the worst delta only when it
            // exceeds a couple of points, so a geometry drift shows up in the
            // device trace without spamming a correct build.
            if let hand = controller.view?.me?.hand, handFrame != .zero, !$0.isEmpty,
               animator.hidden.isEmpty, sweepBattles.isEmpty, !animator.isAnimating {
                let rects = FHandFan.slotRects(cards: hand, width: handFrame.width, crop: currentCollapse)
                var worst = 0.0, worstId = ""
                for c in hand {
                    guard let a = rects[c.identity]?.offsetBy(dx: handFrame.minX, dy: handFrame.minY),
                          let m = $0[c.identity] else { continue }
                    let d = max(abs(a.midX - m.midX), abs(a.midY - m.midY))
                    if d > worst { worst = d; worstId = c.identity }
                }
                if worst > 2 {
                    AnimLog.say("SLOTCHECK MISMATCH n=\(hand.count) worst=\(String(format: "%.1f", worst))pt @\(worstId) collapse=\(String(format: "%.2f", currentCollapse))")
                }
            }
            #endif
        }
        .onChange(of: controller.view) { flyBoutEndToDiscard(to: $0) }
        .fFlash($toast)
        .onChange(of: controller.rejectTick) { _ in
            // 1.0(4): say WHY, from the kernel's reason code, as a plain white
            // flash (no pill).
            Haptics.fire(.reject); toast = FStrings.rejectReason(controller.lastRejectReason)
            // A rejected move publishes NO view change, so the veil `play` put
            // up a moment ago would never be taken down again: the counts would
            // stay frozen at their pre-move values for the rest of the game and
            // any card the move would have added stay invisible. Nothing
            // happened, so give it all straight back.
            handBeforeMyMove = nil
            deckCountOverride = nil; discardCountOverride = nil; seatCountOverride = [:]
            // Round-6 bug 13: the same applies to a card `playAt` hid on its way
            // to the table. A rejected move publishes no view change, so nothing
            // will ever fly it and nothing else would take that veil down - the
            // card would simply be missing from the hand it never left.
            if let p = pendingPlacement {
                // Round-8: also drop the resting held ghost `playAt` spawned - the
                // move was rejected, so it will never fly; reveal the hand copy AND
                // clear the ghost, or a static ghost would sit at the source.
                animator.cancelHeld(Set(p.cards.map(\.identity)))
                animator.reveal(Set(p.cards.map(\.identity)))
                pendingPlacement = nil
            }
        }
        .task {
            AnimLog.say("board .task seat=\(controller.mySeat) ready=\(controller.ready)")
            // note 39: defensive reset — see `showResults`'s doc.
            showResults = false
            if !controller.ready { await controller.begin() }
            // Backstop for the veil: a controller that was ALREADY ready when
            // this board mounted publishes no view change, so the onChange that
            // normally lifts the veil never fires and the board would sit at
            // its pre-move state forever. Only safe when there is nothing to
            // replay — when there is, that onChange is the very thing driving
            // it, and lifting the veil here would be the flash we are avoiding.
            if controller.openReplayEvents.isEmpty { settled = true }
            // Genesis where I can't act (I dealt but I'm not the first attacker):
            // stage the deal immediately so I can send it on. When I CAN act,
            // canStage is false until I play, so this is a no-op then.
            await stageNow()
            #if DEBUG
            // FoolishHarness screenshotting only: auto-open the Settings / Help
            // sheet so it can be captured settled without a tap.
            if ProcessInfo.processInfo.environment["HARNESS_OPEN_SETTINGS"] != nil { showSettings = true }
            if ProcessInfo.processInfo.environment["HARNESS_OPEN_RULES"] != nil { showRules = true }
            // FoolishHarness screenshotting only: auto-play the first legal move so
            // the auto-stage flow (move -> staged bubble) is visible without a tap.
            // The auto-player must only make moves a HUMAN could make here.
            // `controller.legal` is the raw kernel menu, which always offers
            // GOOD — the owner's rule that an attacker cannot say good until
            // the table is fully covered is a UI gate (FActionBar's `canDone`,
            // via CardPlay.canSayGood), not a kernel one. Picking the first
            // non-wait move off the raw menu therefore said good over uncovered
            // attacks constantly, which no player can do, so an auto-run was
            // exercising a game nobody can play.
            if ProcessInfo.processInfo.environment["HARNESS_AUTOMOVE"] != nil,
               let view = controller.view,
               let m = CardPlay.humanMoves(battles: view.battles, legal: controller.legal).first {
                // Let the incoming replay (the OTHER player's last move flying
                // deck/seat→table on open) finish and rest so it's watchable,
                // THEN auto-play our move. Dev pacing only.
                let pace = Double(ProcessInfo.processInfo.environment["HARNESS_PACE"] ?? "") ?? 1
                try? await Task.sleep(nanoseconds: UInt64(2.4 * pace * 1_000_000_000))
                // Route through the SAME entry points a human tap hits (playAt /
                // playCover), not `play(m)` directly - so an auto-run exercises the
                // real placement path (preHide + the hand→table flight), which is
                // where the "ghost card / cards jump" bugs live. `play(m)` skips
                // all of that, so an auto-run of it can never reproduce them.
                switch m.type {
                case .attack, .pass: playAt(.table, m.cards, view)
                case .cover:         playCover(m.cards, view)
                case .pickup:        play(.pickup)
                case .good:          play(.good)
                default:             play(m)
                }
                // HARNESS_AUTOUNDO: after auto-playing, wait for the move to settle
                // (and the drawer to auto-collapse, if it does), then undo it - so a
                // run reproduces the "undo double animation / fade" the overlay-less
                // undo path shows, without a human tapping Undo.
                if ProcessInfo.processInfo.environment["HARNESS_AUTOUNDO"] != nil {
                    try? await Task.sleep(nanoseconds: UInt64(3.0 * pace * 1_000_000_000))
                    await controller.undo()
                }
            }
            #endif
        }
        // 1.0(4): the left Settings/Help squares present these.
        .sheet(isPresented: $showSettings) {
            MessageSettingsView { showSettings = false }
        }
        .sheet(isPresented: $showRules) {
            RulesView { showRules = false }
        }
    }

    // MARK: the veil (round-4 notes 3/5) — see `settled`

    /// The open-replay this board has NOT started yet, if any: the cards it will
    /// move and the counts the board should show until it does. A pure function
    /// of the controller, so `body` may read it on the very first paint — which
    /// is the whole point, that being the paint the onChange path misses.
    /// nil once `settled`, or when there is nothing to replay.
    private var pendingOpen: (ids: Set<String>, counts: (deck: Int, discard: Int, hand: [Int: Int]))? {
        guard !settled, let view = controller.view else { return nil }
        let events = controller.openReplayEvents
        guard !events.isEmpty else { return nil }
        return (controller.openReplayTouchedCardIds, Self.preCounts(events, finalView: view))
    }

    /// Every card the board must render as not-yet-there: what is in flight
    /// right now, plus what the veil is still holding back. THE set passed to
    /// the battle grid and the hand fan, so "is this card on the table" has one
    /// answer and the cover tilt can be read straight off it.
    private var veiledCardIds: Set<String> {
        var ids = animator.hidden
        if let open = pendingOpen { ids.formUnion(open.ids) }
        // Live play: cards this move just put in my hand, which have not been
        // pre-hidden yet (that also happens an onChange late).
        if let before = handBeforeMyMove, let hand = controller.view?.me?.hand {
            ids.formUnion(Set(hand.map(\.identity)).subtracting(before))
        }
        return ids
    }

    /// Round-6 bug 10: which veiled hand cards reserve NO fan width YET. A deal
    /// heading for my hand is veiled from the moment the whole sequence starts,
    /// but if it also RESERVED its slot from then, my present cards would slide
    /// left "in anticipation" while unrelated earlier steps (other seats' deals
    /// / pickups) play — the exact complaint. So everything veiled defers its
    /// slot EXCEPT the card whose flight is playing this instant: that one keeps
    /// its slot (it needs a real landing frame, and the fan opens for it as it
    /// lands). `animator.hidden \ animator.preHidden` is precisely "flying right
    /// now" (see `BoardAnimator.openSlots`); everything else waits its turn.
    private var handSlotDeferred: Set<String> {
        let flyingNow = animator.hidden.subtracting(animator.preHidden)
        return veiledCardIds.subtracting(flyingNow)
    }

    /// A seat badge's displayed hand count: the per-step override once a
    /// sequence is running, else the veil's pre-move value, else the truth.
    private func shownHandCount(_ p: PlayerView) -> Int {
        if let n = seatCountOverride[p.seat] { return n }
        if let n = pendingOpen?.counts.hand[p.seat] { return n }
        return p.handCount
    }
    private func shownDeckCount(_ view: GameView) -> Int {
        deckCountOverride ?? pendingOpen?.counts.deck ?? view.deckCount
    }
    private func shownDiscardCount(_ view: GameView) -> Int {
        discardCountOverride ?? pendingOpen?.counts.discard ?? view.discardCount
    }

    /// The live board, laid out like the web GameBoard: every piece is placed
    /// ABSOLUTELY against the board rect, so the centred pieces never shift when a
    /// corner changes (the bug where the deck pushed the opponent off-centre and
    /// clipped it). Deck pins top-left, discard top-right; opponents ring a 35%
    /// ellipse (the local player is the hand, so it is omitted); the battles sit
    /// dead-centre; my hand hugs the bottom. The first-attacker SWORD (not a "your
    /// move" label that ate layout) marks that I must open the bout.
    private func boardContent(_ view: GameView) -> some View {
        GeometryReader { geo in
            // Round-6 bugs 2/4: what USED to be a binary `geo.size.height < 340`
            // cliff — every compact-derived quantity flipping in one frame as the
            // drawer crossed that single height — is now a CONTINUOUS collapse
            // fraction (0 = fully expanded, 1 = fully compact drawer). The hand's
            // card crop, the role mark / action bar offsets, and the opponent ring
            // radius are all smooth functions of THIS, so the self hand descends
            // gradually and its companions track it as the drawer height changes
            // (whether the human drags the grabber — bug 4 — or the staged
            // auto-collapse animates the height down — bug 2). The old single-
            // instant flip is what "snapped" then; nothing about the RESTING
            // compact/expanded look changes, only the transit between them.
            let collapse = Self.collapseFraction(height: geo.size.height)
            let myHand = view.me?.hand ?? []
            // The width FHandFan itself actually lays out in: this reader's
            // width minus `hand(_:)`'s own `.padding(.horizontal, FSpace.s)`.
            let handWidth = max(0, geo.size.width - FSpace.s * 2)
            // Bug 10: the cards the fan actually LAYS OUT right now — the whole
            // hand minus any deal still deferring its slot (see
            // `handSlotDeferred`). Room is reserved off THIS, so an incoming
            // card's width is not reserved until its own flight opens the slot.
            let deferredSlots = handSlotDeferred
            // The hand's on-screen height at this collapse fraction (one
            // cropped/uncropped row, or two once M6 splits it). The self-role
            // indicator and action bar float a fixed gap ABOVE this, so driving
            // their offset off it is what makes them descend WITH the hand as it
            // crops down (bug 4) instead of hanging at a fixed spot the shrinking
            // hand pulls away from.
            //
            // Round-7 ("buttons should not move"): measured off the FULL `myHand`,
            // NOT `laidOutHand`. A deal/pickup adds cards that land one at a time
            // (each `laidOutHand`-visible only once its own flight opens its slot),
            // so anchoring off `laidOutHand` grew this height card-by-card and the
            // buttons visibly FLOATED UP as the cards arrived. `myHand` is already
            // the FINAL hand the instant the move applies, so the buttons sit at
            // their final spot from the start and the incoming cards fill UP toward
            // them - the hand makes room, the buttons hold still. Still a pure
            // function of crop + the final card count, so the compact-drawer
            // descent (bug 4) is unchanged.
            let handHeight = FHandFan.height(cards: myHand, availableWidth: handWidth, crop: collapse)
            // The buttons/role mark ride THIS, mirrored out via `.onChange` below so
            // their movement is a snap, never the board spring (see `buttonLift`).
            // Until the first mirror lands (-1) they read `handHeight` directly, so a
            // fresh board places them correctly on the very first paint.
            let lift = buttonLift < 0 ? handHeight : buttonLift
            ZStack {
                // Battles — dead centre of the board (web: absolute, both axes).
                battlesArea(view)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)

                // Opponent ring — each seat placed by trig on a 35% ellipse. The
                // local player is visual-index 0 (bottom edge) and is drawn as the
                // hand, so it is skipped here.
                ForEach(view.players.filter { $0.seat != controller.mySeat }) { p in
                    opponentSeat(p, view)
                        .position(ringPoint(seat: p.seat, n: view.players.count, in: geo.size))
                }

                // Deck top-left, discard top-right — pinned to the corners and OUT
                // of the centred flow, so they never push the ring or battles.
                FDeckWell(deckCount: shownDeckCount(view), flipped: view.flipped,
                          hasFlipped: view.hasFlipped, trumpSuit: view.trumpSuit)
                    // FDeckWell now anchors its own content top-leading with a
                    // small symmetric inset (note 14), so no per-call-site
                    // compensation offset is needed here anymore.
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                // Note 10: the discard pile shares the draw deck's y-baseline —
                // the centre of the deck's BOTTOM card (FDeckWell's fixed
                // anchor, see its type doc) must land on the centre of the
                // discard's own rotated cards. FDeckWell is pinned top-leading
                // with an `FSpace.s` (8pt) inset and its bottom card's rotated
                // footprint is 46pt tall, so that centre sits at 8 + 46/2 = 31pt
                // from the board's top edge. FDiscardPile is an (unrotated) 78×68
                // box pinned top-TRAILING with no inset of its own, so its own
                // centre — where its rotated cards actually converge — sits at
                // 68/2 = 34pt by default; -3 closes that gap. (This is UNRELATED
                // to `TableView.swift`'s own "+16" on FDeckWell's `.position()`:
                // that one compensates for centring FDeckWell's whole 92×108
                // frame in the OFFLINE app board, a different layout mechanism
                // than the corner-pinned `.frame(alignment:)` used here — neither
                // touches the other.)
                FDiscardPile(count: shownDiscardCount(view))
                    .offset(y: -3)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)

                // Self role indicator: the local seat never got a role mark before
                // (note 3) — only opponents (FSeatBadge) did. Same spot the old
                // first-attacker-only sword used: just above my hand.
                selfRoleIndicator(view)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                    .padding(.bottom, lift + 6)
                    // Round-7 ("buttons should NEVER move / float"): isolate this
                    // from the board's card spring. The ancestor animates on
                    // `.animation(cardMotion, value: controller.view)`; the correct
                    // override is a NESTED `.animation(nil, value: controller.view)`
                    // - same trigger value, innermost wins - NOT a `.transaction`
                    // (which does not reliably beat a scoped value-animation, the
                    // reason the earlier transaction fix let the role mark still
                    // drift) and NOT keying on `handHeight` (the wrong value - the
                    // change rides controller.view). Position now also snaps because
                    // it reads the mirrored `lift`, not the springy `handHeight`.
                    .animation(nil, value: controller.view)

                // Action buttons float bottom-right, above the hand (web absolute
                // bottom:90/right:20). They only appear when a flag enables them.
                actionBar(view)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                    .padding(.trailing, 4).padding(.bottom, lift + 4)
                    .animation(nil, value: controller.view)   // never float the buttons — see the role mark above

                // 1.0(4): Settings + Help squares, MIRRORING the action column on
                // the LEFT. Same 40pt height as the action pills, square, at the
                // same bottom line (lift + 4) and the SAME 16pt edge inset (4
                // outer + FSpace.m inner, exactly like actionBar's trailing). The
                // two squares + their gap span one action-button width
                // (40 + 16 + 40 = 96), so the left group is the mirror of the
                // right one. Faded out as the drawer collapses so they never
                // crowd the compact strip; the board spring never floats them.
                settingsHelpBar
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomLeading)
                    .padding(.leading, 4).padding(.bottom, lift + 4)
                    .opacity(Double(1 - min(1, collapse * 2)))
                    .allowsHitTesting(collapse < 0.4)
                    .animation(nil, value: controller.view)

                // My hand hugs the bottom (web: bottom max(10, safe-area)); the
                // outer .padding(12) is the safe-area inset that keeps it unclipped.
                hand(view, crop: collapse, reserveNoSlot: deferredSlots)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)

                // note 33 / round-4 note 4 / round-5 finding 5: the verb hint
                // used to ride above the FINGER on both axes, then above the
                // finger vertically and the CARD horizontally.
                //
                // Round-6 bug 5 ("action text preview on drag is broken,
                // sometimes appearing quite far away from the cards. Just keep
                // it right above the dragged card"): the Y axis was the half
                // that still tracked the fingertip, and a fingertip is only
                // where the card is if you happened to grab the card dead
                // centre. Grab one near its top edge, or drag a multi-card
                // selection, and the pill floated a card-height away from
                // everything it was describing. So BOTH axes now come off the
                // dragged card's own live centre and the pill rides a fixed
                // lift above it — see `dragHintPosition`, which is where the
                // fallbacks and the top-edge clamp live.
                //
                // `dragPoint`/`dragCardCenter` are in `boardSpace` (FHandFan
                // publishes them there, and the drop targets are hit-tested in
                // it), while `.position` is local to this GeometryReader - so
                // subtract this reader's own origin in that space rather than
                // assuming the two agree.
                if let p = dragPoint {
                    let resting = dragCard.flatMap { handCardFrames[$0.identity] }
                        .map { CGPoint(x: $0.midX, y: $0.midY) }
                    let at = Self.dragHintPosition(cardCentre: dragCardCenter, restingCentre: resting,
                                                   finger: p,
                                                   origin: geo.frame(in: .named(boardSpace)).origin)
                    dragHint(view).position(x: at.x, y: at.y)
                }
            }
            // Mirror the live collapse fraction out to a flight builder, which runs
            // outside this reader and needs it to compute a hand card's final slot.
            .onChange(of: collapse) { currentCollapse = $0 }
            // Mirror the hand height out to `buttonLift` so the buttons SNAP to it
            // (this callback runs in its own no-animation transaction) instead of
            // floating on the board spring. Logged so a device trace shows exactly
            // when and why the reserved height moves - the "button floats up" report.
            .onChange(of: handHeight) {
                AnimLog.say("handHeight \(Int(buttonLift))->\(Int($0)) cards=\(myHand.count) rows=\(FHandFan.rowCount(cards: myHand, availableWidth: handWidth)) collapse=\(String(format: "%.2f", collapse))")
                buttonLift = $0
            }
            .onAppear { currentCollapse = collapse; buttonLift = handHeight }
        }
    }

    /// How far above the dragged CARD'S CENTRE the verb hint floats (round-6
    /// bug 5 re-anchored it from the fingertip to the card; the number itself is
    /// unchanged). A hand card is 72pt tall, so 52 leaves the pill ~16pt clear
    /// of the card's top edge: reads as attached to the card, still well out
    /// from under the thumb holding it.
    static let dragHintLift: CGFloat = 52
    /// The highest the pill may sit in the board's own coordinates. Dragging a
    /// card up to the deck/discard corners would otherwise push it off the top
    /// edge (a 52pt lift off a card whose centre is 40pt down is -12), and a
    /// verb you cannot read is the same bug in a different direction. Half the
    /// pill's own height (13pt text + 2x FSpace.xs) plus a couple of points of
    /// margin, since `.position` places its CENTRE. When the pill cannot fit
    /// above the card it goes BELOW it rather than parking on this line - see
    /// `dragHintPosition`.
    static let dragHintMinY: CGFloat = 16

    /// Where the verb-hint pill sits, in the board GeometryReader's own local
    /// coordinates (round-6 bug 5). Pure + static so the anchoring can be
    /// asserted directly instead of eyeballed mid-drag on a device.
    ///
    /// `cardCentre` is the dragged card's live visual centre in `boardSpace`
    /// (FHandFan.onDragCardMoved). `restingCentre` is that card's hand slot,
    /// used for the ONE frame at the start of a drag where the board has a drag
    /// point but no reported card centre yet - which is the frame the card has
    /// not moved in anyway, so the slot IS the card. `finger` is the last
    /// resort, and only reachable if the dragged card has no published frame at
    /// all. A multi-card selection anchors on the card actually being dragged:
    /// the rest of the selection has not moved out of the fan, so that is the
    /// only card the pill could sensibly point at.
    ///
    /// Drag a card up against the board's top edge (the deck / discard corners)
    /// and there is no room for the pill above it. Clamping it to the ceiling
    /// then parks it ON TOP OF the card, hiding the very card it describes
    /// (screenshotted on the simulator mid-fix), so it FLIPS below the card
    /// instead: still touching the card, still fully on screen, and never
    /// covering it, since the flip distance is the same lift that already
    /// clears half a card.
    static func dragHintPosition(cardCentre: CGPoint?, restingCentre: CGPoint?,
                                 finger: CGPoint, origin: CGPoint) -> CGPoint {
        let anchor = cardCentre ?? restingCentre ?? finger
        let y = anchor.y - origin.y
        return CGPoint(x: anchor.x - origin.x,
                       y: y - dragHintLift >= dragHintMinY ? y - dragHintLift : y + dragHintLift)
    }

    /// The board's CONTINUOUS collapse fraction from its own height: 0 at/above
    /// `expandedAnchor` (the resting expanded board), 1 at/below `compactAnchor`
    /// (the resting compact drawer), linearly ramped between. Every compact-
    /// derived quantity in `boardContent`/`ringPoint` reads THIS instead of the
    /// old `height < 340` cliff, so each is a smooth function of the drawer
    /// height — the fix for round-6 bugs 2 and 4 (a manual grabber-drag OR the
    /// staged auto-collapse now TWEENS the layout down as the height animates
    /// instead of every quantity flipping in one frame at a single height). The
    /// anchors are chosen so BOTH resting states saturate: `compactAnchor` is the
    /// old 340 cliff, which that binary test proved the compact drawer always
    /// sits below (so it -> 1 at rest), and the near-full-screen expanded board
    /// is always well above 440 (so it -> 0 at rest). Only the transit between
    /// the two is now a ramp; neither resting look changes.
    static func collapseFraction(height: CGFloat) -> CGFloat {
        let compactAnchor: CGFloat = 340
        let expandedAnchor: CGFloat = 440
        if height <= compactAnchor { return 1 }
        if height >= expandedAnchor { return 0 }
        return (expandedAnchor - height) / (expandedAnchor - compactAnchor)
    }

    /// The finish order for the end screen: rank 1 = first player out (best),
    /// counting up to the fool last. `eliminationOrder` is first-out first and
    /// holds everyone except the fool; the fool is `view.gameOver` (the one seat
    /// still holding cards), given the last place. Mirrors web WinScreen.
    private func finishRows(_ view: GameView) -> [FinishRow] {
        let total = view.players.count
        var rows: [FinishRow] = []
        for (i, seat) in view.eliminationOrder.enumerated() {
            rows.append(FinishRow(place: i + 1, total: total, name: name(seat),
                                  isYou: seat == controller.mySeat))
        }
        if view.gameOver >= 0 {
            rows.append(FinishRow(place: total, total: total, name: name(view.gameOver),
                                  isYou: view.gameOver == controller.mySeat))
        }
        return rows
    }

    // MARK: zones

    private func name(_ seat: Int) -> String { controller.names[seat] ?? "Seat \(seat + 1)" }

    /// Does this seat wear the sword? An attacker keeps it until THEY say good,
    /// even once every attack on the table is covered — not "any uncovered
    /// battle exists", which erased every sword the instant the table filled up.
    ///
    /// But on an EMPTY table only ONE seat can act at all: the seat that opens
    /// the bout. Marking every non-defender then was just wrong — "when no cards
    /// are on the table, and only the first attacker can move, ONLY the first
    /// attacker gets the sword. Everyone else has no icon." Once the bout is
    /// open, throw-ins make the others attackers for real, and they get one.
    private func showsSword(seat: Int, isOut: Bool, _ view: GameView) -> Bool {
        guard seat != view.defender, !isOut, !view.hasSaidGood(seat) else { return false }
        return view.battles.isEmpty ? seat == view.firstAttacker : true
    }

    /// One opponent seat badge, publishing its frame in `boardSpace` so bout-end
    /// flights can target it. Placed on the ring by `ringPoint`.
    private func opponentSeat(_ p: PlayerView, _ view: GameView) -> some View {
        FSeatBadge(name: name(p.seat),
                   handCount: shownHandCount(p),
                   isDefender: p.seat == view.defender,
                   isAttacker: showsSword(seat: p.seat, isOut: p.isOut, view),
                   saidGood: view.hasSaidGood(p.seat),
                   isOut: p.isOut)
            .background(GeometryReader { g in
                Color.clear.preference(key: SeatFramesKey.self,
                                       value: [p.seat: g.frame(in: .named(boardSpace))])
            })
    }

    /// A seat's centre on the web's 35% ellipse (PlayerRing): the local player is
    /// visual-index 0 → bottom centre (drawn as the hand); opponents fan clockwise.
    /// Percentages resolve against width vs height, which reads as an oval on a
    /// non-square board - exactly the web's trick.
    private func ringPoint(seat: Int, n: Int, in size: CGSize) -> CGPoint {
        let visual = (seat - controller.mySeat + n) % n
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

    /// My own role mark — shield/check/sword — mirroring FSeatBadge's roleRow for
    /// opponents (note 3: the local player never saw their own role before, only
    /// the special-cased first-attacker sword). Nothing shows once I'm out (the
    /// game-over screen replaces the whole board, so "game over" is already
    /// handled by the caller never reaching here then).
    private func selfRoleIndicator(_ view: GameView) -> some View {
        let mySeat = controller.mySeat
        let isOut = view.me?.isOut ?? false
        let isDefender = view.defender == mySeat
        let saidGood = view.hasSaidGood(mySeat)
        let isAttacker = showsSword(seat: mySeat, isOut: isOut, view)
        return Group {
            if !isOut {
                // Round-5 m4 ("make sword and shield larger and darker") —
                // sized up at this call site specifically (check 19->22,
                // shield 22->26, sword 19->23); FSeatBadge's opponent-facing
                // copies are another agent's file and were dictated the same
                // target sizes separately.
                HStack(spacing: FSpace.xs) {
                    if saidGood { FCheck(size: 22) }
                    if isDefender { FShield(size: 26) }
                    // Larger than the shield, not equal to it (owner, on device:
                    // "make the sword icon larger"). FSword draws on a 24-grid
                    // and then rotates 45°, so its blade only spans ~70% of the
                    // box it is given — a sword and a shield at the SAME nominal
                    // size do not read the same size on screen.
                    else if isAttacker { FSword(size: 32) }
                }
            }
        }
    }

    private func battlesArea(_ view: GameView) -> some View {
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
        let sweeping = view.battles.isEmpty && !sweepBattles.isEmpty
        let shown = sweeping ? sweepBattles : view.battles
        // note 34: a pass preview shows the ghost slot instead of a cover highlight.
        // Never while sweeping (the cards are leaving, not a drop target).
        let passPreview = sweeping ? false : isPassPreview(view)
        return Group {
            if !shown.isEmpty {
                FBattleGrid(battles: shown, trumpSuit: view.trumpSuit,
                            coverable: sweeping || passPreview ? [] : highlightBattles(view),
                            onTapBattle: sweeping ? { _ in } : { idx in tapBattle(idx, view) },
                            namespace: cardNS,
                            // Sweeping cards are hidden per-flight (`sweptFlownIds`),
                            // NOT by the hand veil (`veiledCardIds`, which also hides
                            // a picked-up card's HAND copy) - the table copy must
                            // stay up until its own flight lifts it.
                            hidden: sweeping ? sweptFlownIds : veiledCardIds,
                            showGhostSlot: sweeping ? false : passPreview,
                            // Round-7 #7: the covers whose flight is playing this
                            // instant, so the attack beneath one tilts WITH it (same
                            // set that drives `handSlotDeferred`'s "flying now").
                            flyingNow: sweeping ? [] : animator.hidden.subtracting(animator.preHidden))
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

    /// Round-7 #3 ("rearranging while in the compact view keeps giving 'move not
    /// allowed'"): the hand's DROP/cancel hit-region, grown generously upward (and
    /// a little down) from the published hand frame. In the compact drawer the fan
    /// is cropped to a ~44pt strip at the very bottom, so a horizontal rearrange
    /// whose finger drifts up off that thin strip fell OUTSIDE `handFrame` - and a
    /// release there resolves to `.table`, i.e. an attack/pass, which `CardPlay`
    /// rejects with the "move not allowed" toast. Battles are hit-tested FIRST in
    /// `BoardDrop.target`, so widening the hand band never swallows a real cover;
    /// and a genuine open-table attack is dropped well above this band (the centre
    /// of the board), so it still resolves to `.table`. A rearrange that never
    /// reached a battle now cancels quietly instead of rejecting.
    private var handDropFrame: CGRect {
        guard handFrame != .zero else { return handFrame }
        let up: CGFloat = 64, down: CGFloat = 24
        return CGRect(x: handFrame.minX, y: handFrame.minY - up,
                      width: handFrame.width, height: handFrame.height + up + down)
    }

    /// The move a release right now would resolve to, if any — the SAME
    /// `BoardDrop.target` + `CardPlay.resolve` math `onDragEnded` uses, shared
    /// by the verb hint (note 33) and the pass ghost-slot preview (note 34) so
    /// neither can disagree with what actually happens on release.
    private func dragPreview(_ view: GameView) -> (target: PlayTarget, move: Move)? {
        guard let card = dragCard, let point = dragPoint else { return nil }
        let target = BoardDrop.target(at: point, battles: battleFrames, handFrame: handDropFrame)
        guard target != .hand,
              let move = CardPlay.resolve(cards: playCards(for: card, view), target: target,
                                          isDefender: view.defender == controller.mySeat,
                                          battles: view.battles, legal: controller.legal)
        else { return nil }
        return (target, move)
    }

    /// note 34: is the live drag currently previewing a PASS onto open table
    /// space? Gates both the ghost slot and the suppression of the ordinary
    /// per-battle cover highlight while it's showing.
    private func isPassPreview(_ view: GameView) -> Bool {
        guard let preview = dragPreview(view) else { return false }
        return preview.target == .table && preview.move.type == .pass
    }

    /// note 33: what a release would do, localized — "Attack" / "Cover" /
    /// "Pass". Nothing over the hand (that's a reorder, not a play) or for a
    /// drop `CardPlay` can't resolve into a legal move.
    private func dragHintText(_ view: GameView) -> String? {
        guard let move = dragPreview(view)?.move else { return nil }
        switch move.type {
        case .attack: return FStrings.t("attack")
        case .cover: return FStrings.t("cover")
        case .pass: return FStrings.t("pass")
        default: return nil
        }
    }

    /// note 33: a small unobtrusive pill naming what release would do — web
    /// DragShadow parity. Round-4 note 4: it now tracks the fingertip (see
    /// boardContent), which is what "anchored to a fixed spot above the
    /// battles" was traded against and lost — the fixed anchor was easy to
    /// place but sat at the top of the screen while your hand was at the
    /// bottom, so it read as unrelated to the drag.
    @ViewBuilder
    private func dragHint(_ view: GameView) -> some View {
        if let text = dragHintText(view) {
            Text(text)
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(FColor.card)
                .padding(.horizontal, FSpace.m)
                .padding(.vertical, FSpace.xs)
                .background(FColor.ink.opacity(0.85))
                .clipShape(RoundedRectangle(cornerRadius: FRadius.chip))
                .shadow(color: .black.opacity(0.35), radius: 6, y: 2)
                .transition(.opacity)
                .allowsHitTesting(false)
        }
    }

    /// The cards a play would use right now: the whole selection if the dragged (or
    /// tapped) card is part of it, else just that one card (web selected-or-single).
    private func playCards(for card: Card, _ view: GameView) -> [Card] {
        selection.contains(card.identity) ? selectedCards(view) : [card]
    }

    /// Battles to highlight — what the in-flight drag (or the current selection)
    /// could cover.
    private func highlightBattles(_ view: GameView) -> Set<Int> {
        let cards = dragCard.map { playCards(for: $0, view) } ?? selectedCards(view)
        return CardPlay.coverableBattles(cards: cards, battles: view.battles, legal: controller.legal)
    }

    /// A bout closed (the table cleared): animate its end - the discard or pickup
    /// sweep, then every refill - from the KERNEL's evwire for that move, the SAME
    /// runEventStream the open-replay uses. No GameView diff decides what flies any
    /// more (the old takerSeat / beaten / myNewCards / lastBoutDraws reconstruction
    /// is gone): the kernel's events are the only source, live and on open alike.
    /// The one thing still read off old/new is the TRIGGER (did battles go
    /// non-empty -> empty) and which of MY cards to hide before they fly - both UI
    /// timing, not animation derivation.
    private func flyBoutEndToDiscard(to newView: GameView?) {
        let prior = lastView
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
        guard let new = newView else { return }
        // Once this returns, `animator.hidden` and the count overrides are the
        // whole truth — so this is exactly where the board stops veiling.
        defer { settled = true }
        // …and any path that returns WITHOUT starting a sequence has to hand the
        // counts back, because `play` now freezes them before every move, not
        // just the ones that end a bout. Leaving them frozen after a plain
        // attack would pin every badge at its pre-move value for good.
        func releaseCounts() {
            deckCountOverride = nil; discardCountOverride = nil; seatCountOverride = [:]
        }
        if reduceMotion {
            releaseCounts(); clearSweep()
            if new.isOver { showResults = true }   // note 39b
            return
        }
        // note 10: undo can legally take battles -> empty; never a bout end.
        // Round-8: an undo is the EXACT REVERSE of the play it undoes - the card
        // flies FROM the table back to its hand slot while the present cards slide
        // apart to make room, never fading on the table and teleporting into the
        // hand. `flyUndoReturn` runs that reverse flight and returns true when it
        // owns the animation; a non-return undo (nothing came back to MY hand -
        // e.g. undoing a pickup, or someone else's move) falls through to the snap.
        if controller.lastChangeWasUndo {
            releaseCounts()
            if let old = prior, flyUndoReturn(old: old, new: new) { return }
            clearSweep(); return
        }
        // First appear with a delivered game: the open-replay (same event path).
        if prior == nil { AnimLog.say("-> openReplay"); replayLastMoveOnOpen(new); return }
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
            return
        }

        // note 17: a cover that ended the bout in the SAME apply still needs its
        // landing flown first - the kernel jumped straight to a cleared table, so
        // there is no rendered intermediate state for the discard sweep to carry it
        // from. Its battle rect must have been part of the table we just cleared.
        let boutFrames = lastBattleFrames
        var matchedCover: PendingCover?
        if let pc = cover, boutFrames.values.contains(pc.battleRect) { matchedCover = pc }

        // Pre-hide the cards about to land in MY hand so they fly from the deck
        // rather than popping in. This is the ONE view diff that remains, and only
        // to choose which of my cards to hide before the first paint - the events
        // that DECIDE the animation need an async kernel call we cannot make
        // synchronously here. Everything that actually flies is the kernel's.
        let oldHandIds = Set((old.me?.hand ?? []).map(\.identity))
        let myNewIds = Set((new.me?.hand ?? []).map(\.identity)).subtracting(oldHandIds)
        if !myNewIds.isEmpty { animator.preHide(myNewIds) }
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
            if let pc = matchedCover {
                BoardAnimator.sequenceDepth += 1
                await playStep { _ in self.pendingCoverLandingFlights(pc) }
                BoardAnimator.sequenceDepth -= 1
            }
            let events = await MessageKernel.shared.lastMoveEvents(viewer: controller.mySeat)
            await runEventStream(events, finalView: new)
        }
    }

    /// Animate an ordered evwire stream (the kernel's events for ONE move) as
    /// sequential flights, freezing each displayed count to that step's OWN board
    /// (GameEvent.state) as its flight lands - so a count never jumps ahead of its
    /// animation, and there is no backward count arithmetic to keep in sync. THE
    /// one animation path, shared by the open-replay and the live bout-end, so
    /// neither derives what-flies-where from a GameView diff. The caller pre-hides
    /// the moved cards first (synchronously, before the first paint).
    private func runEventStream(_ events: [GameEvent], finalView view: GameView, openReplay: Bool = false) async {
        let run = AnimLog.on ? AnimLog.nextRun() : 0
        AnimLog.say("stream#\(run) begin n=\(events.count) [\(events.map { "\($0.kind.map(String.init(describing:)) ?? "?")@\($0.seat)x\($0.cards.count)" }.joined(separator: " "))] depth=\(BoardAnimator.sequenceDepth)")
        guard !events.isEmpty else {
            if view.isOver { settleResults() }
            return
        }
        // Bug 9: claim the animator. Anything already running is now stale.
        animSequenceToken += 1
        let mySeq = animSequenceToken
        BoardAnimator.sequenceDepth += 1
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
        defer {
            BoardAnimator.sequenceDepth -= 1
            // ONLY the newest sequence may hand the veil and the counts back. A
            // superseded one doing it here is the double pickup (see
            // `animSequenceToken`): it un-hides cards the sequence that replaced
            // it has pre-hidden but not yet flown, so they appear in the fan and
            // are then flown into it a second time.
            if mySeq == animSequenceToken {
                deckCountOverride = nil; discardCountOverride = nil; seatCountOverride = [:]
                animator.clearPreHidden()
                // The swept table has finished flying, so take down the pre-bout
                // grid (its cards now live in a hand / the discard / a badge). Both
                // paths - a live bout-end and an open-replay - lay it out now.
                sweepBattles = []; sweepTableIds = []; sweptFlownIds = []
                let stuck = openedThisSeq.filter { animator.isHidden($0) }
                if !stuck.isEmpty {
                    AnimLog.say("stream#\(run) rescue-reveal \(stuck.count) opened-but-unflown [\(stuck.sorted().joined(separator: ","))]")
                    animator.reveal(stuck)
                }
            } else {
                AnimLog.say("stream#\(run) superseded by seq \(animSequenceToken) - teardown skipped")
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

        for ev in events {
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
            // Round-7: the DECK count drops as the cards LEAVE the deck (they start
            // flying NOW), not when they land - a full deck with cards visibly
            // flying out of it reads wrong. Deck only, and only for deck-sourced
            // draws; the discard pile and seat badges still tick up when THEIR cards
            // arrive (the per-step advance after the flight, below).
            if let s = ev.state, ev.kind == .deal || ev.kind == .refill {
                deckCountOverride = s.deckCount
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
            await playStep { lastChance in
                let f = self.openReplayFlights(ev, view: view, lastChance: lastChance)
                if let f { AnimLog.say("stream#\(run) step \(ev.kind.map(String.init(describing:)) ?? "?")@\(ev.seat) flights=\(f.count) [\(f.map(\.id).joined(separator: ","))]") }
                return f
            }
            if let s = ev.state {
                deckCountOverride = s.deckCount
                discardCountOverride = s.discardCount
                for p in s.players where p.seat != controller.mySeat { seatCountOverride[p.seat] = p.handCount }
            }
        }
        AnimLog.say("stream#\(run) end")
        // Bug 9: a stale sequence must not settle the results screen either —
        // the sequence that replaced it owns when the board gives way.
        if view.isOver, mySeq == animSequenceToken { settleResults() }
    }

    /// Bug 10: the REAL card identities THIS event lands in MY hand (a deal /
    /// refill / pickup to my own seat). Empty for everything else — an opponent's
    /// draw (masked backs to a badge), a table placement, a discard sweep. These
    /// are exactly the cards whose fan slot `openSlots` cuts as this step begins,
    /// so the fan opens for them then instead of at the whole sequence's start.
    private func myHandLandingIds(_ ev: GameEvent) -> Set<String> {
        guard ev.seat == controller.mySeat else { return [] }
        switch ev.kind {
        case .deal, .refill, .pickup: return Set(ev.cards.compactMap { $0?.identity })
        default: return []
        }
    }

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
    private func freezeCounts(to v: GameView) {
        deckCountOverride = v.deckCount
        discardCountOverride = v.discardCount
        var counts: [Int: Int] = [:]
        for p in v.players where p.seat != controller.mySeat { counts[p.seat] = p.handCount }
        seatCountOverride = counts
    }

    /// note 39: hold the board on-screen a beat after whatever's animating
    /// (a bout-end sequence, or an open-delta replay) has visibly finished,
    /// THEN swap to the results screen. The only place `showResults` is ever
    /// set true. A short guard against re-scheduling once it's already flipped.
    private func settleResults() {
        guard !showResults else { return }
        Task {
            try? await Task.sleep(nanoseconds: 500_000_000)
            withAnimation(.easeOut) { showResults = true }
        }
    }

    /// Poll (up to ~1.2s) for a step's frames to be ready, then play it and await
    /// the animation. `build` returns nil (frames not ready — retry), [] (nothing to
    /// animate), or the flights.
    private func playStep(_ build: (_ lastChance: Bool) -> [Flight]?) async {
        for i in 0..<26 {
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

    // MARK: bout-end flight builders (each returns nil until its frames are ready)

    /// note 17: the cover-that-ends-the-bout's own landing step — its cards
    /// fly from their hand rects AT PLAY TIME (snapshotted before `apply`,
    /// since the hand has already moved on by the time this plays) to the
    /// battle they covered. A pure snapshot, so no retry: what was measured
    /// is what there is.
    private func pendingCoverLandingFlights(_ pc: PendingCover) -> [Flight]? {
        pc.cards.enumerated().compactMap { i, c in
            pc.fromRects[c.identity].map {
                Flight(id: "coverland-\(c.identity)", card: c, from: $0,
                      to: pc.battleRect.offsetBy(dx: CGFloat(i) * 8, dy: CGFloat(i) * 6))
            }
        }
    }

    /// Round-6 bug 13: fly a play of MINE onto the table - from the release point
    /// (or the hand slot, for a tap) to the slot it landed in. The cards are
    /// already hidden (`playAt` pre-hid them before the apply), so what the
    /// viewer sees is one continuous movement out of the fingertip.
    ///
    /// Wrapped in `sequenceDepth` like every other animated sequence, which is
    /// what makes the extension's auto-collapse (note 8's `waitForSettle`) hold
    /// off until the card has actually landed instead of yanking the drawer down
    /// mid-flight.
    private func flyPlacement(_ pp: PendingPlacement, to view: GameView) {
        let ids = Set(pp.cards.map(\.identity))
        Task {
            BoardAnimator.sequenceDepth += 1
            defer {
                BoardAnimator.sequenceDepth -= 1
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

    /// Round-8: the EXACT REVERSE of `flyPlacement`. Undoing a play flies each
    /// card that came back to MY hand FROM the table slot it sat in TO its hand
    /// slot, while the present cards slide apart to make room (openSlots, animated
    /// over the flight) - mirroring the play's gap-close. The table copy is SWEPT
    /// (kept rendered, then snapped hidden the instant its own flight lifts it) so
    /// it never fades out on the table and the card never teleports into the hand.
    /// Returns false when nothing came back to my hand (a non-return undo - undoing
    /// a pickup, or someone else's move), so the caller falls back to a plain snap.
    private func flyUndoReturn(old: GameView, new: GameView) -> Bool {
        let oldHand = Set((old.me?.hand ?? []).map(\.identity))
        let returned = (new.me?.hand ?? []).filter { !oldHand.contains($0.identity) }
        guard !returned.isEmpty, handFrame != .zero else { return false }
        let ids = Set(returned.map(\.identity))
        AnimLog.say("-> undoReturn [\(ids.sorted().joined(separator: ","))]")
        // Veil the returning cards in the hand AND defer their fan slots - the hand
        // opens for each only as its flight arrives (the mirror of the play's veil).
        animator.preHide(ids)
        // Keep the pre-undo table rendered so each card flies FROM where it sat,
        // rather than the table copy fading out (its FBattleGrid removal) as the
        // view empties. The sweep hides each copy as its own flight lifts it.
        setSweep(old.battles)
        animSequenceToken += 1
        let mySeq = animSequenceToken
        Task {
            BoardAnimator.sequenceDepth += 1
            defer {
                BoardAnimator.sequenceDepth -= 1
                if mySeq == animSequenceToken {
                    animator.clearPreHidden()
                    sweepBattles = []; sweepTableIds = []; sweptFlownIds = []
                    let stuck = ids.filter { animator.isHidden($0) }
                    if !stuck.isEmpty { animator.reveal(stuck) }
                }
            }
            // A beat for the swept table and the (deferred) hand to publish frames.
            try? await Task.sleep(nanoseconds: 16_000_000)
            // Make room for the returning cards (present cards slide apart),
            // animated over the flight - the reverse of the play's gap-close.
            withAnimation(.timingCurve(0.25, 0.46, 0.45, 0.94, duration: flightTime)) {
                self.animator.openSlots(ids)
            }
            // Lift the table copies: snap them hidden (no fade) as the flight starts.
            self.sweptFlownIds.formUnion(ids)
            await playStep { lastChance in
                let laid = self.laidOutHandNow(new)
                var flights: [Flight] = []
                for c in returned {
                    guard let from = self.lastBattleCardFrames[c.identity]
                            ?? self.lastBattleFrames.values.first else { continue }
                    guard let to = self.handLandingSlot(c, laidOut: laid)
                            ?? (lastChance ? self.handApproxLanding() : nil) else { return nil }
                    flights.append(Flight(id: "undo-\(c.identity)", card: c, from: from, to: to))
                }
                return flights.isEmpty ? (lastChance ? [] : nil) : flights
            }
        }
        return true
    }

    /// The flights for one placement: each card from where it left to the battle
    /// slot it landed in, in the FINAL view. nil (retry) while a landing slot's
    /// rect has not published yet - an attack or a pass CREATES its slot, so the
    /// frame only exists a paint or two after the apply. A card that is not on
    /// the table at all any more is skipped rather than retried (best-effort,
    /// like `openReplayFlights`), so a lost card can never wedge the poll.
    private func placementFlights(_ pp: PendingPlacement, view: GameView) -> [Flight]? {
        var out: [Flight] = []
        for c in pp.cards {
            guard let idx = view.battles.firstIndex(where: { $0.attack == c || $0.defense == c })
            else { continue }
            guard let rect = battleFrames[idx] else { return nil }
            let from = pp.fromRects[c.identity] ?? handCardFrames[c.identity] ?? rect
            // Round-6 bug 1 (batch 3) parity: a card landing as the DEFENSE lies
            // across, so its ghost rotates INTO that tilt over the flight rather
            // than arriving flat and snapping tilted when the ghost is removed.
            let landedAngle = view.battles[idx].defense == c ? FBattleGrid.coverAngle : 0
            out.append(Flight(id: "place-\(c.identity)", card: c, from: from, to: rect,
                              angle: landedAngle))
        }
        return out
    }

    private func myDrawFlights(_ cards: [Card], laidOut: [Card], lastChance: Bool = false) -> [Flight]? {
        if cards.isEmpty { return [] }
        guard deckFrame != .zero, handFrame != .zero else { return nil }
        return cards.enumerated().compactMap { i, c in
            (handLandingSlot(c, laidOut: laidOut) ?? handCardFrames[c.identity]
                ?? handApproxLanding(index: i, of: cards.count))
                .map { Flight(id: "draw-\(c.identity)", card: c, from: deckFrame, to: $0) }
        }
    }

    /// Round-7 #1: a rough card-sized landing rect for a draw whose exact per-card
    /// slot frame never published in time. SPREAD across the hand fan by index, so
    /// several unresolved cards fly to separate places rather than piling onto one
    /// point and then snapping apart (the first-open "bunch then ungroup"). A
    /// deck->hand flight to about-the-right-place reads far better than the card
    /// silently appearing; nil only if the hand itself hasn't rendered yet.
    private func handApproxLanding(index: Int = 0, of count: Int = 1) -> CGRect? {
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
    private func handLandingSlot(_ card: Card, laidOut: [Card]) -> CGRect? {
        guard handFrame != .zero else { return nil }
        let rects = FHandFan.slotRects(cards: laidOut, width: handFrame.width, crop: currentCollapse)
        guard let local = rects[card.identity] else { return nil }
        return local.offsetBy(dx: handFrame.minX, dy: handFrame.minY)
    }

    /// The hand cards the fan actually lays out at this instant: the whole hand
    /// minus any deal still deferring its slot (the same rule `boardContent` uses
    /// for `laidOutHand`, recomputed here for the flight builders). The incoming
    /// card whose flight is playing now is NOT deferred (its slot is open), so it
    /// is included - which is why `handLandingSlot` can find it.
    private func laidOutHandNow(_ view: GameView) -> [Card] {
        let deferred = handSlotDeferred
        return (view.me?.hand ?? []).filter { !deferred.contains($0.identity) }
    }


    /// Deck/discard/every-seat's hand count as of BEFORE the open-replay's
    /// `events`, walked backward from the FINAL view's counts by undoing each
    /// event's card movement. This freezes the on-screen counts to their pre-move
    /// values before the sequence starts; every step then jumps forward to its
    /// OWN board (GameEvent.state) as its flight lands, so this only sets the
    /// starting frame, not the per-step values. Card COUNTS are all we need here
    /// (real identities the events already carry): a deal/refill takes n out of
    /// the deck into the acting seat; a discard adds to the pile; a pickup takes
    /// the table into a hand; an attack/cover/pass puts a card onto the table.
    private static func preCounts(_ events: [GameEvent], finalView: GameView)
        -> (deck: Int, discard: Int, hand: [Int: Int]) {
        var deck = finalView.deckCount, discard = finalView.discardCount
        var hand = Dictionary(uniqueKeysWithValues: finalView.players.map { ($0.seat, $0.handCount) })
        for ev in events.reversed() {
            let n = ev.cards.count
            switch ev.kind {
            case .deal, .refill: deck += n; if let s = ev.actorSeat { hand[s, default: 0] -= n }
            case .discard, .cardsToTrash: discard -= n
            case .pickup: if let s = ev.actorSeat { hand[s, default: 0] -= n }
            case .attackPass, .cover, .defenderMove: if let s = ev.actorSeat { hand[s, default: 0] += n }
            default: break
            }
        }
        return (deck, discard, hand)
    }

    /// note 4: an approximate source rect for a pickup/discard flight replayed
    /// on open — the pre-bout table itself is never rendered (the game is
    /// already past it by the time we open), so there is no real per-battle
    /// rect to fly from. Prefers the last REAL battle rects seen this session
    /// (rare on a fresh open); otherwise a small rect at the board's visual
    /// centre, between the deck/discard corners and the hand, so the flight
    /// still reads as "from the table" rather than from nowhere. nil only
    /// when even the hand hasn't rendered yet (poll again via playStep).
    private func approximateTableCenter() -> CGRect? {
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
        let half = 35.0                      // half of FBattleGrid's 70pt card height
        let r = tilt * .pi / 180
        let rect = slot.offsetBy(dx: CGFloat(sin(r) * half),
                                 dy: slot.height / 2 - CGFloat(half) + CGFloat((1 - cos(r)) * half))
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
    private func tableCardSource(_ card: Card, fallbackIndex i: Int) -> CGRect? {
        if let rect = lastBattleCardFrames[card.identity] { return rect }
        if let src = discardSource(for: card) { return src.rect }
        guard let center = approximateTableCenter() else { return nil }
        return center.offsetBy(dx: CGFloat(i) * 6, dy: CGFloat(i) * 4)
    }

    /// Round-7 (replay bunch): are the on-table SOURCE slots for these swept
    /// cards measured yet? On an open-replay the pre-bout table is laid out
    /// invisibly (`replayPreBattles`) a paint after the stream starts, so a
    /// bout-end flight built on the very first poll would read the centre
    /// fallback and bunch. Gate the build on this (except on the final poll,
    /// `lastChance`, where a rough centre still beats a card that never flies).
    /// Cards NOT in `replayTableIds` (the live board, or an opponent whose cards
    /// aren't reconstructed) never block - they were never going to publish here.
    private func tableSourceReady(_ cards: [Card]) -> Bool {
        cards.allSatisfy { !sweepTableIds.contains($0.identity) || lastBattleCardFrames[$0.identity] != nil }
    }

    /// Lay out `battles` as the pre-bout table a bout-end sequence sweeps - the
    /// cards sit VISIBLE on the table (via `battlesArea`) until each flies. One
    /// setter for both the live bout-end (prior view's battles) and the open-replay
    /// (reconstructed). Resets `sweptFlownIds` so nothing is pre-hidden.
    private func setSweep(_ battles: [BattleView]) {
        sweepBattles = battles
        sweptFlownIds = []
        sweepTableIds = Set(battles.flatMap { b -> [String] in
            [b.attack.identity] + (b.defense.map { [$0.identity] } ?? [])
        })
    }

    /// Drop the pre-bout table. Called on any view change that empties the table
    /// WITHOUT running a bout-end sequence (reduce-motion, an undo), so a sweep
    /// captured synchronously by `play` can never linger as phantom cards on a
    /// table that isn't actually mid-animation. A no-op when nothing is swept.
    private func clearSweep() {
        guard !sweepBattles.isEmpty else { return }
        sweepBattles = []; sweepTableIds = []; sweptFlownIds = []
    }

    /// One open-replay event's flights, straight from the KERNEL's evwire stream
    /// (a GameEvent). The event ALREADY carries viewer-correct cards - my own
    /// draws/pickups as real identities, opponents' as nil (a back) - so unlike
    /// the old diff path there is no reconstructed "my cards" argument, and no
    /// case where my own cards silently go missing (the round-2 #9 bug). Returns
    /// nil to ask `playStep` to retry (a needed frame isn't published yet), or a
    /// (possibly empty) flight list. `view` is the FINAL board, for locating a
    /// card still on the table.
    private func openReplayFlights(_ ev: GameEvent, view: GameView, lastChance: Bool = false) -> [Flight]? {
        let mine = ev.seat == controller.mySeat
        switch ev.kind {
        case .attackPass, .defenderMove, .cover:
            // A card placed on the table (hand -> its battle). Best-effort, no
            // retry: a card already swept onward to a discard/pickup later in
            // this same open is simply skipped (that event flies it).
            let source = mine ? handFrame : (seatFrames[ev.seat] ?? .zero)
            var out: [Flight] = []
            for case let card? in ev.cards {
                guard let idx = view.battles.firstIndex(where: { $0.attack == card || $0.defense == card }),
                      let rect = battleFrames[idx] else { continue }
                let from = source != .zero ? source : rect.offsetBy(dx: 0, dy: -220)
                // Bug 1: a card that lands as the DEFENSE (cover) lies across at
                // +coverAngle, so its ghost rotates into that tilt as it flies. An
                // attack lands upright (0); its own later tilt, once ITS cover
                // lands, is the battle grid's job, not this flight's.
                let landedAngle = view.battles[idx].defense == card ? FBattleGrid.coverAngle : 0
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
                    (handLandingSlot(c, laidOut: laid) ?? handCardFrames[c.identity]
                        ?? handApproxLanding(index: i, of: cards.count)).map {
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
                    return (handLandingSlot(c, laidOut: laid) ?? handCardFrames[c.identity]
                        ?? handApproxLanding(index: i, of: cards.count)).map {
                        Flight(id: "openpick-\(c.identity)", card: c, from: from, to: $0) } }
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
                return Flight(id: "openpick-\(ev.seat)-\(c.identity)", card: c, from: from,
                              to: badge.offsetBy(dx: CGFloat(i) * 3, dy: 0)) }

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
                if let rect = lastBattleCardFrames[c.identity] {
                    return Flight(id: "opendiscard-\(c.identity)", card: c, from: rect, to: discardFrame)
                }
                if let src = discardSource(for: c) {
                    return Flight(id: "opendiscard-\(c.identity)", card: c, from: src.rect,
                                  to: discardFrame, angle: 0, fromAngle: src.tilt)
                }
                let center = approximateTableCenter() ?? discardFrame
                return Flight(id: "opendiscard-\(c.identity)", card: c,
                              from: center.offsetBy(dx: CGFloat(i) * 6, dy: CGFloat(i) * 4),
                              to: discardFrame)
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
    private func replayLastMoveOnOpen(_ view: GameView) {
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
                // Bug 9: this deal is a sequence like any other — claim the
                // animator so a live move started on top of it supersedes it,
                // and so ITS teardown can never clear a newer sequence's veil.
                animSequenceToken += 1
                let mySeq = animSequenceToken
                Task {
                    BoardAnimator.sequenceDepth += 1
                    defer {
                        BoardAnimator.sequenceDepth -= 1
                        if mySeq == animSequenceToken {
                            animator.clearPreHidden()
                            // Same rescue as runEventStream's teardown: openSlots
                            // pulled the opening hand OUT of preHidden, so
                            // clearPreHidden can't reveal it if myDrawFlights
                            // never built (frames not ready). Force it visible so
                            // a genesis deal can never end as invisible cards.
                            let stuck = ids.filter { animator.isHidden($0) }
                            if !stuck.isEmpty {
                                AnimLog.say("genesis rescue-reveal \(stuck.count) opened-but-unflown")
                                animator.reveal(stuck)
                            }
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

        // …and the counts, likewise taking over from the veil's own
        // `pendingOpen.counts`, which is this same walk-back (`preCounts`) done
        // in `body` because there is no prior view on an open - this board IS
        // the first paint.
        let pre = Self.preCounts(events, finalView: view)
        deckCountOverride = pre.deck
        discardCountOverride = pre.discard
        var counts: [Int: Int] = [:]
        for (seat, c) in pre.hand where seat != controller.mySeat { counts[seat] = c }
        seatCountOverride = counts

        // The pre-bout table this open sweeps (a pickup or discard). Rendered
        // VISIBLE by `battlesArea` so the cards sit on the table and then fly off
        // it - see `sweepBattles`. Set BEFORE the stream starts so the grid lays
        // out on the next paint, in time for the flight to measure and fly from it.
        // Empty for a plain attack/cover replay (those cards are still on the table
        // in `view`).
        setSweep(controller.openReplayPreBattles)

        // The SAME animator the live bout-end uses - one path, the kernel's events.
        // `openReplay: true` opens the fan for a COLD first open so each drawn card
        // flies to its correct slot instead of bunching.
        Task { await runEventStream(events, finalView: view, openReplay: true) }
    }

    /// notes 33/34: FHandFan already delivers the live boardSpace point on
    /// every change — kept now (previously discarded at the call site) so the
    /// verb hint / ghost-slot preview can resolve the same drop target live.
    private func onDragChanged(_ card: Card, at point: CGPoint) {
        dragCard = card
        dragPoint = point
    }

    private func onDragEnded(_ card: Card, at point: CGPoint, _ view: GameView) {
        // Round-6 bug 13: WHERE the card was when the finger let go, captured
        // BEFORE the drag state is torn down two lines down. That teardown used
        // to happen first and `playAt` ran with no memory of the drag at all, so
        // the card returned to its hand slot and the play animated out of the
        // HAND - "it should animate from where we let go to the table, not back
        // from its original position in hand". The card's own centre, not the
        // fingertip, for the same reason the verb hint uses it (bug 5): the
        // finger is wherever you grabbed the card, which is not where the card
        // is. Falls back to the finger only if no card centre was ever reported.
        let releaseCentre = dragCardCenter ?? point
        dragCard = nil
        dragPoint = nil
        dragCardCenter = nil
        // Round-7 #3: `handDropFrame`, not the raw published `handFrame` - a
        // compact-drawer rearrange that drifts off the thin cropped strip still
        // reads as a reorder/cancel, not a rejected attack. See `handDropFrame`.
        let target = BoardDrop.target(at: point, battles: battleFrames, handFrame: handDropFrame)
        // Dropped back in the fan — cancel. Nothing to fly: FHandFan has already
        // sprung the card home to its slot, which is the right animation for a
        // drag that played nothing.
        if target == .hand { return }
        playAt(target, playCards(for: card, view), view, released: (card, releaseCentre))
    }

    /// The Settings + Help squares, mirroring `actionBar` on the LEFT (1.0(4)).
    /// Now the shared `SettingsHelpSquares` pair — the New-game setup and the
    /// lobby float the same component at the same corner (durak-rules-redesign),
    /// so the three screens cannot drift apart. Persistent (unlike the move
    /// buttons) — Settings and Help are always available while playing.
    private var settingsHelpBar: some View {
        SettingsHelpSquares(onSettings: { showSettings = true },
                            onHelp: { showRules = true })
    }

    private func actionBar(_ view: GameView) -> some View {
        let cards = selectedCards(view)
        let defending = view.defender == controller.mySeat
        // Play buttons only while I can act and have NOT staged; once staged, the
        // only control is Undo (the extension has dropped the user at Messages' Send).
        let acting = controller.iCanAct && !controller.canSend
        return FActionBar(
            canAttack: acting && !defending && CardPlay.canAttack(cards, legal: controller.legal),
            canCover: acting && defending && CardPlay.canCover(cards, battles: view.battles, legal: controller.legal),
            canPass: acting && defending && CardPlay.canPass(cards, legal: controller.legal),
            // Selection-aware: with cards selected, the defender's Take and the
            // attacker's Good must disappear — a stray tap on either while mid-
            // selection would abandon the cards you'd picked (web parity TODO).
            //
            // Take is available to the defender when there are cards on the table
            // (web's own condition: `rawPickup = isDefending && table_battles > 0`,
            // NOT the kernel's legal menu, which stops LISTING pickup once every
            // attack is covered even though the kernel still ACCEPTS it). Plus the
            // no-selection rule so a stray tap can't abandon a picked selection.
            //
            // Round-7 UPDATE (owner, on device): ALSO gated on `!canSend`, so the
            // instant a defender move is staged the bar collapses to just Undo -
            // exactly like every acting-gated button (attack/cover/pass/good, all
            // `acting = iCanAct && !canSend`). This REVERSES the earlier "Take
            // survives the staged/all-covered state" rule: leaving Take up while
            // Undo appeared BELOW it shoved the bottom-anchored column upward, so
            // the Take pill visibly rode up as Undo popped in (the "ghostly Pickup
            // floating above Undo"). The owner chose the clean swap over keeping
            // Take through a staged cover: to take your own covered table now, Undo
            // first, then Take. (The kernel still accepts the move, so no reject.)
            canPickup: defending && !view.battles.isEmpty && cards.isEmpty
                && !(view.me?.isOut ?? false) && !controller.canSend,
            canDone: acting && CardPlay.canSayGood(battles: view.battles, legal: controller.legal) && cards.isEmpty,
            canUndo: controller.canSend,
            onAttack: { playAt(.table, cards, view) },
            onCover: { playCover(cards, view) },
            onPass: { playAt(.table, cards, view) },
            onPickup: { play(.pickup) },
            onDone: { play(.good) },
            onUndo: { Task {
                await controller.undo()
                // Undo that still has staged moves re-stages the shorter chain
                // (replaces the input bubble). Undo that empties `pending` must
                // CANCEL the staged move: Apple offers no API to remove an
                // inserted bubble, so on a continuation we overwrite it with the
                // base (received) state — the undone move can then no longer be
                // sent. A genesis with no move left is not sealable, so there we
                // can only retract our own bookkeeping (`onUnstage`).
                if controller.canStage { await stageNow() }
                else if controller.isContinuation { await stageBaseNow() }
                else { onUnstage() }
            } }
        )
    }

    /// - `crop` (round-5 M5b, made continuous in round-6): how much of each hand
    ///   card to hide off the bottom — 0 the whole card (expanded), 1 the top
    ///   half (fully compact drawer), any value between as the drawer collapses.
    ///   See `boardContent`'s `collapse`.
    private func hand(_ view: GameView, crop: CGFloat, reserveNoSlot: Set<String>) -> some View {
        FHandFan(cards: view.me?.hand ?? [], trumpSuit: view.trumpSuit,
                 selection: $selection, onTap: { toggle($0) },
                 onDragChanged: { card, point in onDragChanged(card, at: point) },
                 onDragEnded: { card, point in onDragEnded(card, at: point, view) },
                 namespace: cardNS, hidden: veiledCardIds,
                 crop: crop,
                 onDragCardMoved: { center in dragCardCenter = center },
                 reserveNoSlot: reserveNoSlot, instantExit: true)
            .padding(.horizontal, FSpace.s)
    }

    // MARK: interaction (mirrors TableView — every branch reads the kernel menu)

    private func play(_ move: Move) {
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
        Task { await controller.apply(move); await stageNow() }
    }

    /// Compose + stage the bubble the instant a move is applied (§11.4 flow, B4
    /// feedback: "too many buttons before you can send"). Playing an attack /
    /// cover / pickup / pass / good drops you straight to the staged message, so
    /// the only remaining action is the send arrow. Re-staging replaces the input
    /// bubble, so throwing in more cards just updates it. No-op until something is
    /// staged (a 0-action genesis body is unsealable).
    private func stageNow() async {
        guard controller.canStage else { return }
        if let payload = try? await controller.stagedPayload() { await onSend(payload) }
    }

    /// Undo-to-empty on a continuation (1.0(4)): re-seal the base (received) state
    /// and stage it, REPLACING the stale move bubble the host still holds. This is
    /// the closest to "cancel the staged move" the Messages API allows — there is
    /// no call to remove an inserted bubble, so the move is overwritten with a
    /// bubble that carries nothing new (sending it just re-shares the same board).
    private func stageBaseNow() async {
        guard controller.isContinuation else { return }
        if let payload = try? await controller.stagedPayload() { await onSend(payload) }
    }


    // Dumb selection; CardPlay resolves (selection, target) into one legal move,
    // exactly like the app's TableView (both read the kernel menu, never a rule).

    private func toggle(_ card: Card) {
        if selection.contains(card.identity) { selection.remove(card.identity) }
        else { selection.insert(card.identity) }
    }

    private func selectedCards(_ view: GameView) -> [Card] {
        (view.me?.hand ?? []).filter { selection.contains($0.identity) }
    }

    /// Tap an uncovered attack while cards are selected → cover it (two-tap cover).
    private func tapBattle(_ index: Int, _ view: GameView) {
        playAt(.battle(index), selectedCards(view), view)
    }

    /// `released` (round-6 bug 13) is the card the finger was holding and the
    /// board-space centre it was let go at, when this play came from a drag; nil
    /// for a tap/button play, which starts from the hand slot as it always has.
    private func playAt(_ target: PlayTarget, _ cards: [Card], _ view: GameView,
                        released: (card: Card, centre: CGPoint)? = nil) {
        guard let move = CardPlay.resolve(cards: cards, target: target,
                                          isDefender: view.defender == controller.mySeat,
                                          battles: view.battles, legal: controller.legal) else {
            Haptics.fire(.reject); toast = FStrings.t("ios.reject"); return
        }
        // Bug 13: where each card of this play leaves from. ONE answer, used by
        // both landing animations below - the ordinary placement flight and note
        // 17's cover-that-ended-the-bout - so a dragged card cannot start from
        // the release point in one and from the hand in the other.
        let fromRects = Self.playSourceRects(cards: cards, handRects: handCardFrames,
                                             released: released.map { ($0.card.identity, $0.centre) })
        // note 17: a cover might end the bout in the SAME kernel apply as the
        // cover itself (the defender's hand empties) — stash enough, BEFORE
        // applying, for flyBoutEndToDiscard to synthesize the landing step it
        // would otherwise have no rendered state to animate from.
        if move.type == .cover, case .battle(let i) = target, i >= 0, i < view.battles.count,
           let rect = battleFrames[i] ?? lastBattleFrames[i] {
            pendingCover = PendingCover(cards: cards, battleRect: rect, fromRects: fromRects)
        } else {
            pendingCover = nil
        }
        // Bug 13: hand the placement to `flyBoutEndToDiscard`, and hide the cards
        // NOW - synchronously, before `apply` can publish a view with them
        // already sitting on the table. This is the same veil trick as
        // `handBeforeMyMove` and for the same reason (an onChange fires a paint
        // too late): without it the card would paint landed, vanish, and only
        // then fly. `animator.preHide` is the one hiding mechanism the board
        // already has, so the handoff to the flight's own `hidden` set is
        // seamless, and any path that ends up not flying them reveals them again.
        pendingPlacement = PendingPlacement(cards: cards, fromRects: fromRects)
        animator.preHide(Set(cards.map(\.identity)))
        // Round-8 (atomic takeoff): the same instant the hand copy is veiled above,
        // put a resting ghost where each card WAS, so the swap is seamless - no
        // frame where the card is neither in the hand nor in the overlay. The real
        // `place-<id>` flight (`placementFlights` -> `animator.play`) reuses these
        // ids and simply starts moving them once the kernel publishes the table
        // slot. `fromRects` is the card's own hand slot (or drag-release point).
        animator.showHeld(cards.compactMap { c in
            fromRects[c.identity].map { Flight(id: "place-\(c.identity)", card: c, from: $0, to: $0) }
        })
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

    /// Cover button: cover the first uncovered attack the selection can beat.
    private func playCover(_ cards: [Card], _ view: GameView) {
        guard let i = CardPlay.coverableBattles(cards: cards, battles: view.battles,
                                                legal: controller.legal).sorted().first else {
            Haptics.fire(.reject); return
        }
        playAt(.battle(i), cards, view)
    }

    private func coverableBattles(_ view: GameView) -> Set<Int> {
        let out = CardPlay.coverableBattles(cards: selectedCards(view), battles: view.battles, legal: controller.legal)
        return out
    }
}

/// The first-attacker sword — a hand-built UPRIGHT sword on a 24x24 grid that
/// actually reads as a sword: a pointed blade, a wide crossguard, a grip, and a
/// round pommel, all filled FLAT dark gray. Marks "you open this bout".
struct FSword: View {
    @Environment(\.colorScheme) private var scheme
    var size: CGFloat = 24
    var body: some View {
        Canvas { [scheme] ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            func R(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> CGRect {
                CGRect(x: x * s, y: y * s, width: w * s, height: h * s)
            }
            // Round-5 m4 ("make sword and shield larger and darker"): darkened
            // from 0x3A3A3A so the glyph reads at a glance on the wool instead
            // of blending into it at the sizes it's actually drawn (m4's own
            // complaint was "no legibility... at the size they are drawn").
            //
            // Dark mode inverts that reasoning rather than repeating it: m4's
            // near-black is legible BECAUSE the light weave is bright, and on
            // the walnut weave it is the one glyph on the board that vanishes
            // completely (the shield's mid-gray fill and the check's saturated
            // green both still carry). Steel, not black, in dark mode - and it
            // stays a FLAT fill either way, so the sword still reads as one
            // solid silhouette and not as a shaded object.
            let gray = scheme == .dark ? Color(hex: 0xD3D6DC) : Color(hex: 0x26262A)

            // Blade: a pointed spike from the tip (top) down to the guard.
            var blade = Path()
            blade.move(to: P(12, 1.5))     // tip
            blade.addLine(to: P(13.5, 5.5))
            blade.addLine(to: P(13.5, 14.5))
            blade.addLine(to: P(10.5, 14.5))
            blade.addLine(to: P(10.5, 5.5))
            blade.closeSubpath()
            ctx.fill(blade, with: .color(gray))

            // Crossguard: a wide bar under the blade.
            ctx.fill(Path(roundedRect: R(6, 14.3, 12, 2.2), cornerRadius: 0.7 * s), with: .color(gray))
            // Grip: the handle below the guard.
            ctx.fill(Path(R(10.9, 16.4, 2.2, 4.6)), with: .color(gray))
            // Pommel: a round knob at the base.
            let r = 1.7 * s
            ctx.fill(Path(ellipseIn: CGRect(x: 12 * s - r, y: 21.2 * s - r, width: 2 * r, height: 2 * r)),
                     with: .color(gray))
        }
        .frame(width: size, height: size)
        .rotationEffect(.degrees(45))   // point it up-and-to-the-right
        // Round-5 m2: this was a hard-coded English literal while every visible
        // string on the board goes through FStrings — a ru/ko VoiceOver user
        // got an English board here even though the label was otherwise well
        // chosen. The key already exists (en/ru/ko all carry it).
        .accessibilityLabel(Text(FStrings.t("ios.a11y.attackfirst")))
    }
}

/// The defender shield — a hand-built heraldic shield, filled flat gray with a
/// darker edge for definition on the wool. Marks the current defender.
///
/// Round-5 m4 ("make sword and shield larger and darker. Make the shield have
/// like pointed upper corners to make it more obvious"): the old flat-top shape
/// read as a plain gray plaque at 20pt with no legend (m4's own complaint).
/// The top is now two points reaching UP with a shallow dip between them — a
/// heraldic silhouette, not a rounded rectangle — and both fill and edge are
/// darkened a step from before. The fill stays a distinctly LIGHTER gray than
/// the sword's near-black (so the two glyphs don't collapse into "two dark
/// blobs" at a glance); the edge alone goes almost as dark as the sword's fill,
/// which is what actually reads as "darker" against the wool at 20pt.
struct FShield: View {
    var size: CGFloat = 24
    var body: some View {
        Canvas { ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            let gray = Color(hex: 0x878E96), edge = Color(hex: 0x2E3338)
            var shield = Path()
            // Round-7: the heater / crusader shield of the owner's reference - a
            // raised POINT at the top centre, rounded shoulders as the widest
            // span, curving to a point at the bottom. (Was round-5 m4's inverse:
            // raised corners with a dipped centre.) The peak sits at y=1.5, the
            // shoulders at y=5.5 span the full width, and the sides sweep to the
            // bottom point at (12,22.5).
            shield.move(to: P(12, 1.5))                                   // top-centre peak
            shield.addQuadCurve(to: P(21, 5.5),  control: P(15.5, 5))     // peak -> right shoulder
            shield.addQuadCurve(to: P(12, 22.5), control: P(21, 15.5))    // right side -> bottom point
            shield.addQuadCurve(to: P(3, 5.5),   control: P(3, 15.5))     // bottom point -> left shoulder
            shield.addQuadCurve(to: P(12, 1.5),  control: P(8.5, 5))      // left shoulder -> peak
            shield.closeSubpath()
            // The two top edges are CONCAVE to the shield (owner's nudge): the
            // control points sit BELOW the straight peak->shoulder line (y=5 vs
            // the line's ~3.5 midpoint), so the edge bows inward/down toward the
            // centre rather than bulging out - the crusader-shield sweep up to
            // the point, not a balloon.
            ctx.fill(shield, with: .color(gray))
            ctx.stroke(shield, with: .color(edge), style: StrokeStyle(lineWidth: 1.1 * s, lineJoin: .round))
        }
        .frame(width: size, height: size)
        // Round-5 m2: was a hard-coded English literal (see FSword's).
        .accessibilityLabel(Text(FStrings.t("ios.a11y.defending")))
    }
}

/// The "said good" mark — a hand-built green check stroke on the same 24x24 grid
/// as FSword/FShield. Hand-built for the same reason: SF Symbols (previously
/// `checkmark.seal.fill`) are unreliable under ImageRenderer bubble snapshots.
struct FCheck: View {
    var size: CGFloat = 24
    var body: some View {
        Canvas { ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            let green = Color(hex: 0x2E9E4F)
            var check = Path()
            check.move(to: P(4.5, 12.5))
            check.addLine(to: P(9.5, 18))
            check.addLine(to: P(20, 5.5))
            ctx.stroke(check, with: .color(green),
                       style: StrokeStyle(lineWidth: 3 * s, lineCap: .round, lineJoin: .round))
        }
        .frame(width: size, height: size)
        // Round-5 m2: was a hard-coded English literal (see FSword's). "good"
        // (no `ios.` prefix) is the same key the action bar's Good button uses.
        .accessibilityLabel(Text(FStrings.t("good")))
    }
}

/// One row of the ranked end screen (web WinScreen parity, minus ELO). `place` is
/// 1-based: rank 1 is the first player out (best); the fool is `place == total`.
struct FinishRow: Identifiable {
    let place: Int
    let total: Int
    let name: String
    let isYou: Bool
    var id: Int { place }
    var isFool: Bool { place == total }
}

/// The game-over results, replacing the board when the fool is decided (design:
/// mirror the web WinScreen). Players are listed in finishing order - first out
/// (rank 1) down to the fool - with New game at the bottom so any player, out or
/// not, can deal the next one. No ELO in the iMessage game, and no emoji: the rank
/// is a colored number (brass for 1st, red for the fool) with a "Fool" tag.
struct FGameOverList: View {
    private static let rowH: CGFloat = 34   // fixed per-row height (plank scales with player count)
    let rows: [FinishRow]
    let onNewGame: () -> Void

    private var plankHeight: CGFloat { CGFloat(rows.count) * Self.rowH }

    var body: some View {
        // Title + ranking sit at the TOP; New game is pinned to the bottom.
        VStack(spacing: 14) {
            // Round-6 #17: this title sits on the plain wool (it is ABOVE the
            // wood plank, not on it), so it takes the wool half of the
            // text-on-a-surface pairing - thick black ink, not the bone/dark-
            // shadow combo it used to carry (that combo was tuned for wood).
            Text(FStrings.t("game_over"))
                .font(.title2)
                .onWoolText()
            // ONE continuous wood plank behind the whole ranking (no dividers):
            // WoodFill is a ZStack LAYER (not a .background, which over-drew and
            // made the plank too tall), hard-clipped to exactly rows × rowH so it
            // is a single block that scales with the player count.
            ZStack {
                WoodFill()
                VStack(spacing: 0) {
                    ForEach(rows) { row in
                        HStack(spacing: 12) {
                            // The last place reads "Fool" in the rank column itself
                            // (no separate pill); everyone else is "#N".
                            //
                            // EVERY rank is plain white, including the two that
                            // used to be tinted (brass for 1st, red for the
                            // fool). Those tints were an attempt to make the two
                            // rows a player actually cares about stand out, and
                            // on device they did the opposite — mid-value colours
                            // on bright orange wood are the two LEAST legible
                            // things on the screen (the review's own M10 reading).
                            // Owner's call after seeing it: "just make it white
                            // for #1 and Fool. If it's thick enough text, it
                            // looks fine on the wood background." Round-6 #17
                            // promoted that exact treatment to `onWoodText`
                            // (Tokens.swift) — used here for both columns, since
                            // the name column was ALSO plain text on the same
                            // wood plank and used to fight the rank column with
                            // dark instead of light text.
                            Text(row.isFool ? FStrings.t("ios.fool") : "#\(row.place)")
                                .font(.headline).monospacedDigit()
                                .onWoodText()
                                .frame(width: 56, alignment: .leading)
                            Text(row.name + (row.isYou ? " (\(FStrings.t("ios.you")))" : ""))
                                .font(.body)
                                .onWoodText()
                                .lineLimit(1)
                            Spacer(minLength: 0)
                        }
                        .frame(height: Self.rowH)
                        .padding(.horizontal, 12)
                    }
                }
            }
            // Round-5 B2: WoodFill is an aspect-FILL image, so height-only sizing
            // let it propose a width that grew right along with the plank's
            // height — more players -> taller plank -> WIDER plank, overflowing
            // the surface and clipping the rank column first, then the names
            // (worst case: 8 players, the plank goes blank but for a stray `)`).
            // Pinning the width alongside the height is the fix the finding names
            // directly: the plank is now exactly the surface width at every
            // count 2...8, and WoodFill fills THAT box instead of dictating it.
            //
            // Spelled as min/maxHeight rather than `height:` because SwiftUI has
            // no `frame(maxWidth:height:)` overload — the fixed-size and the
            // flexible-size frames are two different modifiers, and mixing one
            // argument from each does not compile.
            .frame(maxWidth: .infinity, minHeight: plankHeight, maxHeight: plankHeight)
            .clipShape(Rectangle())
            .overlay(Rectangle().strokeBorder(.black.opacity(0.4), lineWidth: 1.5))
            // NO extra horizontal inset here (owner, on device: "ranking list
            // should be same width as the new game button"). The plank used to
            // carry .padding(.horizontal, 4) that the button below does not, so
            // the two wooden blocks on this screen were 4pt out of alignment on
            // each side — enough to read as a mistake once both are full width.
            // Both now rely on the VStack's outer .padding() alone.
            Spacer(minLength: 0)
            FButton(FStrings.t("ios.msg.newgame"), kind: .wood, action: onNewGame)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}
