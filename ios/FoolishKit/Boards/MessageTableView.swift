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
    /// Re-render this view when a setting changes (see FPrefs). Only the
    /// OBSERVATION matters - the strings still come from FStrings.t and the
    /// table surface still comes from FTextures.
    @ObservedObject private var prefs = FPrefs.shared
    /// Seal the staged chain and hand it to the extension to compose + insert.
    /// The view never touches MSMessage; it only produces the payload. The `Bool`
    /// is `fromUndo`: a fresh move drops the player at Messages' Send (the drawer
    /// collapses), but an UNDO re-stages only to refresh the input bubble and must
    /// KEEP the expanded board up - undoing means "let me pick a different move",
    /// so collapsing the screen out from under them is wrong.
    private let onSend: (Data, _ fromUndo: Bool) async -> Void
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
    /// Round-9: the SURFACE staged a sendable bubble for this game outside the
    /// controller's own pending list - the one board case is the starter's LIVE
    /// handoff (startGame stages the deal; the new controller has nothing
    /// pending, but the bubble still needs Messages' Send). Feeds the send
    /// reminder alongside `controller.canSend`.
    private let alsoStaged: Bool
    /// Round 12: hold the gear for 5 seconds to raise the last-message dump.
    /// The dump's fields live on the SURFACE (it owns the payload bytes and the
    /// decode result), so the board only reports the gesture; see
    /// `MessagesRootView.diagnosticPanel`.
    private let onDiagnostics: () -> Void

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
    /// Round-11: there is no live crop any more, and therefore no mirrored
    /// collapse fraction. The hand's geometry is the SAME at every drawer
    /// height (see `boardContent`'s `handCrop`), so a flight builder running
    /// outside the geometry reader can just ask for the one layout.
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
    /// Round-12: this now READS the controller (`replayPending`) instead of
    /// being board `@State` initialized at construction. Same contract, one
    /// less reason to tear the board down.
    ///
    /// The old version was right that construction is the one moment guaranteed
    /// to precede every paint - which is precisely why an arriving bubble had to
    /// build a WHOLE NEW BOARD to be veiled in time, and that rebuild is the
    /// "still flashes if move comes in during expanded screen" the owner sees.
    /// Published controller state is up before the first paint of the new chain
    /// as well, and costs no teardown; see `MessageTurnController.replayPending`.
    /// While it is false the board derives the veil PURELY from the controller
    /// (`veiledCardIds`, `veiledCounts`) — no mutation, so it is legal to do in
    /// `body` — and once `flyBoutEndToDiscard` has pre-hidden and frozen for
    /// real, this flips and `animator.hidden` + the count overrides take over
    /// unchanged. The handoff is invisible because both sides name the same cards.
    private var settled: Bool { !controller.replayPending }
    /// The last `controller.arrivalTick` this board has reacted to. When it
    /// falls behind, the next view change is a bubble that ARRIVED rather than a
    /// move made here, and it is played like a cold open (see
    /// `flyBoutEndToDiscard`).
    @State private var seenArrivalTick = 0
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

    public init(controller: MessageTurnController, onSend: @escaping (Data, Bool) async -> Void,
                onNewGame: @escaping () -> Void = {}, onUnstage: @escaping () -> Void = {},
                alsoStaged: Bool = false, onDiagnostics: @escaping () -> Void = {}) {
        self.controller = controller
        self.onSend = onSend
        self.onNewGame = onNewGame
        self.onUnstage = onUnstage
        self.alsoStaged = alsoStaged
        self.onDiagnostics = onDiagnostics
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
                // Round-8 #4: the fan renders the DISPLAY order (the stored
                // per-game arrangement; the store and the fan's live order are
                // kept in sync by onOrderChanged), so the analytical slots must
                // be computed against it too, or every cosmetic reorder would
                // log as a phantom geometry mismatch here.
                let display = FHandFan.displayOrder(
                    cards: hand,
                    order: MessageGameStore.shared.handOrder(gameId: controller.gameIdString))
                let rects = FHandFan.slotRects(cards: display, width: handFrame.width,
                                               crop: Self.handCrop)
                var worst = 0.0, worstId = ""
                for c in hand {
                    guard let a = rects[c.identity]?.offsetBy(dx: handFrame.minX, dy: handFrame.minY),
                          let m = $0[c.identity] else { continue }
                    let d = max(abs(a.midX - m.midX), abs(a.midY - m.midY))
                    if d > worst { worst = d; worstId = c.identity }
                }
                if worst > 2 {
                    AnimLog.say("SLOTCHECK MISMATCH n=\(hand.count) worst=\(String(format: "%.1f", worst))pt @\(worstId)")
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
            if controller.openReplayEvents.isEmpty { controller.consumeReplayPending() }
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
            // Round-10e (owner: "for reproducibility of taps I would suggest a
            // fixed seed, enabled by some flags - dev build flags only"): the
            // iMessage extension is launched by Messages, so it never sees our
            // environment. This DEBUG-only App Group key is the equivalent
            // switch, settable from the outside with
            //   xcrun simctl spawn <sim> defaults write \
            //       group.cards.foolish.msg dev.automove -bool YES
            // so a verification run can play a legal move without depending on
            // synthesized drags or which seat the deal handed us. Compiled out
            // of every Release build with the rest of this block.
            // A FILE in the App Group container, not a UserDefaults key:
            // `defaults write` from outside lands in the wrong domain, and
            // cfprefsd caches group prefs in memory (a write is not seen until
            // a reboot). A file is read straight off disk, every open.
            let devAutoMove = FileManager.default
                .containerURL(forSecurityApplicationGroupIdentifier: "group.cards.foolish.msg")
                .map { FileManager.default.fileExists(atPath: $0.appendingPathComponent("dev.automove").path) }
                ?? false
            if ProcessInfo.processInfo.environment["HARNESS_AUTOMOVE"] != nil || devAutoMove,
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
            // The board's live collapse fraction (0 = expanded, 1 = fully compact
            // drawer). Round-11 narrowed what this may drive: it gates the send
            // hint and widens the opponent ring, and that is ALL. Nothing that
            // positions the hand or the chrome may read it - see `handCrop`.
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
            // The hand's on-screen height (one row, or two once M6 splits it).
            // The self-role indicator and action bar float a fixed gap ABOVE
            // this, so a hand that grows to two rows pushes them up with it.
            //
            // Round-7 ("buttons should not move"): measured off the FULL `myHand`,
            // NOT `laidOutHand`. A deal/pickup adds cards that land one at a time
            // (each `laidOutHand`-visible only once its own flight opens its slot),
            // so anchoring off `laidOutHand` grew this height card-by-card and the
            // buttons visibly FLOATED UP as the cards arrived. `myHand` is already
            // the FINAL hand the instant the move applies, so the buttons sit at
            // their final spot from the start and the incoming cards fill UP toward
            // them - the hand makes room, the buttons hold still.
            let handHeight = FHandFan.height(cards: myHand, availableWidth: handWidth,
                                             crop: Self.handCrop)
            // The buttons/role mark ride THIS, mirrored out via `.onChange` below so
            // their movement is a snap, never the board spring (see `buttonLift`).
            // Until the first mirror lands (-1) they read `handHeight` directly, so a
            // fresh board places them correctly on the very first paint.
            let lift = buttonLift < 0 ? handHeight : buttonLift
            ZStack {
                // Battles — dead centre of the board (web: absolute, both axes).
                //
                // Round-11, filmed on an iPhone SE: in that 262pt drawer the
                // played card overlaps the chrome, because a board centred in
                // its whole height has nowhere to put a battle once a deck row,
                // a chrome row and a full-height hand are in. Centring it in the
                // free band instead (below the deck, above the chrome) fixes the
                // small phone but lifts the table ~30pt on every other one, and
                // the owner's call is that the SE crop is acceptable for now:
                // "an iPhone SE is so small, collision is fine". Left dead
                // centre deliberately, not by omission.
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
                    // A CONSTANT, always-present, fixed-size container (owner:
                    // "the action column CONTAINER could be a constant always
                    // present fixed size view... just reserve enough height for
                    // two"): the column's own geometry then never changes as
                    // pills come and go, so it has nothing to interpolate and
                    // behaves like the settings squares. Two pills is the
                    // deepest the board can ever show - a defender holding a
                    // selection that is both a legal cover and a legal pass -
                    // so 40 + 8 + 40 = 88, bottom-anchored, and a second pill
                    // grows upward into reserved space instead of moving the
                    // first one.
                    .frame(width: 128, height: 88, alignment: .bottomTrailing)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                    .padding(.trailing, 4).padding(.bottom, lift + 4)
                    .animation(nil, value: controller.view)   // never float the buttons — see the role mark above
                    // Round-10g, and the ONLY change to a collapse the owner
                    // otherwise signed off on ("make it like that but JUST fix
                    // the undo button"): the pill must not be INTERPOLATED.
                    // This column is the one piece of chrome whose CONTENT
                    // changes at the staging frame (Attack -> Undo), and a
                    // newly-inserted pill had its position animated by whatever
                    // transaction was in flight - the collapse's own
                    // `withAnimation`. Measured off the film: Undo appeared
                    // ~295pt above its slot and flew down over ~7 frames while
                    // the settings squares - a CONSTANT view, nothing to insert
                    // - sat still. `.animation(nil, value:)` above cannot stop
                    // that (it only covers changes driven by `controller.view`)
                    // and FActionBar's own `.transaction` sits BELOW this
                    // placement, so it cannot either. Here it can, and it
                    // touches nothing else: the squares, the role mark, the
                    // hand and the board's collapse are exactly as they were.
                    .transaction { $0.animation = nil }

                // 1.0(4): Settings + Rulebook squares, MIRRORING the action
                // column on the LEFT. Same 40pt height as the action pills,
                // square, at the same bottom line (lift + 4) and the SAME 16pt
                // edge inset (4 outer + FSpace.m inner, exactly like actionBar's
                // trailing). The two squares + their gap span one action-button
                // width (40 + 16 + 40 = 96), so the left group is the mirror of
                // the right one. Round-9 (owner: "we need to bring them back"):
                // ALWAYS visible - the old collapse fade hid them in the compact
                // drawer, which is where most play happens, so in practice the
                // pair read as removed. The board spring never floats them.
                undoSlot
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                    .padding(.trailing, 20).padding(.bottom, lift + 4)
                    .animation(nil, value: controller.view)

                settingsHelpBar
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomLeading)
                    .padding(.leading, 4).padding(.bottom, lift + 4)
                    .animation(nil, value: controller.view)

                // My hand hugs the bottom (web: bottom max(10, safe-area)); the
                // outer .padding(12) is the safe-area inset that keeps it unclipped.
                hand(view, crop: Self.handCrop, reserveNoSlot: deferredSlots)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)

                // Round-8 #3 / round-9: the staged-but-unsent reminder - a blue
                // arrow bobbing under Messages' own Send button (in the compose
                // bar directly above this view's top-right corner), with a
                // caption in the same blue. COLLAPSED VIEW ONLY (the expanded
                // board is not under the Send button at all). `alsoStaged` is
                // the surface's own stage (the starter's LIVE handoff renders a
                // board with nothing pending, but its bubble still needs
                // sending). Fuse + fade live inside StagedSendHint.
                StagedSendHint(staged: controller.canSend || alsoStaged,
                               visible: collapse > 0.95,
                               centerFromTrailing: Self.sendHintCenterFromTrailing)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
                    .animation(nil, value: controller.view)   // never ride the board spring

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
            // Mirror the hand height out to `buttonLift` so the buttons SNAP to it
            // (this callback runs in its own no-animation transaction) instead of
            // floating on the board spring. Since round-11 this only ever fires on
            // a ROW-COUNT change - the drawer height no longer enters it - so the
            // log line is now the whole story of why the chrome ever moves.
            .onChange(of: handHeight) {
                AnimLog.say("handHeight \(Int(buttonLift))->\(Int($0)) cards=\(myHand.count) rows=\(FHandFan.rowCount(cards: myHand, availableWidth: handWidth))")
                buttonLift = $0
            }
            .onAppear { buttonLift = handHeight }
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

    /// How much of each hand card the board hides off the bottom. A CONSTANT,
    /// and the reason it is one is round-11.
    ///
    /// This used to be the live `collapseFraction`: the compact drawer showed
    /// the top half of every card (crop 1), the expanded board the whole card
    /// (crop 0). That is a 36pt change in the hand's reserved height, and it
    /// bought the compact drawer 36pt of table - but it is ALSO, measurably,
    /// every vertical defect the owner reported in the collapse:
    ///
    /// * The hand LANDED SOMEWHERE ELSE. A cropped card is drawn full height,
    ///   top-aligned in a half-height slot, so cropping slides the cards down
    ///   past their own container. Filmed: the hand's top edge rested at y=760
    ///   expanded and y=791 compact.
    /// * The chrome landed somewhere else too, in the other direction. The
    ///   buttons float `handHeight` above the board's bottom, so a hand that
    ///   reserves 36pt less pulls them down 36pt - reading as the buttons and
    ///   the hand drifting APART across the transition.
    /// * The BOUNCE. The host does not hand us a monotone height ramp; it
    ///   re-lays the board out at whatever intermediate (and out-of-order)
    ///   sizes the drawer animation passes through - one filmed collapse
    ///   reported 748, 315, 307, 778, 758, 253, 315. Every one of those was a
    ///   different crop, so the hand and the chrome chased the noise, under the
    ///   card spring, and rang: the hand's top edge went 807, 763, 767, 776,
    ///   790, 793, 787, 790 before settling.
    ///
    /// Pinning the crop makes all three impossible rather than smaller: the
    /// hand and the chrome now have geometry that does not mention the drawer
    /// height at all, so there is nothing to interpolate, nothing to ring, and
    /// the resting expanded and compact layouts are the same layout. The
    /// compact drawer pays 36pt of table for it. `FHandFan` keeps the crop
    /// parameter (it is tested, and a short drawer is exactly the case it was
    /// built for) - this board simply no longer drives it from live geometry.
    static let handCrop: CGFloat = 0

    /// The board's CONTINUOUS collapse fraction from its own height: 0 at/above
    /// `expandedAnchor` (the resting expanded board), 1 at/below `compactAnchor`
    /// (the resting compact drawer), linearly ramped between. What may read it
    /// is now deliberately small - the send hint's visibility and the opponent
    /// ring's radius - because anything that POSITIONS something off the live
    /// drawer height inherits the noise described on `handCrop`. The anchors are
    /// chosen so both resting states saturate: `compactAnchor` is the old
    /// `height < 340` cliff, which that binary test proved the compact drawer
    /// always sits below (so it -> 1 at rest), and the near-full-screen expanded
    /// board is always well above 440 (so it -> 0 at rest).
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
                // ONE size table for both role rows: `FRoleMark.size`. This
                // call site and FSeatBadge's `roleRow` used to carry their own
                // numbers with a comment on each asking the other to keep in
                // step, which is not a mechanism - my own role must not read
                // bigger than an opponent's just because it is mine.
                HStack(spacing: FSpace.xs) {
                    if saidGood { FCheck(size: FRoleMark.check) }
                    if isDefender { FShield(size: FRoleMark.shield) }
                    else if isAttacker { FSword(size: FRoleMark.sword) }
                }
            }
        }
    }

    #if DEBUG
    private static var lastGridTrace = ""
    static func traceGrid(sweeping: Bool, shown: [BattleView], hidden: Set<String>) {
        let pairs = shown.filter { $0.defense != nil }.count
        let visible = shown.reduce(0) { n, b in
            n + (hidden.contains(b.attack.identity) ? 0 : 1)
              + ((b.defense.map { hidden.contains($0.identity) ? 0 : 1 }) ?? 0)
        }
        let line = "grid sweeping=\(sweeping) cells=\(shown.count) pairs=\(pairs) visible=\(visible) hidden=\(hidden.count)"
        if line != lastGridTrace { lastGridTrace = line; AnimLog.say(line) }
    }
    #endif

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
        #if DEBUG
        // What the table is actually PAINTING, logged only when it changes - so
        // a re-layout mid-sweep shows up as a line instead of having to be read
        // off a video frame. Added for the round-12 pickup sweep and kept: it is
        // what caught #11, where a card sat `hidden` on a table nothing was ever
        // going to un-hide (`visible=0 hidden=1` with no flight after it).
        Self.traceGrid(sweeping: sweeping, shown: shown,
                       hidden: sweeping ? sweptFlownIds : veiledCardIds)
        #endif
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
        defer { controller.consumeReplayPending() }
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
            // Fetched BEFORE the cover's landing flight, not after: the swept
            // table has to change the instant that flight ends (see below), and
            // a kernel round-trip in between is a paint the cover spends
            // nowhere. Nothing is visible during the fetch either way - the
            // held ghost from `playAt` is already resting at the card's source.
            let events = await MessageKernel.shared.lastMoveEvents(viewer: controller.mySeat,
                                                                   atomsBefore: controller.animAtomsBefore)
            if let pc = matchedCover {
                // BALANCED BY `defer`, like every other sequence claim in this
                // file. It was a bare `+= 1` / `-= 1` pair when round 16 added
                // it, and an early return or a cancellation between them would
                // have leaked the counter PERMANENTLY - after which
                // `BoardAnimator.waitForSettle` (which the extension awaits
                // before staging a bubble) spends its full 8-second timeout on
                // every send for the rest of the process. That is indis-
                // tinguishable from "sometimes it just hangs", so it is not
                // something to leave resting on this function having no other
                // way out.
                BoardAnimator.sequenceDepth += 1
                defer { BoardAnimator.sequenceDepth -= 1 }
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
                if let covered = Self.coveredSweep(events, current: sweepBattles) {
                    setSweep(covered)
                }
            }
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
            // ROUND 16: HAND THE COUNTS BACK. Every caller freezes them to the
            // pre-move board SYNCHRONOUSLY (`play`, then `flyBoutEndToDiscard`)
            // and relies on this function's teardown to release them - but that
            // teardown is installed below, past this guard, so a stream that
            // came back empty left every badge and the deck pinned to the board
            // before the move, for as long as it took the next move to arrive
            // and re-freeze them. An opponent stuck a card too high until they
            // played again is the owner's "briefly bumped, then they play a
            // single card and it goes back down".
            deckCountOverride = nil; discardCountOverride = nil; seatCountOverride = [:]
            animator.clearPreHidden()
            clearSweep()
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

        let groups = Self.parallelGroups(events)
        for (gi, group) in groups.enumerated() {
            // Every step below is written against ONE event; a group of several
            // is a MULTI-CARD COVER, whose cards must fly together (see
            // `parallelGroups`). `ev` leads the group for everything that reads
            // one event - the make-room, the deck override, the sweep marks -
            // and only the FLIGHTS are built from all of them, which is exactly
            // the difference between "at the same time" and "one after another".
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
                AnimLog.say("stream#\(run) step \(ev.kind.map(String.init(describing:)) ?? "?")@\(ev.seat) n=\(group.count) flights=\(f.count) [\(f.map(\.id).joined(separator: ","))]")
                return f
            }
            // The board settles to the LAST event of the group: the intermediate
            // states inside one move are boards nobody was ever shown.
            if let s = group.last?.state ?? ev.state {
                deckCountOverride = s.deckCount
                discardCountOverride = s.discardCount
                for p in s.players where p.seat != controller.mySeat { seatCountOverride[p.seat] = p.handCount }
            }
            // ROUND 16: a cover that ended the bout HOLDS before the sweep takes
            // the table away. See `boutEndHold` for why this one beat is unlike
            // every other gap in a sequence. Placed here rather than at either
            // call site because both sides reach it: the defender's own board
            // arrives with the landing flight already flown (its cover step is a
            // no-op - the card is not in the final view to fly to), and every
            // receiver replays the same stream from the top.
            if Self.holdsAfter(groups, gi) {
                AnimLog.say("stream#\(run) hold \(Int(boutEndHold * 1000))ms - bout-ending cover")
                try? await Task.sleep(nanoseconds: UInt64(boutEndHold * 1_000_000_000))
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
    /// Split a turn's events into the steps that PLAY, which is not the same as
    /// the events that happened.
    ///
    /// One step, one beat of animation. Almost every event is its own step, and
    /// there is exactly one exception: a defender covering SEVERAL CARDS IN ONE
    /// MOVE. The kernel emits a COVER event per card (one engine hook per pair,
    /// each carrying its own board snapshot), so a two-card cover arrives as two
    /// events - and played as two steps, the receiver watches the cards leave
    /// the hand one after the other, while the player who made the move saw them
    /// go together. Same move, two different animations, which is the defect.
    ///
    /// WHAT THE CHAIN CANNOT SAY, and why this groups by adjacency. The obvious
    /// rule would be "group the covers that came from one MOVE" - but the move
    /// boundary is not on the chain to group by. A v6 body records atoms, and
    /// the codec spends one COVER atom per card, so a defender who covered two
    /// cards at once and a defender who covered twice produce the same atoms in
    /// the same order, byte for byte. (This is the same blindness round 16 met
    /// at the bubble boundary, one level down, and it is why that one had to be
    /// answered by a new header field rather than by reading the body harder.)
    ///
    /// So the boundary this uses is THE BUBBLE, which the chain does say: these
    /// events are one bubble's (`lastMoveEvents` returns exactly what this
    /// bubble added), and consecutive covers by one seat inside it fly together.
    /// Two covers sent as two bubbles are two separate replays and never meet
    /// here, which is the case the owner cared about - "that is ok if they are
    /// in fact in the same bubble, but if they are not in the same bubble..."
    /// The residual is a defender who staged two covers and sent them as one
    /// bubble: those now fly together, having arrived together. Reading it any
    /// other way would need a move marker in every replay code ever written.
    ///
    /// CONSECUTIVE, so a bout boundary still splits: a cover that closed a bout
    /// puts a DISCARD between it and the next cover, which ends the run.
    ///
    /// Only COVER groups. Attacks and passes already carry every card of the
    /// move in one event; deals and refills are per seat; and a bout's closing
    /// DISCARD/REFILL are the cover's consequences, not part of the same
    /// movement - they keep their own beats, which is what makes the counts
    /// settle in the right order.
    static func parallelGroups(_ events: [GameEvent]) -> [[GameEvent]] {
        var out: [[GameEvent]] = []
        for ev in events {
            if ev.kind == .cover, let head = out.last?.first,
               head.kind == .cover, head.seat == ev.seat {
                out[out.count - 1].append(ev)
            } else {
                out.append([ev])
            }
        }
        return out
    }

    /// ROUND 16: does the sequence HOLD after group `i`? True only for a COVER
    /// that ended its bout - the case the owner named, "when you cover and cause
    /// the deck to discard (last defense)".
    ///
    /// The bout end is the DISCARD, so this looks forward for one. Not merely at
    /// the next group: a bout that ends because the defender's last card went
    /// down puts their OUT (and, at the end of a game, a magic transition)
    /// between the cover and the trash, and those carry no flight of their own -
    /// they are notices, not movements, so they neither separate the cover from
    /// its consequence nor deserve a hold of their own. Anything that DOES move a
    /// card ends the scan: a refill or a pickup after a cover means the table did
    /// not close on it, and holding there would be a stall in the middle of a
    /// sequence that is still going somewhere.
    ///
    /// The far commoner bout end - defender covers, an ATTACKER then says good -
    /// is two bubbles, so the discard arrives in a stream with no cover in it at
    /// all and nothing here fires. That is right: nobody covered in that beat,
    /// and the table has been sitting there readable since the last one.
    static func holdsAfter(_ groups: [[GameEvent]], _ i: Int) -> Bool {
        guard i >= 0, i < groups.count, groups[i].first?.kind == .cover else { return false }
        for j in (i + 1)..<groups.count {
            switch groups[j].first?.kind {
            case .discard, .cardsToTrash: return true
            case .out, .magicTransition, .flipped: continue
            default: return false
            }
        }
        return false
    }

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
        let rects = FHandFan.slotRects(cards: laidOut, width: handFrame.width, crop: Self.handCrop)
        guard let local = rects[card.identity] else { return nil }
        return local.offsetBy(dx: handFrame.minX, dy: handFrame.minY)
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
    private func laidOutHandNow(_ view: GameView) -> [Card] {
        Self.laidOut(hand: view.me?.hand ?? [], deferred: handSlotDeferred,
                     order: MessageGameStore.shared.handOrder(gameId: controller.gameIdString))
    }

    /// The pure half, so the ordering contract can be asserted directly (it is
    /// the whole of round-12's deal-lands-in-the-wrong-slot fix, and a test that
    /// only exercised `FHandFan.displayOrder` would pass against the bug -
    /// the bug was never in `displayOrder`, it was in not CALLING it).
    static func laidOut(hand: [Card], deferred: Set<String>, order: [String]) -> [Card] {
        FHandFan.displayOrder(cards: hand.filter { !deferred.contains($0.identity) },
                              order: order)
    }


    /// Deck/discard/every-seat's hand count as of BEFORE this stream's `events`.
    /// This freezes the on-screen counts to their pre-move values before the
    /// sequence starts; every step then jumps forward to its OWN board
    /// (GameEvent.state) as its flight lands, so this only sets the starting
    /// frame, not the per-step values.
    ///
    /// ANCHOR ON THE FIRST EVENT AND UNDO EXACTLY ONE. Every event carries the
    /// board it produced, so `events[0].state` IS the board one event in - and
    /// getting from there to the board before it is a single undo. This is not a
    /// shortcut for undoing all of them; it is the only version that is right.
    ///
    /// ROUND 16, the owner: "I sometimes saw the deck suddenly go to 5 cards,
    /// then deal, and now I have 6 cards? Is it a problem with the flipped
    /// card?" It was the flipped card. Undoing a REFILL means putting its cards
    /// back in the deck, and at the end of a game that is wrong: the trump lies
    /// under the deck and is handed out LAST, but `deck_count` never counted it,
    /// so a refill of two off a deck of one is real and the walk-back put two
    /// back. The deck badge then opened one too high and corrected itself as the
    /// draw landed - which is exactly the report, and exactly why it only ever
    /// happened near the end of a game. Proven and pinned in
    /// MessageCountWindingTests against the kernel's own boards.
    ///
    /// A refill can never be the FIRST event of a stream - it is always some
    /// bout end's consequence, so a pickup, a trash or a magic transition
    /// precedes it - which is what makes one undo safe where n were not. The
    /// full walk remains as the fallback for a stream with no snapshots at all,
    /// which the packed evwire does not produce (every event carries one).
    static func preCounts(_ events: [GameEvent], finalView: GameView)
        -> (deck: Int, discard: Int, hand: [Int: Int]) {
        let anchor = events.first?.state
        var deck = anchor?.deckCount ?? finalView.deckCount
        var discard = anchor?.discardCount ?? finalView.discardCount
        var hand = Dictionary(uniqueKeysWithValues:
            (anchor?.players ?? finalView.players).map { ($0.seat, $0.handCount) })
        let undo = anchor == nil ? Array(events.reversed()) : Array(events.prefix(1))
        for ev in undo {
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

    /// The table a replayed pickup/discard should be shown sweeping off.
    ///
    /// ROUND 12 ("pickup animation sometimes quickly rearranges into grid before
    /// moving to hand for many players"). `controller.openReplayPreBattles`
    /// RECONSTRUCTS the pre-pickup table from the pickup step's own cards, and
    /// it has to guess the pairing: the kernel hands over one flat list, so the
    /// reconstruction lays every card in its own uncovered slot. A table that
    /// really held three attacks with two of them covered comes back as FIVE
    /// single-card battles - a different grid, with a different cell count and a
    /// different shape.
    ///
    /// On a cold open nobody sees that, because there is no earlier frame to
    /// compare it against. But a pickup that ARRIVES while the board is up runs
    /// this same path, and there the player was looking at the real covered
    /// table a frame ago - so the reconstruction reads as the table shuffling
    /// itself into a grid before anything flies. The more players, the more
    /// throw-ins, the more covered pairs get split, and the worse it looks -
    /// which is exactly the "for many players" in the report.
    ///
    /// So: prefer the REAL table when this board has one. `lastBattles` is the
    /// last table that actually had cards on it (kept by `flyBoutEndToDiscard`),
    /// and it is the truth the reconstruction is approximating. It is only used
    /// when it accounts for every card the sweep is about to move; otherwise it
    /// is a stale table from an earlier bout and the reconstruction - which is at
    /// least about the right cards - wins.
    private func sweepTableForReplay() -> [BattleView] {
        let reconstructed = controller.openReplayPreBattles
        guard !reconstructed.isEmpty else { return [] }
        let need = Set(reconstructed.flatMap { b in
            [b.attack.identity] + (b.defense.map { [$0.identity] } ?? [])
        })
        let have = Set(lastBattles.flatMap { b in
            [b.attack.identity] + (b.defense.map { [$0.identity] } ?? [])
        })
        return need.isSubset(of: have) ? lastBattles : reconstructed
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
    private func tableCardSource(_ card: Card, fallbackIndex i: Int) -> (rect: CGRect, tilt: Double)? {
        let tilt = sweptTilt(of: card)
        if let base = lastBattleCardFrames[card.identity] {
            return (Self.swungAboutBottom(base, degrees: tilt), tilt)
        }
        if let src = discardSource(for: card) { return src }
        guard let center = approximateTableCenter() else { return nil }
        return (center.offsetBy(dx: CGFloat(i) * 6, dy: CGFloat(i) * 4), 0)
    }

    /// How far over a card on the swept table is lying: +`coverAngle` for a
    /// cover, -`coverAngle` for the attack under one, 0 for an uncovered attack.
    /// Mirrors what `FBattleGrid` actually draws (its attack takes the negative
    /// tilt, its defense the positive one).
    private func sweptTilt(of card: Card) -> Double {
        let table = sweepBattles.isEmpty ? lastBattles : sweepBattles
        guard let b = table.first(where: { $0.attack == card || $0.defense == card })
        else { return 0 }
        if b.defense == card { return FBattleGrid.coverAngle }
        return b.defense != nil ? -FBattleGrid.coverAngle : 0
    }

    /// A card's VISUAL rect once it has been rotated about its own bottom edge.
    ///
    /// ROUND 12 - this is the "pickup animation quickly rearranges into grid"
    /// bug, and it is a layout-vs-render mix-up. `FBattleGrid` stacks a battle's
    /// two cards in a `ZStack(alignment: .bottom)` and separates them ONLY with
    /// `.rotationEffect(anchor: .bottom)`. Rotation is a render transform: it
    /// does not move the layout frame, so the rect each card publishes through
    /// `BattleCardFramesKey` is the SAME rect for the attack and the cover.
    /// Flying both ghosts from that rect makes the covering card jump onto its
    /// attack the instant the sweep starts - ten cards collapsing into five
    /// stacked pairs, which is precisely the "rearrange into a grid" the owner
    /// saw, and why it is worse the more covered pairs the table holds.
    ///
    /// Rotating about the bottom edge swings the centre sideways by
    /// sin(tilt)*halfHeight and down by (1-cos(tilt))*halfHeight - the same
    /// correction `discardSource` has always applied to its slot rect. This puts
    /// it where it belongs, on the per-card rect that supersedes it.
    static func swungAboutBottom(_ r: CGRect, degrees: Double) -> CGRect {
        guard degrees != 0 else { return r }
        let rad = degrees * .pi / 180
        let half = r.height / 2
        return r.offsetBy(dx: CGFloat(sin(rad)) * half,
                          dy: CGFloat(1 - cos(rad)) * half)
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

    /// ROUND 16: the table a bout-ending COVER should be swept off - the kernel's
    /// covered table (`preBoutTable`), the same one a receiver's open-replay lays
    /// out, so both sides sweep the identical board.
    ///
    /// nil unless it ACCOUNTS FOR everything the current sweep already holds. The
    /// live sweep is the real prior view and is never wrong about which cards were
    /// on the table; this only ever earns the swap by ADDING the cover to it. A
    /// stream that came back short (or with the flattened one-slot-per-card shape
    /// `preBoutTable` reconstructs for a pickup) is refused rather than allowed to
    /// drop a covered pair off the table mid-sequence.
    static func coveredSweep(_ events: [GameEvent], current: [BattleView]) -> [BattleView]? {
        func ids(_ bs: [BattleView]) -> Set<String> {
            Set(bs.flatMap { [$0.attack.identity] + ($0.defense.map { [$0.identity] } ?? []) })
        }
        let table = MessageTurnController.preBoutTable(events)
        guard !table.isEmpty, ids(current).isSubset(of: ids(table)) else { return nil }
        return table
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
        setSweep(sweepTableForReplay())

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
                            onHelp: { showRules = true },
                            onDiagnostics: onDiagnostics)
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
            // ROUND 16 (owner): "you cannot pickup within 15 seconds of the
            // attack ... this is to give attackers a fair chance to throw in
            // additional cards". While that hold stands the pill is simply not
            // there - no greyed-out button, no countdown, nothing to press -
            // and it appears on its own when the hold lapses (the controller
            // ticks it down). The same number refuses the move in
            // `MessageTurnController.apply`, so this is the polite half of the
            // rule, not the rule.
            canPickup: defending && !view.battles.isEmpty && cards.isEmpty
                && !(view.me?.isOut ?? false) && !controller.canSend
                && controller.pickupHold == 0,
            canDone: acting && CardPlay.canSayGood(battles: view.battles, legal: controller.legal) && cards.isEmpty,
            canUndo: false,   // the board draws its own - see `undoSlot`
            onAttack: { playAt(.table, cards, view) },
            onCover: { playCover(cards, view) },
            onPass: { playAt(.table, cards, view) },
            onPickup: { play(.pickup) },
            onDone: { play(.good) },
            onUndo: { undoAction() }
        )
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
    private func undoAction() {
        Task {
            await controller.undo()
            if controller.canStage { await stageNow() }
            else if controller.isContinuation { await stageBaseNow() }
            else { onUnstage() }
        }
    }

    /// The Undo pill, built and placed EXACTLY like `settingsHelpBar`: a
    /// constant, always-present, fixed-size slot that the button merely appears
    /// INSIDE of.
    ///
    /// Round-10g (owner: "literally just take whatever you do to the settings
    /// button and do it to the undo button"). Undo used to live in FActionBar's
    /// VStack, whose height and membership change the instant a move is staged
    /// (Attack disappears, Undo appears) - so the pill was INSERTED into a
    /// column that was itself resizing, and SwiftUI animated its position from
    /// wherever the old layout put it. Filmed repeatedly: Undo appearing ~280pt
    /// above its slot and flying down over ~7 frames while the settings squares
    /// - which are always there, at a fixed size - never moved a pixel.
    /// Neither `.animation(nil, value:)`, nor `.transaction { animation = nil }`
    /// on the placement, nor a fixed box around the whole column stopped it;
    /// not inserting anything does. Undo and the play buttons are mutually
    /// exclusive by construction (every play button is gated on `!canSend`,
    /// Undo on `canSend`), so hoisting it out of the column changes no layout.
    private var undoSlot: some View {
        ZStack {
            if controller.canSend {
                FButton(FStrings.t("ios.msg.undo"), kind: .wood, compact: true,
                        fixedWidth: 96, action: undoAction)
            }
        }
        .frame(width: 96, height: 40)
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
    private func stageBaseNow() async {
        guard controller.isContinuation else { return }
        // Always a consequence of undo-to-empty (the only caller is onUndo), so keep
        // the board expanded - never collapse on an undo.
        if let payload = try? await controller.stagedPayload() {
            await onSend(payload, controller.lastChangeWasUndo)
        }
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

    /// Cover button: cover the BIGGEST uncovered attack the selection can beat
    /// (round 16 - see `CardPlay.bestCoverTarget`; the drag path names its own
    /// target and is untouched).
    private func playCover(_ cards: [Card], _ view: GameView) {
        guard let i = CardPlay.bestCoverTarget(cards: cards, battles: view.battles,
                                               legal: controller.legal,
                                               trumpSuit: view.trumpSuit) else {
            Haptics.fire(.reject); return
        }
        playAt(.battle(i), cards, view)
    }

    private func coverableBattles(_ view: GameView) -> Set<Int> {
        let out = CardPlay.coverableBattles(cards: selectedCards(view), battles: view.battles, legal: controller.legal)
        return out
    }

    /// Round-8 #3 / round-9: where the send reminder's CENTRE sits, measured
    /// from the SCREEN's right edge. Measured off a real device screenshot:
    /// Messages' Send circle sits inside the compose field's right end with the
    /// drawer chrome inset around it, its centre ~42pt from the screen edge
    /// (the first guess of ~24 read the field as nearly full-bleed - the owner:
    /// "the arrow should be bumped a bit to the left").
    static let sendHintCenterFromScreenTrailing: CGFloat = 42
    /// The same axis measured from the BOARD's trailing edge - the board is
    /// inset 8 from the screen (`.padding(.horizontal, 8)` on the root).
    static let sendHintCenterFromTrailing: CGFloat = sendHintCenterFromScreenTrailing - 8
}

/// Round-8 #3: the shared axis the send reminder's arrow and caption align on
/// - the vertical line under Messages' Send button. The arrow always centres
/// on it; the caption centres on it too UNLESS that would push it past the
/// board's right edge (a Russian caption did exactly that, running off
/// screen), in which case its own guide shifts so it hugs the edge instead.
private extension HorizontalAlignment {
    enum SendAxis: AlignmentID {
        static func defaultValue(in d: ViewDimensions) -> CGFloat {
            d[HorizontalAlignment.center]
        }
    }
    static let sendAxis = HorizontalAlignment(SendAxis.self)
}

/// Round-8 #3 / round-9: the staged-but-unsent reminder. An up-arrow in the
/// exact glyph Messages' own Send button carries (SF Symbols `arrow.up`, bold -
/// the same symbol the compose bar's circle draws in white), in the same system
/// blue that circle is filled with, bobbing on a sine wave; under it, at the
/// BOTTOM of the arrow's travel, a caption in the action buttons' own text
/// treatment (owner: "larger and bulder, like the rest action button texts").
/// The arrow only travels UP from its resting spot, so its rest position -
/// directly above the caption - is the bottom of the wave.
struct SendHintReminder: View {
    /// True while the reminder is not actually on screen (fuse not elapsed, or
    /// the drawer is expanded): freezes the TimelineView so an invisible arrow
    /// doesn't burn frames inside a Messages extension.
    var paused = false
    @Environment(\.colorScheme) private var scheme

    /// Messages fills its Send circle with the system blue, so match it
    /// exactly: UIKit systemBlue's resolved values (#007AFF light, #0A84FF
    /// dark), written out as literals so FoolishKit needs no UIKit import.
    private var sendBlue: Color {
        scheme == .dark ? Color(red: 0x0A / 255, green: 0x84 / 255, blue: 0xFF / 255)
                        : Color(red: 0x00 / 255, green: 0x7A / 255, blue: 0xFF / 255)
    }

    // ROUND 16 (owner): "make the send button hint arrow more obvious. Make the
    // arrow taller, a bit more width, and make it move faster and in a larger
    // range. It should be pretty clear that you aren't meant to hit the hint
    // arrow, but the actual iMessage send arrow (that is above the collapsed
    // view)." The four numbers below are that sentence.
    //
    // The arrow is drawn RESIZABLE at an explicit width x height rather than at
    // a font size, because "taller AND a bit wider" are two numbers and a font
    // size is one - the glyph is deliberately stretched ~20% taller than the
    // symbol's own proportions, which also makes it read as an arrow POINTING
    // somewhere rather than as a button you press.
    static let arrowSize = CGSize(width: 21, height: 29)   // was ~15 x 17 (font 16 bold)
    /// Peak-to-trough travel. Nearly 3x the old 5pt: the whole point is that the
    /// eye follows it UP, off this view and onto Messages' own Send button.
    static let bobTravel: CGFloat = 14
    /// Seconds per bob. Was 1.5 - slow enough to read as decoration.
    static let bobPeriod: Double = 0.85
    /// Where the hint RESTS, measured down from the top of the container it is
    /// laid into (the board's own top inset). NEGATIVE: round 16 lifts the whole
    /// hint out of the board and into the drawer's top margin - owner: "take the
    /// entire send hint div and just move it up".
    ///
    /// It is also smaller than `bobTravel`, so the crest rides higher still.
    /// Raising the REST position is the only way to get closer to Messages' Send
    /// button: growing the travel alone just pushes the rest position down,
    /// since the crest is pinned by whatever headroom is left above it.
    ///
    /// The only thing in that margin is the grabber, which is CENTRED while this
    /// hint hugs the trailing edge, so there is nothing up there to collide
    /// with; the drawer's rounded corner is the real ceiling and the crest still
    /// clears it (measured on device: the tip stops ~8pt short of the edge).
    static let crestRoom: CGFloat = -9

    var body: some View {
        // Round-9: the bob is a TimelineView-driven pure sine of wall-clock
        // time, not a `repeatForever` @State animation - the stateful kind is
        // silently CANCELLED whenever an ancestor re-renders inside a
        // no-animation transaction (this board carries several), which is why
        // "sometimes the send arrow doesn't go up and down". A value computed
        // fresh every frame cannot be cancelled.
        //
        // ROUND 16: the WHOLE hint rides the wave, caption included (owner:
        // "lets make the text move up and down so it looks less like a button
        // and more like an arrow to the imessage button"). With the caption
        // pinned and only the arrow moving, the pair read as a label with a
        // fidgeting icon - a control. Moving together, they read as one object
        // travelling toward the Send button above, which is the whole message.
        TimelineView(.animation(minimumInterval: nil, paused: paused)) { ctx in
            let t = ctx.date.timeIntervalSinceReferenceDate
            // Rest at 0, crest at -bobTravel: (1-cos) starts the wave at rest
            // and only ever lifts away from it.
            let lift = Self.bobTravel / 2 * (1 - cos(t * 2 * .pi / Self.bobPeriod))
            let arrow = Image(systemName: "arrow.up")
                .resizable()
                .frame(width: Self.arrowSize.width, height: Self.arrowSize.height)
            VStack(alignment: .sendAxis, spacing: 3) {
                // Round-10 #3 (owner): a WHITE stroke around the blue arrow so
                // it carries on the wool. SF Symbols have no outline mode, so
                // the stroke is the same glyph stamped in white at 8 compass
                // offsets under the blue one - a solid ring. Round 16: 1.6, up
                // with the glyph, so the bigger arrow keeps the same ratio of
                // outline to body rather than wearing a hairline.
                ZStack {
                    ForEach(0..<8, id: \.self) { i in
                        let a = CGFloat(i) * .pi / 4
                        arrow.foregroundColor(.white)
                            .offset(x: 1.6 * cos(a), y: 1.6 * sin(a))
                    }
                    arrow
                }
                .alignmentGuide(.sendAxis) { d in d[HorizontalAlignment.center] }
                Text(FStrings.t("ios.msg.sendhint"))
                    .font(FType.title(15)).fontWeight(.heavy)
                    .fixedSize()
                    // Centred on the arrow, CLAMPED to the screen: the axis sits
                    // `sendHintCenterFromScreenTrailing` (42) from the screen's
                    // right edge, so the caption may extend at most 38pt right of
                    // it (a 4pt screen margin). A caption wider than centring
                    // allows shifts left to hug the edge instead of running off it
                    // (the ru caption did exactly that).
                    .alignmentGuide(.sendAxis) { d in
                        max(d[HorizontalAlignment.center],
                            d.width - (MessageTableView.sendHintCenterFromScreenTrailing - 4))
                    }
            }
            .offset(y: -lift)
        }
        .foregroundColor(sendBlue)
        .accessibilityHidden(true)   // decorative; the staged state already reads via Undo
    }
}

/// Round-9: the reminder's LIFECYCLE, shared by every surface that stages a
/// sendable bubble (the board's moves AND the lobby's join/invite/start - the
/// owner: "the send arrow should show up for all things where you stage and can
/// send"). Owns the 3-second fuse and the fade: `staged` restarts the fuse
/// whenever the staged state flips (send/undo hides it at once); `visible`
/// gates rendering to the collapsed view without disturbing the fuse.
/// `centerFromTrailing` is where the send-button axis sits measured from THIS
/// container's trailing edge (the board is inset 8 from the screen, the lobby
/// overlay is full-bleed). Purely decorative: it never eats a tap.
struct StagedSendHint: View {
    /// Re-render this view when a setting changes (see FPrefs). Only the
    /// OBSERVATION matters - the strings still come from FStrings.t and the
    /// table surface still comes from FTextures.
    @ObservedObject private var prefs = FPrefs.shared
    let staged: Bool
    let visible: Bool
    var centerFromTrailing: CGFloat = MessageTableView.sendHintCenterFromScreenTrailing
    @State private var shown = false

    var body: some View {
        let on = shown && visible
        SendHintReminder(paused: !on)
            .alignmentGuide(.trailing) { d in
                d[HorizontalAlignment.sendAxis] + centerFromTrailing
            }
            // Where the arrow rests, and so how high the crest reaches: round 16
            // pulls the crest up out of this container and into the drawer's top
            // margin, toward Messages' own Send button. See
            // SendHintReminder.crestRoom for why it is smaller than the travel.
            .padding(.top, SendHintReminder.crestRoom)
            .opacity(on ? 1 : 0)
            // Round-10 #2 ("fade it out"): BOTH directions animate - the old
            // one-way withAnimation faded it in but let a style change snap it
            // off (or on) instantly. Scoped to `on` so nothing else rides it.
            .animation(.easeInOut(duration: 0.35), value: on)
            .allowsHitTesting(false)
            .task(id: staged) {
                guard staged else { shown = false; return }
                try? await Task.sleep(nanoseconds: 3_000_000_000)
                guard !Task.isCancelled, staged else { return }
                shown = true
            }
    }
}

/// THE ink every role mark is drawn in: a WHITE body with a BLACK outline.
///
/// The three marks used to be three different colour schemes - a near-black
/// sword that flipped to steel in dark mode, a mid-gray shield with a darker
/// edge, a saturated green check - so at a glance the board carried three
/// unrelated objects, and each one had to fight the weave on its own terms
/// (the sword's near-black vanished on the walnut wool, which is why it had a
/// dark-mode special case at all). White-on-black is the one pairing that
/// carries on BOTH weaves without a per-scheme branch: the white body is the
/// silhouette, the black outline is what separates it from a light table.
///
/// Owner, this round: "Bigger sword and shield and good icons. Maybe unify
/// them to white fill + black stroke to stand out?"
enum FRoleInk {
    static let fill = Color.white
    static let line = Color(hex: 0x101014)
    /// Outline weight, in GRID units (each mark draws on the same 24x24 grid and
    /// scales by `size / 24`), so the outline thickens with the mark instead of
    /// turning into a hairline at 40pt and a blob at 20pt.
    static let stroke: CGFloat = 1.6
    /// The "said good" green. Lives here beside the shared ink so the one mark
    /// that is NOT white is still declared in the same place as the rest.
    static let good = Color(hex: 0x2E9E4F)
}

/// How big each role mark is drawn, everywhere it is drawn (the board's own
/// `selfRoleIndicator` and every opponent's `FSeatBadge.roleRow`).
///
/// Owner, this round: "Bigger sword and shield and good icons" - each up ~25%
/// on the round-5/7 numbers (check 20/22 -> 26, shield 26 -> 33, sword 32 -> 40).
/// The SWORD stays the largest of the three on purpose: it draws on the shared
/// 24x24 grid and is then rotated 45 degrees, so its blade spans only ~70% of
/// the box it is given, and a sword and a shield at the same nominal size do
/// not read the same size on screen.
enum FRoleMark {
    static let check: CGFloat = 26
    static let shield: CGFloat = 33
    static let sword: CGFloat = 40
    /// A role row must be at least this tall or it clips the sword's corners.
    static let rowHeight: CGFloat = sword
}

/// The first-attacker sword — a hand-built UPRIGHT sword on a 24x24 grid that
/// actually reads as a sword: a pointed blade, a wide crossguard, a grip, and a
/// round pommel. Marks "you open this bout".
///
/// Drawn as ONE closed outline rather than four filled pieces: overlapping
/// filled parts each carrying their own stroke would draw internal seams where
/// the blade meets the guard, which at these sizes reads as a crack down the
/// middle of the sword.
struct FSword: View {
    var size: CGFloat = 24
    var body: some View {
        Canvas { ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            var sword = Path()
            sword.move(to: P(12, 1.2))            // tip
            sword.addLine(to: P(13.6, 5.5))       // right edge of the blade
            sword.addLine(to: P(13.6, 14.3))
            sword.addLine(to: P(18, 14.3))        // right arm of the crossguard
            sword.addLine(to: P(18, 16.6))
            sword.addLine(to: P(13.1, 16.6))
            sword.addLine(to: P(13.1, 19.6))      // grip, right side
            sword.addLine(to: P(10.9, 19.6))
            sword.addLine(to: P(10.9, 16.6))      // grip, left side
            sword.addLine(to: P(6, 16.6))         // left arm of the crossguard
            sword.addLine(to: P(6, 14.3))
            sword.addLine(to: P(10.4, 14.3))
            sword.addLine(to: P(10.4, 5.5))       // left edge of the blade
            sword.closeSubpath()
            ctx.fill(sword, with: .color(FRoleInk.fill))
            ctx.stroke(sword, with: .color(FRoleInk.line),
                       style: StrokeStyle(lineWidth: FRoleInk.stroke * s, lineJoin: .round))
            // Pommel: a round knob at the base of the grip, drawn last so its
            // own outline sits on top of the grip's.
            let r = 2.0 * s
            let knob = Path(ellipseIn: CGRect(x: 12 * s - r, y: 20.6 * s - r,
                                              width: 2 * r, height: 2 * r))
            ctx.fill(knob, with: .color(FRoleInk.fill))
            ctx.stroke(knob, with: .color(FRoleInk.line),
                       style: StrokeStyle(lineWidth: FRoleInk.stroke * s))
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

/// The defender shield — a hand-built heraldic shield in the shared role ink.
///
/// Round-5 m4 asked for pointed upper corners; round-7 settled the silhouette on
/// the heater / crusader shield below (a raised point at the top centre, rounded
/// shoulders as the widest span, curving to a point at the bottom). This round
/// only changes what it is PAINTED in: the old mid-gray-on-darker-gray became
/// white-on-black with the sword and the check (FRoleInk).
struct FShield: View {
    var size: CGFloat = 24
    var body: some View {
        Canvas { ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            var shield = Path()
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
            ctx.fill(shield, with: .color(FRoleInk.fill))
            ctx.stroke(shield, with: .color(FRoleInk.line),
                       style: StrokeStyle(lineWidth: FRoleInk.stroke * s, lineJoin: .round))
        }
        .frame(width: size, height: size)
        // Round-5 m2: was a hard-coded English literal (see FSword's).
        .accessibilityLabel(Text(FStrings.t("ios.a11y.defending")))
    }
}

/// The "said good" mark — a hand-built check on the same 24x24 grid as
/// FSword/FShield. Hand-built for the same reason: SF Symbols (previously
/// `checkmark.seal.fill`) are unreliable under ImageRenderer bubble snapshots.
///
/// A check is a STROKE, not a filled body, so "white fill + black stroke" is
/// drawn here as two passes of the same path: a fat black one, then a thinner
/// white one on top. That is the same silhouette-plus-outline the other two
/// marks get, achieved the only way an open path can.
struct FCheck: View {
    var size: CGFloat = 24
    /// The check's body colour, GREEN by default.
    ///
    /// Deliberately NOT the shared white the sword and shield wear. A round-12
    /// pass unified all three on white and the owner pulled the check back out:
    /// "Keep it green but add a distinct stroke like the other type." Which is
    /// right - the sword and the shield say WHICH ROLE YOU HAVE and want to read
    /// as one family, while a check says something happened, and green is what
    /// carries that at a glance. What it takes from the other two is the BLACK
    /// RIM, so it still looks drawn by the same hand.
    var tint: Color = FRoleInk.good
    var body: some View {
        Canvas { ctx, sz in
            let s = sz.width / 24
            func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * s, y: y * s) }
            var check = Path()
            check.move(to: P(4, 12.5))
            check.addLine(to: P(9.5, 18.5))
            check.addLine(to: P(20, 5))
            // Outline first, body second. The widths differ by 2x the outline
            // weight so the black shows as an even rim on both sides of the
            // white, exactly like the closed marks' 1.6-unit stroke.
            ctx.stroke(check, with: .color(FRoleInk.line),
                       style: StrokeStyle(lineWidth: (3.4 + 2 * FRoleInk.stroke) * s,
                                          lineCap: .round, lineJoin: .round))
            ctx.stroke(check, with: .color(tint),
                       style: StrokeStyle(lineWidth: 3.4 * s, lineCap: .round, lineJoin: .round))
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
                .onTableText()
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
