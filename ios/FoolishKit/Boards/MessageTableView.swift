// MessageTableView — the INTERACTIVE expanded bubble (design §10). Same tap
// grammar as the app's TableView (tap a card to attack, select-then-tap-a-battle
// to cover, the wooden bar for pass/pickup/done) but driven by a
// MessageTurnController: a turn here STAGES a chain rather than committing to a
// live game, and the human presses Send (§11.4), never the code.
//
// Every enable state is the kernel's legal menu (`controller.legal`), never a
// hand-rolled "is it my turn" (§17.16). When my seat has no legal move I am a
// spectator on someone else's staged bubble — read-only, with a hint who is up.
//
// WHAT IS LEFT IN THIS FILE, AND WHERE THE REST WENT.
//
// This is the board's own declaration: its state, its `body`, the layout of the
// table itself (`table` / `boardContent`) and the collapse fraction that layout
// reads. Everything else lives in a `MessageTableView+<subject>.swift` beside
// it, named for what the code is ABOUT rather than for what kind of thing it is
// - the veil, the table and its battles, my hand, the seats on the ring, the
// role marks, the drag, playing a move, undo, the action pills, the end of a
// bout, one animated sequence, the open replay, the rig's auto-player, and the
// oracles the board keeps about itself. A file named for its subject stays
// correct as the code grows; one named for its layer collects whatever is
// passing, which is how this file reached 5,847 lines in the first place.
//
// THE STATE BELOW IS THEREFORE `internal`, NOT `private`. Swift scopes `private`
// to the FILE, and the board's own extensions are no longer in it. Nothing here
// is API - none of it is `public`, so the widening is from "this file" to "this
// framework" and no further - and it costs none of the rules each piece
// carries: `ledger` is still the only way to write what the badges show, and
// `ShownLedger` still refuses a write that does not name its claim.

import SwiftUI
import Foundation

public struct MessageTableView: View {
    @ObservedObject var controller: MessageTurnController
    /// Re-render this view when a setting changes (see FPrefs). Only the
    /// OBSERVATION matters - the strings still come from FStrings.t and the
    /// table surface still comes from FTextures.
    @ObservedObject var prefs = FPrefs.shared
    /// Seal the staged chain and hand it to the extension to compose + insert.
    /// The view never touches MSMessage; it only produces the payload. The `Bool`
    /// is `fromUndo`: a fresh move drops the player at Messages' Send (the drawer
    /// collapses), but an UNDO re-stages only to refresh the input bubble and must
    /// KEEP the expanded board up - undoing means "let me pick a different move",
    /// so collapsing the screen out from under them is wrong.
    let onSend: (Data, _ fromUndo: Bool) async -> Void
    /// Start a fresh game in this thread. Offered on the board only once the game
    /// is over (the fool is decided) — any player, out or not, can deal the next
    /// one. Routes through the host's New game (§5.2), same as the chrome button.
    let onNewGame: () -> Void
    /// Retract a bubble already staged with the host (§10 undo). Undo alone can't
    /// do this: rebuilding the base + replaying `pending` minus the last action is
    /// a LOCAL replay, but the host (harness `staged`, or the real extension's
    /// already-inserted input-field bubble) is a separate piece of state that only
    /// the host can clear. Called when an undo empties `pending` entirely.
    let onUnstage: () -> Void
    /// Round-9: the SURFACE staged a sendable bubble for this game outside the
    /// controller's own pending list - the one board case is the starter's LIVE
    /// handoff (startGame stages the deal; the new controller has nothing
    /// pending, but the bubble still needs Messages' Send). Feeds the send
    /// reminder alongside `controller.canSend`.
    let alsoStaged: Bool
    /// Bumped by the host every time the human deletes the staged bubble out of
    /// the input field (`MessagesViewController.didCancelSending`). A TOKEN and
    /// not a flag because a cancel is an EVENT with no resting state - the
    /// second cancel of a session looks exactly like the first, and a Bool that
    /// went true and back would either be missed or fire twice.
    let cancelToken: Int
    /// Round 12: hold the gear for 5 seconds to raise the last-message dump.
    /// The dump's fields live on the SURFACE (it owns the payload bytes and the
    /// decode result), so the board only reports the gesture; see
    /// `MessagesRootView.diagnosticPanel`.
    /// Leave the extension for a URL. An app extension has no `UIApplication`,
    /// so the only way out is the host's `extensionContext.open` - which is why
    /// this is a closure from above rather than an `@Environment(\.openURL)`.
    let onOpenURL: (URL) async -> Bool

    @State var selection: Set<String> = []
    @State var toast: String?
    // 1.0(4): the left Settings/Help squares present these.
    @State var showSettings = false
    @State var showRules = false
    // Drag-to-play state (frames published by FBattleGrid/FHandFan in `boardSpace`).
    @State var battleFrames: [Int: CGRect] = [:]
    @State var handFrame: CGRect = .zero
    @State var dragCard: Card?
    /// notes 33/34: the drag's live point in `boardSpace`, kept (FHandFan
    /// already delivers it on every `onDragChanged`, previously discarded)
    /// so the verb hint and the pass ghost-slot preview can resolve the SAME
    /// drop target `onDragEnded` will use. nil whenever no drag is active.
    @State var dragPoint: CGPoint?
    /// The dragged card's own LIVE visual centre in `boardSpace`, as reported by
    /// FHandFan's `onDragCardMoved` on every `onDragChanged`. Round-5 finding 5
    /// used it to re-centre `dragHint` HORIZONTALLY on the card; round-6 bug 5
    /// anchors the pill to it on BOTH axes (a fingertip is not where the card
    /// is), and round-6 bug 13 reads it one last time at release as the point a
    /// played card flies FROM. nil whenever no drag is active (set and cleared
    /// alongside `dragPoint`, in `onDragChanged`/`onDragEnded`).
    @State var dragCardCenter: CGPoint?
    /// A pass preview was shown at some point in the drag under way - so the
    /// finger crossing a pair is on its way to the slot. See `PassSlot`.
    @State var passSeenThisDrag = false
    /// The table's pair count when a PASS was released, until the play is
    /// over - the slot stays until the pair that fills it has landed.
    @State var passHeldAt: Int?
    @Environment(\.accessibilityReduceMotion) var reduceMotion
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
    // (round 43: `cardNS` is gone. It was a computed `Namespace.ID?` returning
    // nil, and every call site passed it. Round 8 established that this board
    // has no matchedGeometry namespace and must not have one - the overlay owns
    // every flight, so a shared namespace would DOUBLE-animate and the
    // cross-fade it produces is an opacity animation on a card, which is the
    // one thing this game never does. That is still true; what is gone is the
    // ceremony of passing nil to say so. A board with no namespace argument
    // says it more plainly than a nil-valued one, and it cannot be handed a
    // real namespace by accident. See `BoardDrag.FlightID` for the invariant
    // that replaced the per-card guard.)
    // Overlay flights to the discard pile (bout end), where matchedGeometry has no
    // target view to match against.
    @StateObject var animator = BoardAnimator()
    @State var lastView: GameView?
    @State var discardFrame: CGRect = .zero
    /// The most recent NON-empty battle rects, so a bout-end flight still has the
    /// source positions after the table cleared (preference vs onChange can race).
    @State var lastBattleFrames: [Int: CGRect] = [:]
    /// Round-6 bug 6: the most recent NON-empty battle LAYOUT (which card sat in
    /// which slot), captured alongside `lastBattleFrames`, so a discard sweep can
    /// fly each trashed card FROM ITS OWN battle rect instead of every card
    /// sharing one centroid (which read as a single stack sliding to the pile).
    /// Keyed by the same battle index as `lastBattleFrames`.
    @State var lastBattles: [BattleView] = []
    /// Round-7 #2: each battle CARD's real on-table rect (identity -> rect),
    /// kept at its last non-empty value so a bout-end discard sweep can fly each
    /// trashed card from exactly where it sat, not a shared table centroid.
    @State var lastBattleCardFrames: [String: CGRect] = [:]
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
    ///  - open-replay: `controller.openReplayPreBattles` (the kernel's pre-bout
    ///    table, since that table is never otherwise rendered on open).
    /// Set as the sequence begins, cleared as it ends.
    @State var sweepBattles: [BattleView] = []

    /// Swept cards whose flight has STARTED - hidden in the pre-bout grid from that
    /// instant on (the overlay ghost is now the only copy). Grows through the
    /// sequence, cleared with `sweepBattles`. Kept separate from `animator.hidden`
    /// because that set also hides the card's HAND copy (a pickup card lives in
    /// both places); the table copy must stay VISIBLE until its own flight, which
    /// only this set governs.
    @State var sweptFlownIds: Set<String> = []
    /// A move of mine is between the tap and the kernel's answer - see
    /// `ActionPillSlot.holdsWhilePlaying`.
    @State var playInFlight = false
    /// ROUND 20: cards that are ON the pre-bout grid but have NOT ARRIVED YET -
    /// the mirror image of `sweptFlownIds`, hidden for the same reason at the
    /// other end of the sequence.
    ///
    /// The case is a cover that ENDED the bout, watched by anyone but the player
    /// who made it (owner: "on the last cover for a set, you need to show the
    /// cover animation, then pause then sweep. I wasn't seeing the cover
    /// animation on a replay. In the replay, the cards just showed as covered,
    /// then went to discard"). The final board has no table at all - it was
    /// swept - so the replay's table IS the pre-bout grid, and that grid comes
    /// out of the kernel with the cover already lying on it. Rendering it
    /// straight away is exactly the report: the covered pair is simply THERE on
    /// the first paint, and the only motion left to watch is the sweep.
    ///
    /// So the cover starts absent from the grid, flies to its slot on the grid
    /// (`openReplayFlights` falls back to the sweep table when the final view
    /// has no battles), lands, holds for `boutEndHold`, and only then sweeps.
    /// Kept apart from `sweptFlownIds` because the two mean opposite things and
    /// the debug trace is worth being able to tell them apart.
    ///
    /// ROUND 45 TRIED TO MERGE THEM AND FOUND A SECOND, HARDER REASON. The two
    /// are unioned into one set for the grid (`Veil.grid`), so on that side alone
    /// they could be one - seed it with the un-arrived cards, subtract on
    /// arrival, union on takeoff. But this set has a consumer the other does
    /// not: `openReplayFlights` reads it as "the whole gate" for whether a card
    /// is flying ONTO the pre-bout grid, and `sweepArriving` is derived by
    /// intersecting a step's flights with it. Merged, a card that has already
    /// been carried OFF the table would test true on both - it would be built a
    /// second flight, landing on a grid it just left, and the attack under it
    /// would tilt as it went. The union is a coincidence of what the grid needs
    /// to hide, not evidence that the two facts are one.
    @State var sweepUnplaced: Set<String> = []
    /// The subset of `sweepUnplaced` whose flight is in the air THIS INSTANT, so
    /// the attack underneath a cover tilts in lockstep with the card coming down
    /// on it (`FBattleGrid.coverTilted` reads exactly this pair of sets). The
    /// sweep grid otherwise passes an empty `flyingNow` - during the sweep every
    /// card is leaving, and there is nothing left to tilt onto.
    @State var sweepArriving: Set<String> = []
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
    ///
    /// That rejection is about THIS vector only. The `.transaction` still on
    /// `actionBar`'s fixed-size container is not a leftover of it - it covers a
    /// change `.animation(nil, value:)` cannot see at all. The three overrides
    /// and what each one is for are laid out once, in
    /// `doesNotRideTheBoardSpring`'s doc at the foot of this file.
    @State var buttonLift: CGFloat = -1
    /// `lift` for the promoted status mark (the `selfRoleIndicator` overlay),
    /// which is drawn OUTSIDE `boardContent`'s GeometryReader and so cannot read
    /// the local. `buttonLift` is the mirror the chrome already rides; before it
    /// has been measured (-1, the first paint) fall back to the same arithmetic
    /// it will settle on, off the hand's published width, so the mark does not
    /// spend a frame at the bottom of the board and then jump up.
    private var statusMarkLift: CGFloat {
        if buttonLift >= 0 { return buttonLift }
        let hand = HandLayout.fanCards(controller.view?.me?.hand ?? [], holding: fanHoldback)
        guard handFrame.width > 0 else { return 0 }
        return FHandFan.height(cards: hand, availableWidth: handFrame.width)
    }
    @State var deckFrame: CGRect = .zero
    /// MY CARDS THAT THIS OPEN-REPLAY HAS NOT FLOWN OUT OF MY HAND YET.
    ///
    /// An open replay renders the FINAL board (`controller.view`), so a bubble
    /// carrying my own attack opens with those cards already gone from my hand
    /// and the fan already re-centred - and the flight then had nowhere in the
    /// hand to start from. Owner, 1.0(41): "When I replay an attack of mine, it
    /// seems to animate from where my 'player card count' would be… notice that
    /// there is no self player mini-hand visual. Thus what I'm seeing is that
    /// they spawn in like behind the cards in my hand, then fly in to their
    /// correct positions."
    ///
    /// This is the counts trick (`ledger.hand`) applied to the hand: hold
    /// the cards in the fan at their PRE-MOVE slots, seeded synchronously by
    /// `replayLastMoveOnOpen`, and drop each group the instant its flight is
    /// built. The fan animates its own re-close over exactly `flightTime`
    /// (FHandFan's layout animation is keyed on the laid-out SET), so the hand
    /// closes up as the cards leave rather than before they do - the second
    /// half of the same report.
    ///
    /// Held-back cards are NOT in the kernel hand, and every play path filters
    /// through `view.me.hand` (`selectedCards`) or the legal menu, so one can
    /// never be played. Round 42 left it at that - "the worst a tap on one can
    /// do is a reject toast". ROUND 43: that was wrong twice over. `toggle` wrote
    /// the identity into `selection` regardless, where it stayed for the life of
    /// the board and silently disabled Take and Good (both gate on
    /// `cards.isEmpty`) the moment the card came home in a pickup; and dragging
    /// one raised a "move not allowed" toast about a card the player had already
    /// played. So they are now `locked` in the fan (gesture off, appearance
    /// untouched - see `hand`) and `selectionAfterTap` keeps `selection` inside
    /// the kernel hand by construction.
    ///
    /// ARMED HERE, ANSWERED THROUGH `fanHoldback`. Nothing that renders may read
    /// this property directly - see that one for why.
    @State var handHoldback: [Card] = []
    /// ROUND 43: WHICH VEIL THE HOLDBACK BELONGS TO - `animator.veilEpoch` as it
    /// stood when it was armed, and the exact mirror of `clearPreHidden(raisedBy:)`.
    ///
    /// The holdback used to be cleared in two places only, both inside
    /// `runEventStream`. Every other sequence that can SUPERSEDE a replay -
    /// `flyUndoReturn`, `flyUndoRelease`, the genesis-deal fallback - left it
    /// standing, so a holdback whose stream never got to tear down stayed in the
    /// fan for good: cards you have already played, drawn in your hand, and (the
    /// `veilStandingNow` oracle's complaint) still `preHidden` at rest. Every
    /// other veil in this file has an unconditional rescue; this one had none.
    ///
    /// The epoch is what makes the rescue safe to add in three more places. A
    /// teardown may only take down a holdback IT could have raised: a newer
    /// sequence armed its own after a `preHide` that bumped the epoch, so an
    /// older teardown finds `handHoldbackAt > veiledAt` and leaves it alone.
    /// That is the same hazard the newest-sequence check at `runEventStream`'s
    /// teardown was written for, stated in the units that actually order these
    /// two events.
    @State var handHoldbackAt = 0
    @State var handCardFrames: [String: CGRect] = [:]
    @State var seatFrames: [Int: CGRect] = [:]

    /// WHAT THE BADGES ARE SHOWING - the deck, the discard, every seat's hand
    /// count, who is drawn as OUT and which mark each seat is wearing. All five
    /// LAG the game state during a sequence: a count holds its old value until
    /// that card's flight lands, then bumps (so a hand count never jumps before
    /// the deck→player draw animation plays), and a mark stays where it is until
    /// the move that moved it has been watched.
    ///
    /// ROUND 44: ONE VALUE, WRITTEN ONE WAY. The five used to be five separate
    /// pieces of `@State` with twenty-odd assignments between them, and the rule
    /// about who may make one was a `guard` line copy-pasted into three
    /// functions - which is exactly how round 43 came to find a fourth writer
    /// that undid round 42's fix one line after it applied. They now live behind
    /// `ShownLedger`, in a file this one cannot assign into, and every write
    /// names its claim: see ShownLedger.swift, which is where the whole rule and
    /// its history is written down.
    @State var ledger = ShownLedger()
    @State var roleFlights: [RoleFlight] = []
    @State var roleProgress: Double = 0
    /// Seats whose own mark is in the air - the take-off ends. They blank
    /// instantly: the ghost IS that mark now (round 20 split what used to be one
    /// `roleFlyingSeats` set, because the two ends of a flight no longer behave
    /// the same - see `FRoleCoin`).
    @State var roleDepartingSeats: Set<Int> = []
    /// Seats a mark is flying TO. They turn their own mark away as the ghost
    /// arrives, so it lands ON something rather than into a gap.
    @State var roleArrivingSeats: Set<Int> = []
    /// Where each seat's mark sits, published by the badges and by my own
    /// indicator - the take-off and landing pads.
    @State var roleMarkFrames: [Int: CGRect] = [:]
    /// Claims the overlay, exactly like `animSequenceToken` claims the animator:
    /// a newer hand-off must not have its ghosts cleared by an older one's
    /// teardown.
    @State var roleFlightToken = 0
    /// note 39: the board keeps its stage until whatever's animating (a
    /// bout-end sequence, or an open-delta replay) visibly finishes — the
    /// end screen only swaps in once this flips. Starts false; `.task` resets
    /// it defensively in case this view ever survives a controller swap
    /// (structurally it doesn't today — GameSurface always nils `controller`
    /// before assigning a new one — but nothing here should rely on that).
    @State var showResults = false
    /// note 17: a cover that's about to empty the defender's hand ends the
    /// bout in the SAME kernel apply as the cover itself, so
    /// `flyBoutEndToDiscard` never sees an intermediate "covered" table to
    /// animate from — the card would just vanish straight to the discard
    /// pile. Stashed by `playAt` right before `controller.apply`, consumed
    /// (and always cleared) by the very next `flyBoutEndToDiscard` call.
    struct PendingCover {
        let cards: [Card]
        /// The battle slot EACH covering card lands on, keyed by card identity.
        ///
        /// One rect PER CARD, because a cover can answer several attacks at once
        /// (the kernel's `calc_cover_moves_greedy` emits exactly one such move,
        /// and the Cover button plays it): `move.cards[i]` covers
        /// `move.attackCards[i]`, positionally. This was a single `battleRect`
        /// - the slot the GESTURE named - and every card of a multicover flew at
        /// that one slot, which is what the owner saw as "all three cards
        /// animate towards a single attack card".
        let landing: [String: CGRect]
        let fromRects: [String: CGRect]   // card.identity -> hand rect AT PLAY TIME
        /// The hand's OWN frame at that same instant - the yardstick those rects
        /// were measured against, so a board that has since resized can put them
        /// back where they meant. See `rebased`.
        let handFrameAtPlay: CGRect
    }
    @State var pendingCover: PendingCover?
    /// Round-6 bug 13 ("when we drag a card then let go, it should animate from
    /// where we let go to the table, not back from its original position in hand
    /// to the table"): the cards a play of MINE just put on the table, and the
    /// rect each one LEFT FROM - the release point for the card the finger was
    /// actually holding, its resting hand slot for the rest of a multi-card
    /// selection (they never moved) and for a play made by tapping. Captured by
    /// `playAt` BEFORE `controller.apply`, since by the time the new view lands
    /// the hand has already closed over the gap. Consumed by the very next
    /// `flyBoutEndToDiscard`, exactly like `pendingCover` above.
    struct PendingPlacement {
        let cards: [Card]
        let fromRects: [String: CGRect]   // card.identity -> where it left from
        /// The hand's own frame at that same instant - see `rebased`.
        let handFrameAtPlay: CGRect
    }
    @State var pendingPlacement: PendingPlacement?
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
    var settled: Bool { !controller.replayPending }
    /// The last `controller.arrivalTick` this board has reacted to. When it
    /// falls behind, the next view change is a bubble that ARRIVED rather than a
    /// move made here, and it is played like a cold open (see
    /// `flyBoutEndToDiscard`).
    @State var seenArrivalTick = 0
    /// My hand as it was the instant I played a move, captured SYNCHRONOUSLY in
    /// `play` — before `controller.apply`, so before the view can publish. Any
    /// card in my hand that is not in here is one this move just gave me
    /// (a pickup's table cards, a bout-end refill), and it stays veiled until
    /// the animator hides it for its flight. nil when no move of mine is
    /// in flight. The live-play twin of the open-replay veil above.
    @State var handBeforeMyMove: Set<String>?
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
    @State var animSequenceToken = 0

    /// CLAIM THE ANIMATOR: bump the token, and keep the number that came back.
    ///
    /// The two statements only mean anything together - the bump says
    /// "everything already running is stale", and the number is the receipt
    /// every later guard in that sequence checks itself against
    /// (`mySeq == animSequenceToken`) - and they were written out longhand at
    /// five sites, where a claim that bumped and forgot to keep the number
    /// would look exactly like a claim that did.
    ///
    /// `@discardableResult` because one site genuinely has no receipt to keep:
    /// `drainSupersededForReversal` supersedes whatever is running and then
    /// starts no sequence of its own, so there is nothing later to compare.
    /// Discarding it there is a decision, and it says so in a comment.
    ///
    /// DELIBERATELY NOT A WRAPPER ROUND THE TEARDOWNS TOO. The four claims that
    /// do start a sequence end in four genuinely different ways - one hands
    /// orphaned hand slots forward and deposits into the reversal ledger, one
    /// reveals a specific id set, one drops the swept table as well, one
    /// rescues a holdback - and a helper that pretended they were one shape
    /// would hide differences that are real. Only the claim is shared, because
    /// only the claim is the same everywhere.
    @discardableResult
    func claimAnimSequence() -> Int {
        animSequenceToken += 1
        return animSequenceToken
    }

    /// Cards a SUPERSEDED sequence had already opened a hand slot for and never
    /// got to fly - laid out, opacity 0, and belonging to nobody.
    ///
    /// `openSlots` moves a card OUT of `preHidden` and leaves it in `hidden`, so
    /// `clearPreHidden` - the blanket net every teardown runs - cannot reach it;
    /// only the teardown's own `openedThisSeq` rescue can, and that teardown is
    /// skipped when a newer sequence has taken the animator. So an arrival that
    /// lands mid-flight used to strand every card the running sequence had
    /// opened, invisibly, for the life of the board: a deal that "just doesn't
    /// animate" and leaves five cards showing against a full deck, a cover that
    /// flies and then vanishes, and - because FBattleGrid reads
    /// `hidden \ preHidden` as "in flight right now" - the attack it covered
    /// left sitting untilted, as though never covered at all. Worst on the moves
    /// with the longest sequences to interrupt, which is a round transition.
    /// (Owner, testing 1.0(17), on a live compact drawer.)
    ///
    /// Handed FORWARD instead of released on the spot, because the sequence that
    /// took over may have pre-hidden cards of its own that it has not flown yet -
    /// revealing those is the bug-9 double animation. Whoever finishes last
    /// rescues whatever is still hidden.
    @State var orphanedOpens: Set<String> = []

    /// THE CONFLICT MODEL's ledger (docs/ANIMATION_CATALOGUE.md, 1.0(28)): the
    /// motions a SUPERSEDED sequence had already flown, deposited by its
    /// teardown for the arrival that superseded it to REVERSE - red, in reverse
    /// group order, verdict-filtered (see ConflictModel.swift) - before the
    /// arriving chain animates forward. Deposited only while
    /// `reversalCollecting` is up, i.e. only when an ARRIVAL is standing by to
    /// consume it: a live move of my own taking the animator over is not a
    /// conflict (my move is the newest truth on this device) and its supersede
    /// keeps today's plain hand-off, so a deposit nobody would consume must not
    /// sit here going stale until some later arrival flies it back long after
    /// the board moved on.
    @State var reversalDebt: [[FlownMotion]] = []
    @State var reversalCollecting = false
    /// Claims the whole ARRIVAL PIPELINE (drain, reversal, forward replay) the
    /// way `animSequenceToken` claims the animator: bumped synchronously by
    /// every arrival, checked between the pipeline's phases. This is what makes
    /// a burst "undo whatever is animating, then play ONLY the last" - an
    /// arrival whose epoch has been superseded hands its collected debt on and
    /// never starts its forward replay, so the intermediate boards are not
    /// replayed one by one (explicitly NOT a queue - the owner's burst rule).
    @State var arrivalEpoch = 0

    public init(controller: MessageTurnController, onSend: @escaping (Data, Bool) async -> Void,
                onNewGame: @escaping () -> Void = {}, onUnstage: @escaping () -> Void = {},
                alsoStaged: Bool = false, cancelToken: Int = 0,
                onOpenURL: @escaping (URL) async -> Bool = { _ in false }) {
        self.cancelToken = cancelToken
        self.onOpenURL = onOpenURL
        self.controller = controller
        self.onSend = onSend
        self.onNewGame = onNewGame
        self.onUnstage = onUnstage
        self.alsoStaged = alsoStaged
    }

    /// NOBODY'S SEAT. Round 21: a spectator watches this board (owner:
    /// "spectators opening final move goes straight to rank board, no final
    /// animation. We should show the final move still"), and a spectator holds
    /// no seat - `mySeat` is -1.
    ///
    /// It has to be asked explicitly wherever a SEAT is compared against an
    /// EVENT's seat, because the kernel spends -1 on "no particular player" too:
    /// a discard and a bout transition both carry seat -1, so a seatless viewer
    /// tested with `ev.seat == mySeat` would claim them as their own move and
    /// route the discard sweep into a hand that does not exist. Comparing
    /// against a PLAYER's seat is safe either way - no player is seated at -1 -
    /// and those sites are left alone.
    var isSpectating: Bool { controller.mySeat < 0 }

    /// THE RESULTS SCREEN HAS TAKEN THE BOARD'S PLACE.
    ///
    /// ONE predicate with two kinds of reader, and that is the whole point of
    /// its existing. The swap itself happens INSIDE the VStack below - the
    /// board branch is replaced by `FGameOverList` - but the overlays that
    /// decorate the board are hung on the VStack, OUTSIDE that branch, so they
    /// are not swapped away with it. Anything in an overlay that belongs to the
    /// live board has to ask this question for itself.
    ///
    /// It went wrong exactly once, and instructively: `selfRoleIndicator` is an
    /// overlay, and its doc comment asserted that the game-over screen "replaces
    /// the whole board" so the mark could never be reached once the game ended.
    /// It replaces the board's CONTENT, not the overlay's host - so a finished
    /// 4p game showed a lone shield floating over empty felt, ~55% down, with no
    /// table under it (caught in an App Store screenshot). Two sites spelling
    /// `controller.isOver && showResults` separately is what let them drift;
    /// reading the same property is what stops them drifting again.
    private var showsEndScreen: Bool {
        Self.showsEndScreen(isOver: controller.isOver, showResults: showResults)
    }

    /// The end-screen predicate as a value, so it can be tested without a
    /// rendered board (`MessageTableView` needs a live controller and a host to
    /// draw at all, and a SwiftUI overlay has no assertable identity from a
    /// test). Every reader goes through `showsEndScreen`.
    static func showsEndScreen(isOver: Bool, showResults: Bool) -> Bool {
        isOver && showResults
    }

    public var body: some View {
        table
            // THE TABLE DOES NOT MIRROR, in any language.
            //
            // Arabic and Hebrew arrived with the 25-language table, and SwiftUI's
            // answer to them is to flip every container it owns. That is right
            // for a column of text and wrong for this board, because the board is
            // not laid out in containers: seats, cards in flight and the fan are
            // placed with `.position` and `.offset`, which SwiftUI does NOT
            // mirror, while the deck well, the discard and a few paddings ARE
            // expressed as leading/trailing and would flip. The result is not a
            // mirrored table, it is a half-mirrored one - cards where they were,
            // the furniture swapped around them.
            //
            // So the geometry is pinned and the TEXT is left alone: names, the
            // caption, the rejection line and every label inside still shape and
            // read right-to-left, because bidi is a property of the run, not of
            // the container. An Arabic player gets Arabic on a board that sits
            // where the rules say it sits.
            //
            // The surfaces that are pure text are deliberately NOT pinned - the
            // rulebook, the settings sheet and the lobby mirror as they should.
            // Mirroring the board is a real piece of work; this is the honest
            // half of it, not a stand-in for it.
            .environment(\.layoutDirection, Self.layoutDirection)
    }

    /// The pin above, named so `LocalizationTests` can assert it is still there.
    /// A constant and not a computed answer: there is no language for which this
    /// is allowed to differ today, and the day there is, it should be a visible
    /// change here rather than a quiet one.
    public static let layoutDirection: LayoutDirection = .leftToRight

    private var table: some View {
        VStack(spacing: 8) {
            if let view = controller.view {
                // Game over: the board gives way to the ranked results screen (web
                // WinScreen parity) - finish order first-out down to the fool, with
                // New game there. Gated on `showResults` too (note 39): the board
                // stays the stage until the last flight (a bout-end sequence, or an
                // open-delta replay of someone else's final move) has visibly
                // landed, so the end screen never just cuts in mid-animation.
                if showsEndScreen {
                    // NEW GAME WAITS FOR THE SEND. Owner, 1.0(27): "i dont think
                    // you should be able to send the game end move and then have
                    // the new game button, because that will kinda obscure the
                    // ranking... if you end the game, do not show the new game
                    // button until it gets sent."
                    //
                    // A rematch opens a fresh chain, and the ending move is
                    // still sitting unsent in the input field where the next
                    // stage would REPLACE it - so the bubble that tells everyone
                    // else the game is over would be swapped out for a lobby,
                    // and the ranks on this screen would be the only place the
                    // result ever existed. `pending` rather than `canSend`
                    // because the send window (`sending`, between the tap and
                    // the rebase) is still unsent: the button may not blink into
                    // existence in the middle of it.
                    FGameOverList(rows: finishRows(view), onNewGame: onNewGame,
                                  showNewGame: controller.pending.isEmpty && !alsoStaged,
                                  replayURL: controller.replayURL, onOpenURL: onOpenURL)
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
        // Round-8 #3 / round-9: the staged-but-unsent reminder - a blue arrow
        // bobbing under Messages' own Send button (in the compose bar directly
        // above this view's top-right corner), with a caption in the same blue.
        // COLLAPSED VIEW ONLY (the expanded board is not under the Send button
        // at all). `alsoStaged` is the surface's own stage (the starter's LIVE
        // handoff renders a board with nothing pending, but its bubble still
        // needs sending). Fuse + fade live inside StagedSendHint.
        //
        // OUTSIDE THE BRANCH, not inside the board. Owner, 1.0(27): "send hint
        // arrow needs to pop up for the game ending move too - right now i see
        // the finishing ranks and no send arrow." The move that ENDS the game
        // is staged like any other and still has to be sent, but it is also the
        // move that swaps the board for `FGameOverList` - and the hint lived in
        // the branch that had just been swapped away, so the one bubble whose
        // send nobody can guess from the screen was the one bubble with no
        // arrow over it.
        //
        // As an overlay on the VStack it keeps the geometry it had inside the
        // board's ZStack (same container, same top-trailing corner, hence the
        // same `sendHintCenterFromTrailing`), and it keeps its IDENTITY across
        // the board -> results swap, so the 3-second fuse carries on burning
        // instead of restarting the moment the ranks appear.
        //
        // The collapse fraction is measured HERE rather than taken from
        // `boardContent`'s: that one is a local inside the board's own
        // GeometryReader, and the whole point is to be outside the branch that
        // holds it. This reader wraps the same box (the board is the VStack's
        // only child, and `FGameOverList` fills it too), so it reads the same
        // height and the hint sits in the same corner either way.
        .overlay(alignment: .topTrailing) {
            GeometryReader { geo in
                StagedSendHint(staged: controller.canSend || alsoStaged,
                               visible: Self.collapseFraction(height: geo.size.height) > 0.95,
                               centerFromTrailing: Self.sendHintCenterFromTrailing)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
            }
            .doesNotRideTheBoardSpring(controller.view)
        }
        .padding(.horizontal, 8).padding(.top, 14).padding(.bottom, 4)   // top margin so the ring isn't clipped in the compact drawer
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .overlay { FlyingCardsLayer(animator: animator) }
        // A STATUS ICON IS ALWAYS IN FRONT OF A CARD.
        //
        // Owner, round 41: "when the cards on the table or hand swap from the
        // static version to the animated moving version, the z index on screen
        // changes. As a result this can cause it to go from being under a
        // checkbox to on top of a checkbox."
        //
        // Both halves of that are true and neither is wrong on its own. In the
        // ZStack a table card is drawn FIRST, so it is behind the role marks; a
        // flying card is not in the ZStack at all, it is in the overlay above
        // it, so it is in front of them. Nothing moves the card - the layer it
        // lives in changes the instant it starts moving, and the mark it was
        // behind is suddenly behind IT.
        //
        // So the marks move above the flight layer, and the ordering stops
        // depending on which copy of the card is on screen. Wrapped in the SAME
        // padding the board content carries, so this is a pure change of z: the
        // mark lands on exactly the pixel it landed on before (an overlay is
        // sized to the padded frame, the ZStack to that frame minus the
        // padding, and the difference is what this puts back).
        //
        // NOT DONE HERE, and worth knowing: the OPPONENTS' status pips live
        // inside `FSeatBadge`'s own VStack (`roleRow`), so promoting them means
        // splitting the badge in two - and the badge publishes `SeatFramesKey`
        // from inside itself, which is where every draw and pickup flight aims.
        // The self mark is the one a card actually crosses (it sits 6pt above
        // the hand, in the takeoff path of every card played), so it is the one
        // this fixes.
        .overlay {
            if let v = controller.view {
                selfRoleIndicator(v)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                    // The 6pt over the hand became `selfMarkLift`, which is
                    // `markTightening` less when the marks are tight
                    // (FSeatBadge): the owner's "bring our own slightly
                    // DOWN", by the same amount the seats' marks come up.
                    // `statusMarkLift` itself is untouched - it is the pills'
                    // line too.
                    .padding(.bottom, statusMarkLift + FSeatBadge.selfMarkLift(tight: FSeatBadge.tightMarksOn))
                    // Round-7's rule, carried over with the view.
                    .doesNotRideTheBoardSpring(controller.view)
                    .allowsHitTesting(false)
                    // The board content's own inset, re-applied INSIDE the
                    // overlay. An overlay is sized to the padded frame and the
                    // ZStack to that frame minus the padding, so without this
                    // the mark would sit 4pt lower and 8pt wider than it used
                    // to. This is a change of z and nothing else.
                    .padding(.horizontal, 8).padding(.top, 14).padding(.bottom, 4)
            }
        }
        // MY OWN CARDS, ON TOP OF THE MARK. Owner, round 47: "the badge should
        // be above FLYING CARDS, but just below OUR SELF cards."
        //
        // The mark was promoted above the flight layer in round 41 so a card in
        // the air could not hide it, and that half stands. What did not is where
        // my hand sat: inside the board's ZStack it was under BOTH, so a mark
        // 6pt above the fan drew over my own cards. Promoting the hand to an
        // overlay after the mark is the whole change - flights, then the mark,
        // then the hand.
        //
        // It costs nothing in geometry: `HandFrameKey` publishes
        // `frame(in: .named(boardSpace))` and `.coordinateSpace(name: boardSpace)`
        // is applied BELOW every one of these overlays, so the hand is still
        // inside that space and every drop target reads exactly what it read
        // before. The padding is re-applied for the same reason the mark's is -
        // an overlay is sized to the padded frame and the ZStack to that frame
        // minus the padding.
        .overlay {
            if let view = controller.view {
                ZStack {
                // Redrawn on a short timer: whether the board is still is read
                // from statics nothing publishes (see `boardStill` in actionGates).
                TimelineView(.periodic(from: .now, by: 0.1)) { _ in
                    actionBar(view)
                }
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
                    // - sat still. `.animation(nil, value:)` cannot stop that
                    // (it only covers changes driven by `controller.view`) and
                    // FActionBar's own `.transaction` sits BELOW the placement,
                    // so it cannot either.
                    //
                    // IT WRAPS THE FIXED-SIZE CONTAINER, NOT THE PLACEMENT.
                    // Round 36: it used to sit outermost, above the `lift`
                    // padding, and a `.transaction` nils the animation of
                    // EVERYTHING it passes down - including the board-level
                    // padding that carries the chrome up when the hand grows a
                    // second row. That made this one column the only piece of
                    // chrome that could not honour the owner's "at least make
                    // it slide smoothly instead of jumping" (see `buttonLift`'s
                    // mirror), because the slide was being nulled on its way in.
                    // Confined to the 128x88 container it still covers
                    // everything it was added for - the pill's insertion and its
                    // position WITHIN the column, which is the whole of the
                    // 1.0(10g) film - and nothing it was not.
                    //
                    // KEEP IT. It is vector 2 of the three in
                    // `doesNotRideTheBoardSpring`'s doc, not a weaker spelling
                    // of the `.doesNotRideTheBoardSpring` just below it.
                    .transaction { $0.animation = nil }
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                    .padding(.trailing, ActionPillSlot.outerInset).padding(.bottom, statusMarkLift + 4)
                    .doesNotRideTheBoardSpring(controller.view)

                undoSlot
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                    // The SAME inset as every other pill - see ActionPillSlot for
                    // the 4pt round 10g put here and the owner's measurement.
                    .padding(.trailing, ActionPillSlot.undoTrailing(aligned: ActionPillSlot.aligned))
                    .padding(.bottom, statusMarkLift + 4)
                    .doesNotRideTheBoardSpring(controller.view)

                settingsHelpBar
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomLeading)
                    .padding(.leading, ActionPillSlot.outerInset).padding(.bottom, statusMarkLift + 4)
                    .doesNotRideTheBoardSpring(controller.view)

                    hand(view, reserveNoSlot: handSlotDeferred)
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                }
                .padding(.horizontal, 8).padding(.top, 14).padding(.bottom, 4)
            }
        }
        // Above the cards: a role changing hands IS the thing being read at that
        // moment (it happens after the sweep and the deal, when nothing else is
        // moving), and a shield disappearing behind a badge would read as a
        // glitch rather than as depth.
        .overlay { RoleFlightsLayer(flights: roleFlights, progress: roleProgress) }
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
        .onPreferenceChange(RoleMarkFramesKey.self) { fr in
            // Merged, never replaced: my own indicator and the opponent badges
            // publish into the same key from different branches of the tree, and
            // a seat that has gone out stops publishing entirely - but the mark
            // it last wore may still be mid-flight from that pad.
            roleMarkFrames.merge(fr) { _, new in new }
        }
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
                let rects = FHandFan.slotRects(cards: display, width: handFrame.width)

                var worst = 0.0, worstId = ""
                for c in hand {
                    // Placed by the SAME rule the flights use (`inBoardSpace`,
                    // anchored on the fan's bottom edge). It used to offset by
                    // `handFrame.minY`, which made this check disagree with
                    // every flight it was supposed to be validating - and made
                    // it report a full row of error (86pt at eleven cards)
                    // whenever the two preferences were a paint out of step,
                    // which is noise, not drift. What is worth shouting about
                    // is the two disagreeing about where the hand IS, and that
                    // survives this change: the 391pt collapse gap still reads.
                    guard let a = rects[c.identity].map({
                              Self.inBoardSpace($0, laidOutCount: display.count,
                                                handFrame: handFrame) }),
                          let m = $0[c.identity] else { continue }
                    let d = max(abs(a.midX - m.midX), abs(a.midY - m.midY))
                    if d > worst { worst = d; worstId = c.identity }
                }
                if worst > 2 {
                    AnimLog.say("SLOTCHECK MISMATCH n=\(hand.count) worst=\(String(format: "%.1f", worst))pt @\(worstId)")
                }
                // …AND INTO THE DUMP THE OWNER CAN ACTUALLY READ, once it is
                // past half a card. That threshold is the line between two
                // completely different facts, and only the magnitude separates
                // them: a few points is `handCardFrames` being one layout pass
                // behind something that is still moving, which is harmless and
                // constant during a drag; a whole board is the hand's per-card
                // frames NOT RE-PUBLISHING AT ALL when the drawer resizes.
                //
                // The second is real and measured. On a 667 board collapsing to
                // 261, the rig reads `SLOTCHECK MISMATCH n=7 worst=406.0pt` -
                // and 406 is exactly 667 - 261. `HandFrameKey` re-publishes
                // across that resize and `HandCardFramesKey` does not, which is
                // why the analytical route (`handSlotsNow`, off `handFrame`)
                // reads the new drawer correctly while anything trusting the
                // per-card frames is still describing the expanded board.
                //
                // Everything load-bearing has been moved off them - a takeoff
                // and a landing are both computed now - so this is a WATCH, not
                // a live defect. It is here because this check named the bug in
                // the owner's own Debug build for months and only ever said so
                // to a log nobody was streaming.
                if worst > 35 {
                    FlightRecorder.note("slotcheck", "hand rects \(Int(worst))pt off - stale after a resize?")
                }
            }
            #endif
        }
        .onChange(of: controller.view) { v in
            let sequenced = flyBoutEndToDiscard(to: v)
            // Round 16: a move with no sequence of its own still moves the
            // roles - a PASS hands the shield along mid-bout, and that is the
            // one hand-off nothing else here would animate. A sequence syncs
            // its own roles at the end, once its cards have landed.
            if !sequenced, let v { syncRoles(to: RoleState(v), in: v, animated: true) }
        }
        .fFlash($toast)
        .onChange(of: controller.rejectTick) { _ in
            // 1.0(4): say WHY, from the kernel's reason code, as a plain white
            // flash (no pill).
            Haptics.fire(.reject); toast = FStrings.rejectReason(controller.lastRejectReason)
            // A rejected move publishes NO view change, so the veil `play` put
            // up a moment ago would never be taken down again: the counts would
            // stay frozen at their pre-move values for the rest of the game and
            // any card the move would have added stay invisible. Nothing
            // happened, so give it all straight back - including (round-6 bug
            // 13) the card `playAt` hid on its way to the table, which would
            // otherwise be missing from the hand it never left.
            //
            // ROUND 40 moved the body into `releaseLivePlayVeil`, because a
            // rejection is not the only way a play comes to nothing and the two
            // paths must give back the SAME things.
            releaseLivePlayVeil()
        }
        .task {
            AnimLog.say("board .task seat=\(controller.mySeat) ready=\(controller.ready)")
            // THE CONFLICT MODEL needs to know a board is mounted: an arrival
            // over a staged move is only DEFERRED behind a red retraction when
            // there is a board to fly it (offerArrival). Balanced by
            // .onDisappear below, which also flushes any latched arrival a
            // teardown would otherwise strand behind the failsafe.
            controller.setBoardWatching(true)
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
            // FoolishHarness only: auto-play a move (see the function).
            await autoPlayIfAsked()
            #endif
        }
        .onDisappear { controller.setBoardWatching(false) }
        // THE HUMAN DELETED THE STAGED BUBBLE (didCancelSending, via the host's
        // `cancelToken`). Routed into the SAME undo the pill runs - see
        // `cancelStagedBubble` - so the two can never drift about what a
        // retracted move does to the game.
        .onChange(of: cancelToken) { _ in cancelStagedBubble() }
        #if DEBUG
        // …and again after an ARRIVAL. Folding a chain in keeps this board's
        // identity (round 12), so the mount `.task` above never fires a second
        // time - which meant the rig could only ever stage a move on the first
        // board it mounted, and "somebody moves, then I reply and send" was
        // unreachable. That is the sequence both 1.0(23) reports describe.
        .task(id: controller.arrivalTick) {
            guard controller.arrivalTick > 0 else { return }   // the mount case, handled above
            await autoPlayIfAsked(waitForBoard: true)
        }
        #endif
        // 1.0(4): the left Settings/Help squares present these.
        .sheet(isPresented: $showSettings) {
            MessageSettingsView { showSettings = false }
        }
        .sheet(isPresented: $showRules) {
            // The rulebook teaches THIS table's game: a podkidnoy chain gets a
            // page with no passing in it (RulesView.passing).
            RulesView(passing: controller.passingAllowed) { showRules = false }
        }
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
            // The board's live collapse fraction (0 = expanded, 1 = fully
            // compact drawer) is NOT bound here. Round-11 narrowed what may
            // read it to two things - the send hint's visibility and the
            // opponent ring's radius - and both now ask
            // `Self.collapseFraction` at their own call site, so a binding here
            // was left over reading as "the board has a collapse fraction in
            // hand", which is exactly the invitation round 11 was closing.
            // Nothing that positions the hand or the chrome may read it at all;
            // see `handCrop`.
            let myHand = view.me?.hand ?? []
            // The width FHandFan itself actually lays out in: this reader's
            // width minus `hand(_:)`'s own `.padding(.horizontal, FSpace.s)`.
            let handWidth = max(0, geo.size.width - FSpace.s * 2)
            // Bug 10: the cards the fan actually LAYS OUT right now — the whole
            // hand minus any deal still deferring its slot (see
            // `handSlotDeferred`). Room is reserved off THIS, so an incoming
            // card's width is not reserved until its own flight opens the slot.
            let deferredSlots = handSlotDeferred
            // How many cards the fan actually LAYS OUT this paint - the number
            // its row split is really taken on, as opposed to `myHand.count`
            // which is what the chrome reserves against. The two are equal at
            // rest and only differ mid-sequence; see the `fan-rows` note below,
            // which exists to say which of them moved when the split flips.
            //
            // ROUND 43: THROUGH `fanCards`, like everything else that describes
            // the laid-out hand. This read the kernel hand alone, so the one row
            // change round 42 introduced - the drop when `handHoldback` lets go
            // of my replayed cards - was invisible to the very trace that exists
            // to answer "we had ten cards and they said good, why did the hand
            // change rows?". `handHeight` two lines down was already measured
            // off `fanCards`, so the watch and the thing being watched had come
            // apart.
            let laidHandCount = HandLayout.laidCount(hand: myHand, holding: fanHoldback,
                                                     deferred: deferredSlots)
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
            // …and `myHand` PLUS whatever an open replay is still holding back,
            // which is the hand the fan is really laying out (`fanCards`). Round
            // 7's rule is untouched by this: it is about cards ARRIVING one at a
            // time, and a holdback is a single settled set from the first paint
            // that empties in one step, so the chrome sits still through the
            // replay and drops once, with the hand, rather than floating up
            // card by card. Empty everywhere else, so nothing else moves.
            let handHeight = FHandFan.height(cards: HandLayout.fanCards(myHand, holding: fanHoldback),
                                             availableWidth: handWidth)

            // THE CHROME'S LIFT IS NOT A LOCAL HERE ANY MORE. It was
            // `buttonLift < 0 ? handHeight : buttonLift`, read by the action
            // column, the undo slot, the settings squares and the self role
            // mark while all four were children of this ZStack. Rounds 41 and
            // 47 moved every one of them out into overlays above the flight
            // layer, and those are drawn OUTSIDE this GeometryReader - so they
            // ask `statusMarkLift`, which is the same two-branch rule (the
            // mirror once it has been measured, the hand's own height before
            // that) written where they can reach it. The local went on being
            // computed for two builds with nobody left to read it.
            //
            // The mirror itself is untouched and still the point: it rides
            // `.onChange` below so the chrome's movement is its own animation
            // and never the board's card spring (see `buttonLift`).
            // HOW MUCH OF THE SLIDE A VIEW TAKES BACK OFF ITSELF.
            //
            // The collapse's layer animation pushes the whole board down so the
            // box's BOTTOM edge never moves (CollapseLayer's file note has the
            // measurements). Everything anchored to that edge - the hand, the
            // pills - wants all of it and is left alone. Everything else has to
            // undo it, and NOT by the same amount: a view sitting a fraction
            // `f` down the box moves `1 - f` of what the box's top edge moves,
            // so that is what it takes back.
            //
            //   f = 0    deck, discard    the whole slide
            //   f = 0.5  the battle       half of it
            //   f = 1    the hand         none
            //
            // Subtracting the whole slide from all of them is what sent the
            // table cards off the top of the screen and back - measured, the
            // blue bar on the cards scored a jerk of 73,121 against the hand's
            // 19, and at the flip it sat at 510 - 535 = -25pt, i.e. above the
            // phone. The deck and the discard were the only ones that looked
            // right, and only because f = 0 is the case the old code happened
            // to be correct for.
            //
            // And it is taken back ON A LAYER, not as an offset. As a SwiftUI
            // offset it was a smooth value minus a frame-stale one, and the
            // table cards scored a jerk of 4420 against the hand's 26 - the
            // judder had moved, not gone. `collapseLayer` hosts each of these
            // views on a layer of its own that carries its share of the motion
            // at the composite rate, beside the hosting layer's.
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
                    // BEFORE the fill, not after. `.frame(maxWidth:.infinity)`
                    // and `.position()` both expand a view to the whole box, so
                    // a mark applied after either one centres on the BOX - which
                    // put this bar and the opponent's on the same line, with one
                    // painted over the other, and neither of them on the cards
                    // they are supposed to be reporting.
                    .collapseMark(.table)
                    // Also before the fill: the layer is the grid's own box, and
                    // the frames it relays are the grid's cards.
                    .collapseLayer(fraction: 0.5,
                                   relaying: [BattleFramesKey.self, BattleCardFramesKey.self])
                    .collapseMarkLift()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)

                // Opponent ring — each seat placed by trig on a 35% ellipse. The
                // local player is visual-index 0 (bottom edge) and is drawn as the
                // hand, so it is skipped here.
                ForEach(view.players.filter { $0.seat != controller.mySeat }) { p in
                    // ONE bar, on the first opponent only: two of them in the
                    // same colour would be two candidate rows and the reader
                    // takes the first, so a second would silently decide which
                    // seat is being measured.
                    // ONE bar, on the first opponent only: two of them in the
                    // same colour would be two candidate rows and the reader
                    // takes the first, so a second would silently decide which
                    // seat is being measured.
                    opponentSeat(p, view)
                        .collapseMark(p.seat == view.players.first(where: {
                            $0.seat != controller.mySeat })?.seat ? .opponent : nil)
                        // The ring's OWN rule, not a fraction of it: the radius
                        // opens as the box shrinks, and the layer walks that.
                        .collapseLayer(ride: .path(rest: geo.size.height) { h in
                                           ringPoint(seat: p.seat, n: view.players.count,
                                                     in: CGSize(width: geo.size.width, height: h)).y
                                       },
                                       relaying: [SeatFramesKey.self, RoleMarkFramesKey.self])
                        .collapseMarkLift()
                        .position(ringPoint(seat: p.seat, n: view.players.count, in: geo.size))
                }

                // Deck top-left, discard top-right — pinned to the corners and OUT
                // of the centred flow, so they never push the ring or battles.
                // Both halves of the stock come off the SAME three sources in
                // the same order (`shownDeckCount` / `shownTrumpSlot`); the
                // trump used to be read straight off `view` while the count was
                // held, which is 1.1(55)'s missing flipped card.
                let trump = shownTrumpSlot(view)
                FDeckWell(deckCount: shownDeckCount(view), flipped: trump.card,
                          hasFlipped: trump.exists, trumpSuit: view.trumpSuit)
                    .collapseLayer(fraction: 0, relaying: [DeckFrameKey.self])
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
                    .collapseLayer(fraction: 0, relaying: [DiscardFrameKey.self])
                    .offset(y: -3)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)

                // NO SELF ROLE INDICATOR HERE. It used to be the next child of
                // this ZStack, at `lift + 6`; round 41 ("a status icon is always
                // in front of a card") moved it OUT, into the `selfRoleIndicator`
                // overlay above the flight layer, and it reads `statusMarkLift`
                // there. What round 41 left behind was an empty `Color.clear` at
                // the same padding, kept "so the ZStack order below is
                // unchanged" - which was never a thing that could change: a
                // transparent, non-hit-testing child draws nothing, and dropping
                // a child does not reorder the ones after it. It is gone, and
                // with it the round-7 animation note that described a mark this
                // slot had not drawn for two builds. Round-7's rule itself is
                // alive and spent by every piece of chrome below, as
                // `.doesNotRideTheBoardSpring` - read its doc before touching
                // any of the three overrides down there, they are three
                // different vectors and not three tries at one.

                // Action buttons float bottom-right, above the hand (web absolute
                // bottom:90/right:20). They only appear when a flag enables them.
                // THE ACTION PILLS, UNDO AND THE SETTINGS SQUARES ARE NOT
                // DRAWN HERE ANY MORE - they moved into the overlay beside my
                // hand. Owner, round 47: "the other player fans are BELOW our
                // cards but ABOVE the buttons. Looks strange."
                //
                // They were after the opponent ring in this stack and therefore
                // above it, which is what the board does when it draws itself.
                // Under the collapse's slide the ring's seats are each hosted in
                // a UIHostingController of their own (CollapseLayer), and a
                // hosted view is a real UIView whose compositing does not take
                // its order from this ZStack - so the badges came out over the
                // buttons and under my cards, which is the one order nobody
                // asked for. Promoted to sit with the hand they are above both,
                // and the cards, the pills and the squares share a level. They
                // never collide, so sharing one is free.

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


                // MY HAND IS NOT DRAWN HERE ANY MORE - see the overlay beside
                // `selfRoleIndicator`. It hugged the bottom in this ZStack, which
                // put it under the flight layer and therefore under the status
                // mark as well, and the owner's order is flying cards, then the
                // mark, then my own cards on top.

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
            // Mirror the hand height out to `buttonLift` so the chrome moves on
            // ITS OWN animation instead of floating on the board's card spring.
            // Since round-11 this only ever fires on a ROW-COUNT change - the
            // drawer height no longer enters it - so the log line is the whole
            // story of why the chrome ever moves.
            //
            // AND IT SLIDES NOW, IT DOES NOT JUMP. Round-7's rule was "buttons
            // should NEVER move / float", and the mirror delivered that by
            // running in `.onChange`'s own transaction, which carries no ambient
            // animation - so `buttonLift` snapped. That was the right cure for
            // the wrong half of the problem: what the owner objected to was the
            // chrome DRIFTING on the card spring during motion that had nothing
            // to do with it, not the chrome arriving at a new resting place. The
            // hand growing a second row genuinely does move the chrome, and
            // round 36 asked for that move to be honest: "if you're going to
            // make the settings/rules/action button move up for the double-row,
            // at least make it slide smoothly instead of jumping."
            //
            // So it gets an EXPLICIT animation of its own rather than the
            // ambient one. `FMotion.chrome` is the board's 150ms ease-out for
            // exactly this class of thing, and being explicit is what keeps the
            // round-7 fix intact: the only change that can ever animate this
            // value is a row-count change arriving through here. Every other
            // path into the chrome's position is still `.animation(nil, value:
            // controller.view)`, so a card spring still cannot reach it.
            // ROUND 36 DIAGNOSTIC - WHY DID THE HAND CHANGE ROWS?
            //
            // Owner, on an opponent's `good` landing while holding ten cards:
            // "the cards started to animate towards the two row layout, then
            // like changed their mind mid layout transition and went back to
            // the skinny card one row layout. Why the fuck did that happen? We
            // had ten cards and they said good! There was no way our card count
            // was going to change!!!" - and the same shape again on an
            // opponent's triple cover, which likewise cannot touch my hand.
            //
            // The row split is a pure function of exactly two numbers: how many
            // cards the fan LAYS OUT, and how wide it is
            // (`FHandFan.rowCount`). So a split that flips and flips back is one
            // of those two moving, and reading the code cannot say which - both
            // have a plausible route. The laid-out count is the kernel hand
            // minus whatever the veil is still deferring (`handSlotDeferred`),
            // which a replay drives; the width is the board's own geometry,
            // which the host drives when it resizes the drawer.
            //
            // So the board says which, in the trail the owner can pull off the
            // device with the gear hold. Both counts and the width, on every
            // change of the laid-out row count - not on every paint, which
            // would drown the trail in a value that is constant almost always.
            // Deliberately a `FlightRecorder.note` rather than an `AnimLog.say`:
            // AnimLog is DEBUG-only and this has to survive a shipping build,
            // which is the only kind the report came from.
            // Fires on the LAID-OUT COUNT, not on the row count: below the split
            // threshold the same churn only makes the cards thinner ("normal one
            // row to skinny card view"), which is the same defect and would not
            // have tripped a row-count watch at all.
            .onChange(of: laidHandCount) { _ in
                // READ LIVE, NOT FROM THE CAPTURED BODY LOCALS. `.onChange`'s
                // action is the closure from the LATEST body evaluation, which
                // can be a paint after the change that fired it - so a captured
                // `laidHandCount` describes a different moment than the trigger
                // does, and the first version of this line printed "2 rows" next
                // to a count that needs one. A trace nobody can read is worse
                // than none, because it invites a wrong conclusion.
                let hand = controller.view?.me?.hand ?? []
                let deferred = handSlotDeferred
                // Round 43: the same live re-read as before, through the same
                // `fanCards` the trigger above uses. Recomputing it WITHOUT the
                // holdback (which is what this did) meant the line could
                // disagree with its own trigger - fire on a change it then
                // printed no evidence of. `held=` is new for the same reason:
                // when the split flips, this says which of the three inputs
                // moved.
                let held = fanHoldback
                let laid = HandLayout.laidCount(hand: hand, holding: held, deferred: deferred)
                let w = handFrame.width
                let line = "\(FHandFan.rowCount(count: laid, availableWidth: w)) rows"
                    + " laid=\(laid) hand=\(hand.count) held=\(held.count) width=\(Int(w))"
                    + " deferred=\(deferred.count) veiled=\(veiledCardIds.count)"
                    + " preHidden=\(animator.preHidden.count) hidden=\(animator.hidden.count)"
                    + " settled=\(settled) seq=\(BoardAnimator.sequenceDepth)"
                FlightRecorder.note("fan-rows", line)
                // AnimLog too, so the RIG can read this off the unified log
                // (`log stream --predicate 'subsystem == "cards.foolish.anim"'`)
                // without pulling a device trail. The FlightRecorder line is for
                // the owner's phone; this one is for reproducing it here.
                AnimLog.say("fan-rows \(line)")
            }
            .onChange(of: handHeight) {
                let h = $0
                AnimLog.say("handHeight \(Int(buttonLift))->\(Int(h)) cards=\(myHand.count) rows=\(FHandFan.rowCount(cards: myHand, availableWidth: handWidth))")
                // The FIRST measurement is a placement, not a movement: sliding
                // the chrome up from an unmeasured -1 would be the whole board
                // assembling itself in front of the player on every open.
                if buttonLift < 0 { buttonLift = h }
                else { withAnimation(FMotion.chrome) { buttonLift = h } }
            }
            .onAppear { buttonLift = handHeight }
        }
    }

    // (round 43: `handCrop` is gone. It was a `static let ... = 0` and every
    // geometry call passed it. Round 11 pinned it at 0 after measuring what a
    // live crop cost - the hand landing 31pt lower, the chrome drifting the
    // other way, and both ringing across a collapse the host does not ramp
    // monotonically. FHandFan carries that finding now, beside the parameter it
    // justified removing.)

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
