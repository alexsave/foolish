// MessagesRootView — what the extension shows, per presentation style (§10).
//
// Compact is the KEYBOARD AREA (§3.5): no text field, no horizontal scrollers —
// so it is a label and buttons. Expanded is the table. The routing here is the
// §5/§6/§7 machine wearing a UI: a selected bubble is decoded + adopted, my seat
// is resolved (§6), and I either play (MessageTableView, staging a reply) or,
// when three-plus players leave my seat ambiguous, pick who I am (§6.3). New game
// opens a lobby where I am seat 0 (§5.2/lobby v3, docs/IMESSAGE_LOBBY_V3.md) —
// every chat shape, DM included, locks its seed at create and deals nobody in
// until Start.
//
// No Durak rule is answered in this file — MessageTurnController relays the kernel
// and MessageComposer only stages. Seat identity is the one non-kernel decision,
// and it is SeatIdentity's pure §6 logic, fed the conversation's `senderIsLocal`.
import SwiftUI

/// The extension's two presentation states, decoupled from the Messages
/// framework so this view compiles into FoolishKit and is drivable by both the
/// real `MessagesViewController` (which maps `MSMessagesAppPresentationStyle`
/// onto it) AND the FoolishHarness test app (§ harness). Nothing here imports
/// `Messages`.
public enum MsgPresentation { case compact, expanded }

/// Round-10d: the collapse arm signal, delivered WITHOUT re-presenting.
/// The host used to bump a token and call present() to hand it over, but that
/// rebuilds MessagesRootView, and the routing it re-runs can resolve a
/// different payloadURL - which changes GameSurface's loadKey and reloads the
/// whole board. Filmed: four frames of bare wool and a spinner in the middle
/// of the collapse. An ObservableObject bumped in place re-renders only what
/// observes it, so the live board is untouched.
public final class CollapseSignal: ObservableObject {
    @Published public var token = 0
    public init() {}
}


public struct MessagesRootView: View {
    /// Re-render this view when a setting changes (see FPrefs). Only the
    /// OBSERVATION matters - the strings still come from FStrings.t and the
    /// table surface still comes from FTextures.
    @ObservedObject private var prefs = FPrefs.shared
    let payloadURL: URL?
    /// The presentation style of the present() call that BUILT this view. Kept
    /// for host-API symmetry, but round-10 stopped gating any visual on it:
    /// present() only runs on discrete host events (activate, receive, send,
    /// New game), so across a grabber drag or an auto-collapse this value goes
    /// STALE - it is how the send reminder leaked into the expanded lobby.
    /// Anything that must know how tall the drawer is reads its own live
    /// geometry instead (see `stageHeight` and MessageTableView's
    /// `collapseFraction`).
    let style: MsgPresentation
    let senderIsLocal: Bool
    let startNewGame: Bool
    /// Bumped by the host each time the human taps New game, so an explicit New
    /// game resets the session while a mere compact<->expanded toggle does not.
    let newGameToken: Int
    /// Bumped by the host (MessagesViewController.didStartSending) each time the
    /// human actually SENDS a staged bubble. Threaded down so the live board's
    /// controller can drop the just-sent move from its in-memory pending
    /// (`markSent`) - otherwise the Undo button lingers in the collapsed view and
    /// re-stages an already-sent move (round-6 bug 4). Unlike newGameToken it must
    /// NOT change loadKey: a send re-presents the SAME game, so the board is
    /// signalled (via .onChange) rather than reloaded.
    let sentToken: Int
    /// …and the BYTES that went out with it. ROUND 16: a send no longer closes
    /// the compact drawer, so the live controller has to be rebased onto its own
    /// just-sent chain (`markSent(payload:)`) instead of being rebuilt from it by
    /// a teardown. nil only where a host cannot name them (the harness, an older
    /// call site), which degrades to the pre-round-16 signal.
    let sentPayload: Data?
    /// The bubble that just ARRIVED while the extension is open (`didReceive`),
    /// with `incomingToken` bumped per arrival. Apple does not move
    /// `selectedMessage` for an arrival, so without this the surface sat on its
    /// stale chain until the human happened to re-tap a bubble - which is how a
    /// player stranded on a losing Start fork stayed stranded (the 4-player
    /// double-Start deadlock; see GameSurface.maybeAdoptIncoming). Rule P still
    /// decides: a stale or duplicate arrival changes nothing on screen.
    let incomingURL: URL?
    let incomingToken: Int
    /// This conversation's identity (`ChatKey.make` over its participant set),
    /// threaded down to every `MessageGameStore` lookup so a game cached from a
    /// DIFFERENT chat on this device can never resolve `.known` here — see the
    /// chat-scoping fix in `MessageGameStore`'s type doc.
    let chatKey: String
    let chatIsDM: Bool
    let chatPlayers: Int
    let requestExpand: () -> Void
    let onNewGame: () -> Void
    /// Start a NEW MSSession for whatever is staged next, WITHOUT the teardown
    /// `onNewGame` does. The rematch path needs exactly this half: its first
    /// bubble must not collapse the finished game's result card (see
    /// MessagesViewController's session note), but it has no name to ask for
    /// and no surface to rebuild.
    let onFreshChain: () -> Void
    /// Name the player who just left, for the bubble about to be staged. The
    /// envelope cannot say it - the join carrying the name is exactly what the
    /// leave removed - so the one device that still knows tells the host, which
    /// writes it into the transcript line. Cleared once staged.
    let onAnnounceLeave: (String) -> Void
    let onSend: (Data, Int, Bool) async -> Void
    /// Retract a previously-staged bubble (§10 undo). No-op by default so every
    /// existing caller keeps compiling; the real extension has no API to remove an
    /// inserted input-field bubble, so it can only drop its own pending-stage record.
    let onUnstage: () -> Void
    /// Leave the extension for a URL (the finished game's Replay Link). An app
    /// extension has no `UIApplication`, so the host passes its
    /// `extensionContext.open` down; the no-op default keeps the harness and
    /// previews, which have nowhere to go, compiling and inert.
    let onOpenURL: (URL) async -> Bool

    /// Round-9: bumped by the host (didCancelSending) when the human deletes
    /// the staged bubble from the input field - the surface drops its own
    /// staged-unsent flag so the send reminder doesn't point at a bubble that
    /// no longer exists.
    let cancelToken: Int

    /// Round-10c: bumped by the host right before it requests the compact
    /// style for the post-stage auto-collapse. The surface responds by
    /// animating ITSELF down to the last compact height it has seen - the
    /// board visibly packs into the bottom of the still-expanded drawer under
    /// OUR animation (hand pinned, full control). Only then does the host run
    /// requestPresentationStyle(.compact): whatever snapshot games Messages
    /// plays for that transition (ruler-instrumented films proved the
    /// mid-flight imagery is snapshot compositing our live view can't
    /// influence), the two endpoints' visible bottom strips are now pixel-
    /// identical and everything above is featureless wool - nothing left on
    /// screen that can visibly jump.
    @ObservedObject var collapseSignal: CollapseSignal

    public init(payloadURL: URL?, style: MsgPresentation, senderIsLocal: Bool,
                startNewGame: Bool, newGameToken: Int = 0, sentToken: Int = 0,
                sentPayload: Data? = nil, chatKey: String,
                chatIsDM: Bool, chatPlayers: Int,
                incomingURL: URL? = nil, incomingToken: Int = 0, cancelToken: Int = 0,
                collapseSignal: CollapseSignal = CollapseSignal(),
                requestExpand: @escaping () -> Void, onNewGame: @escaping () -> Void,
                onFreshChain: @escaping () -> Void = {},
                onAnnounceLeave: @escaping (String) -> Void = { _ in },
                onSend: @escaping (Data, Int, Bool) async -> Void,
                onUnstage: @escaping () -> Void = {},
                onOpenURL: @escaping (URL) async -> Bool = { _ in false }) {
        self.payloadURL = payloadURL; self.style = style; self.senderIsLocal = senderIsLocal
        self.startNewGame = startNewGame; self.newGameToken = newGameToken; self.sentToken = sentToken
        self.sentPayload = sentPayload
        self.chatKey = chatKey; self.chatIsDM = chatIsDM; self.chatPlayers = chatPlayers
        self.incomingURL = incomingURL; self.incomingToken = incomingToken
        self.cancelToken = cancelToken; self.collapseSignal = collapseSignal
        self.requestExpand = requestExpand; self.onNewGame = onNewGame
        self.onFreshChain = onFreshChain; self.onAnnounceLeave = onAnnounceLeave
        self.onSend = onSend
        self.onUnstage = onUnstage
        self.onOpenURL = onOpenURL
    }

    /// Round-10 #1: the height the surface is actually LAID OUT against.
    /// A manual grabber drag feeds this view a fresh height every frame, and the
    /// board - a continuous function of height since round 6 - tweens smoothly.
    /// An ANIMATED style change (the auto-collapse after a move, tapping a
    /// bubble to expand) does not: Messages sets the hosting view's model height
    /// to the TARGET in one step and animates only the visible drawer frame, so
    /// the content snapped straight to its compact layout while the drawer was
    /// still tall (filmed frame-by-frame: everything "jumps up at once, THEN
    /// starts collapsing", the host's flat fallback brown filling the vacated
    /// strip). The fix: follow small steps exactly (the manual drag), and TWEEN
    /// through a big one, reproducing in SwiftUI the same intermediate heights a
    /// manual swipe would have delivered. 0 until the first real height lands.
    ///
    /// Round-10b postscript: the residual "self cards dip under the screen"
    /// during the auto-collapse turned out NOT to be this view's geometry at
    /// all - a debug ruler drawn on the surface proved the flying rect in the
    /// films was the just-inserted STAGED BUBBLE's snapshot (public table, no
    /// hand) that Messages animates into the compose slot OVER the drawer.
    /// Fixed at the source: MessagesViewController.stage() now collapses
    /// FIRST and inserts the bubble after the transition settles.
    @State private var stageHeight: CGFloat = 0

    /// Round-10c's `lastCompactHeight` and `extentHold` are GONE (round 22):
    /// both belonged to the pre-collapse pack that round 10d removed, and both
    /// had been write-only ever since - state that is set on every layout pass
    /// and read by nothing reads as a live input when the next person changes
    /// this. The compact threshold moved to `CollapseTween`, which is now where
    /// the whole rule lives.

    /// Round-10d: the box's height while the collapse tween runs; 0 = follow
    /// the model box exactly (every other moment, including manual drags).
    @State private var boxHeight: CGFloat = 0
    /// The previous geometry height, to spot the collapse flip's down-snap.
    @State private var lastGeoHeight: CGFloat = 0
    /// Where the collapse tween is currently headed. Meaningless unless
    /// `collapsing`; a later, taller report re-points it (see `follow`).
    @State private var collapseTarget: CGFloat = 0
    /// Set by the host right before it requests .compact, consumed by the
    /// first down-snap - see `follow`.
    @State private var armed = false
    /// The drawer height at the moment the host armed us - i.e. the EXPANDED
    /// height, captured before any of the transition's noisy reports arrive.
    /// The collapse tween starts here.
    @State private var armedFrom: CGFloat = 0
    /// Round-10c: true from the pre-collapse until well after the transition.
    /// While set, the packed box is TOP-anchored in the wool extent - which is
    /// exactly where the host's collapse compositing expects it. Filmed
    /// mechanics: the collapse renders the (already compact) model pinned to
    /// the drawer's DESCENDING top edge, so a box packed at the top is
    /// continuous through the flip and simply rides the shrink down into the
    /// compact rest under the host's own animation. (A bottom-packed box
    /// teleported ~400pt up at the flip; a display-link counter starved when
    /// the main thread was busiest; an edge-triggered offset mis-accumulated
    /// on the transition's NOISY geometry, which bounces through several
    /// heights in both directions - all three were filmed failing. This is
    /// level-based only.) While collapsing, `follow` also ignores any
    /// expanded-sized geometry report - those are the same transition noise.
    @State private var collapsing = false

    /// Round-10d: the auto-collapse, MEASURED (a ruler drawn on the live
    /// surface, filmed at 30fps in real Messages, bands read per frame):
    ///
    ///   rest expanded   box top  94   box bottom 838
    ///   flip frame      box top ~105  box bottom 405   <- teleport
    ///   +1..+13 frames  bottom 498, 603, 676, 728, 772, 798, 811, 815, 824, 831
    ///
    /// The host snaps our MODEL box to the compact height and renders it glued
    /// to the drawer's DESCENDING TOP edge. So a box that keeps its top on that
    /// edge is already correct at the top; what it needs is the right HEIGHT -
    /// if the box is as tall as the drawer is VISIBLE at that instant, it
    /// exactly fills the drawer: the deck rides the top edge down, and the hand,
    /// action bar and settings squares stay pinned to the screen bottom. That is
    /// the manual-swipe look, and the owner's spec.
    ///
    /// So on the collapse flip the box holds its EXPANDED height (top-anchored,
    /// still filling the drawer) and tweens down to the compact height on the
    /// host's own curve, measured above: a quartic-out over 0.45s (the bezier
    /// below tracks those ten sampled points to within a couple of points).
    /// Nothing is packed, offset or sampled - one height, one curve.
    ///
    /// The EXPAND direction is composited bottom-referenced by the host (the
    /// owner: it "works much better... cards stay at the bottom"), so up-snaps
    /// are followed instantly, exactly as before.
    /// The DECISION is `CollapseTween.step` - a pure function, so the host's
    /// noisy transition reports can be replayed as a test rather than re-filmed
    /// (CollapseTweenTests). This is the part that cannot be pure: the
    /// animation, and the timer that hands the box back to the model.
    private func follow(height: CGFloat) {
        AnimLog.say("stage follow geo=\(Int(lastGeoHeight))->\(Int(height)) armed=\(armed)")
        lastGeoHeight = height
        switch CollapseTween.step(height: height, armed: armed, armedFrom: armedFrom,
                                  collapsing: collapsing, target: collapseTarget) {
        // Consumed on the MODEL SNAP, which is when the host's own drawer
        // animation begins. (Starting on the arm signal instead was filmed
        // leading the host by ~3 frames: the box shrank while the drawer was
        // still full, i.e. the hand rose. Starting later lagged it. The snap is
        // the phase reference.) From the height captured at arm time - the true
        // expanded height, before the transition's noise - down to the snap.
        case .start(let from, let to):
            armed = false
            collapsing = true
            CollapseTween.isTweening = true
            collapseTarget = to
            boxHeight = from
            withAnimation(.timingCurve(0.165, 0.84, 0.44, 1, duration: 0.38)) {
                boxHeight = to
            }
            Task {
                try? await Task.sleep(nanoseconds: 1_200_000_000)
                collapsing = false
                CollapseTween.isTweening = false
                boxHeight = 0
            }
        // The host settled TALLER than the snap this tween started on. Ease up
        // rather than rest short of the drawer and expose the wool under it -
        // see CollapseTween for why this correction is upward only.
        case .retarget(let to):
            collapseTarget = to
            withAnimation(.easeOut(duration: 0.18)) { boxHeight = to }
        case .hold:
            break
        case .follow:
            boxHeight = 0
        }
    }

    public var body: some View {
        // ONE surface for both presentation styles — NOT a compact/expanded switch.
        // The switch made SwiftUI destroy the expanded @State (the whole in-progress
        // game) whenever you dragged to the compact drawer, so the two sizes looked
        // like two separate games (B4 bug). GameSurface is always the root's child,
        // so its game state survives a style change; it renders the SAME table in
        // both, just sized to the strip (compact) or full-screen (expanded).
        // The wool is a `.background` on the content — NOT a ZStack sibling. As a
        // sibling, `TableBackground().ignoresSafeArea()` expands the stack into the
        // safe areas and `GameSurface` (maxHeight: .infinity) fills THAT taller
        // box, so the hand fan dropped off the bottom edge (cards "barely fit", cut
        // off). As a background the wool extends behind, into the safe area via its
        // own `.ignoresSafeArea()`, WITHOUT changing GameSurface's frame — so the
        // content keeps the safe-area height the hand was laid out against and the
        // wool still paints the whole screen. The "wool too short vertically" that
        // remained was the WEAVE IMAGE itself being a fixed size shorter than a
        // tall expanded surface (TableWeave), fixed there, not here.
        GeometryReader { geo in
            GameSurface(payloadURL: payloadURL, senderIsLocal: senderIsLocal,
                        startNewGame: startNewGame, newGameToken: newGameToken, sentToken: sentToken,
                        sentPayload: sentPayload, chatKey: chatKey, chatIsDM: chatIsDM, chatPlayers: chatPlayers,
                        incomingURL: incomingURL, incomingToken: incomingToken,
                        cancelToken: cancelToken,
                        requestExpand: requestExpand, onNewGame: onNewGame,
                        onFreshChain: onFreshChain, onAnnounceLeave: onAnnounceLeave,
                        onSend: onSend,
                        onUnstage: onUnstage, onOpenURL: onOpenURL)
                // Round-10 #1: lay the surface out against the SMOOTHED height,
                // bottom-anchored - the drawer's bottom edge is the one edge
                // that never moves, so the hand stays glued to it while
                // everything above eases. Round-10c: the box sits inside a wool
                // EXTENT of max(box, model) so that during the pre-collapse -
                // when the box is deliberately SHORTER than the still-expanded
                // drawer - the weave keeps covering the whole drawer above the
                // packed-down board instead of exposing the host's flat
                // fallback colour. At rest and during transitions the two
                // heights agree and this is a no-op.
                // Round-10d: the box is the model box, except while the
                // collapse tween runs - then it holds the expanded height and
                // eases down on the host's own curve (see `follow`).
                .frame(width: geo.size.width,
                       height: boxHeight > 0 ? boxHeight : geo.size.height)
                .background(TableBackground())
                // TOP-anchored through the collapse: the host glues our content
                // to the drawer's descending top edge, so a box of the drawer's
                // visible height starting there fills it exactly. Bottom
                // otherwise (the expand is composited bottom-referenced). At
                // rest the box fills the frame and the two agree.
                .frame(maxWidth: .infinity, maxHeight: .infinity,
                       alignment: collapsing ? .top : .bottom)
                .onAppear { lastGeoHeight = geo.size.height }
                .onChange(of: geo.size.height) { follow(height: $0) }
                // The host is about to request .compact - see `follow`.
                // Round-10d: the host arms us and requests .compact in the
                // SAME runloop turn, so starting the tween here starts it in
                // lockstep with the host's own drawer animation. Starting it
                // later - when the geometry snap arrives, 2-3 frames on - left
                // the box taller than the drawer just long enough to clip the
                // hand below its bottom edge (filmed: two frames of bare wool
                // where the hand should be). This is NOT round-10c's
                // pre-collapse pack, which ran a full 0.35s BEFORE the host
                // moved at all and read as "the cards go up, then come back
                // down"; nothing here precedes the host.
                .onChange(of: collapseSignal.token) { _ in
                    armed = true
                    armedFrom = geo.size.height
                }
        }
        // Order matters: the wool is applied INSIDE this, so the keyboard opt-out
        // extends the CONTENT and the WOOL together into the bottom/keyboard
        // region - the hand never sits over a strip the wool didn't reach (the
        // "background gap at the bottom" seen after send-brings-up-the-keyboard,
        // then reopening the bubble).
        .ignoresSafeArea(.keyboard)
            // Round-5 M4/B3/M3: Dynamic Type had no POLICY at all — some
            // controls never scaled (M4), the card faces scaled straight out
            // of their own bounds (B3), and the game-over list collapsed
            // independently of both (M3). Owner's call this round: opt OUT of
            // Dynamic Type entirely rather than pick apart which of dozens of
            // small-screen surfaces can safely grow — "make a clamp so that
            // dynamic type does nothing in my game." The single-value overload
            // (not a range) pins the WHOLE hierarchy below this line to the
            // default, non-accessibility size regardless of the system
            // setting. Revisit if/when there is room to do this surface by
            // surface instead of as one blanket clamp.
            .dynamicTypeSize(.large)
    }
}

private struct GameSurface: View {
    let payloadURL: URL?
    let senderIsLocal: Bool
    let startNewGame: Bool
    let newGameToken: Int
    let sentToken: Int
    let sentPayload: Data?
    let chatKey: String
    let chatIsDM: Bool
    let chatPlayers: Int
    let incomingURL: URL?
    let incomingToken: Int
    let cancelToken: Int
    let requestExpand: () -> Void
    let onNewGame: () -> Void
    /// Start a NEW MSSession for whatever is staged next, WITHOUT the teardown
    /// `onNewGame` does. The rematch path needs exactly this half: its first
    /// bubble must not collapse the finished game's result card (see
    /// MessagesViewController's session note), but it has no name to ask for
    /// and no surface to rebuild.
    let onFreshChain: () -> Void
    /// Name the player who just left, for the bubble about to be staged. The
    /// envelope cannot say it - the join carrying the name is exactly what the
    /// leave removed - so the one device that still knows tells the host, which
    /// writes it into the transcript line. Cleared once staged.
    let onAnnounceLeave: (String) -> Void
    let onSend: (Data, Int, Bool) async -> Void
    let onUnstage: () -> Void
    let onOpenURL: (URL) async -> Bool

    /// A phase-0/handoff lobby the extension shows instead of the board (§5.2).
    private struct Lobby { let env: MessageEnvelope; let payload: Data }
    /// A resolved seat waiting on the human's name (§B3). Since lobby v3 EVERY
    /// seated player named themselves on the way in (the creator in setup, every
    /// joiner — DM opponent included — at the lobby's Join field), so this fires
    /// only on §6.2 cache-loss recovery: a reinstall or second device, where the
    /// seat resolves from an exact signal but the stored nickname is gone with
    /// the cache. Any player count. Ask once, store it, then seat them.
    private struct NameGate { let env: MessageEnvelope; let payload: Data; let seat: Int
                              var quietOpen = false }    // round-9 #5: my just-sent chain

    @State private var controller: MessageTurnController?
    /// The setup/lobby screens' Settings + Help squares present these
    /// (durak-rules-redesign) — the board keeps its own pair of flags inside
    /// MessageTableView, so the two never fight over one sheet.
    @State private var showSettings = false
    @State private var showRules = false
    @State private var ambiguous: (env: MessageEnvelope, payload: Data)?
    /// RELEASE-ONLY substitute for `ambiguous` (§6.3): an unresolved identity in
    /// Release must never offer a seat picker (anyone could claim any hand), so we
    /// show the same PUBLIC spectator board a delivered bubble's snapshot uses,
    /// instead. DEBUG keeps the real picker (single-simulator testing needs it).
    @State private var spectator: (view: GameView, names: [Int: String])?
    /// Round 20: the finished spectator board's Replay Link, captured with the
    /// board itself - see where it is set. nil unless the game is over.
    @State private var spectatorReplayURL: URL?
    /// ROUND 21: A WATCHER GETS THE REAL BOARD FOR THE LAST MOVE.
    ///
    /// The owner: "spectators opening final move goes straight to rank board, no
    /// final animation. We should show the final move still." Round 20 gave a
    /// spectator the RESULT of a finished chain and skipped how it got there,
    /// because the spectator branch draws `MessageBoardView` - a still picture of
    /// a `GameView`, with no animator in it.
    ///
    /// So a finished chain hands the watcher a `MessageTurnController` instead,
    /// seated at -1. Everything a player gets then falls out of machinery that
    /// already exists and is already tested: `begin()` resolves the kernel's
    /// evwire for the last move, the board replays it, and `settleResults` gives
    /// way to the same `FGameOverList`. Read-only by construction rather than by
    /// a flag - the kernel refuses a legal-move query for seat -1, so `legal` is
    /// empty, so `iCanAct` and `canStage` are both false, and every control on
    /// the board is gated on one of those two.
    ///
    /// PUBLIC-SAFE for the same reason the still picture was: viewer -1 is the
    /// masked view (§10), so there are no hands in it to leak - the fan simply
    /// has nothing to draw.
    ///
    /// nil while the chain is still running; that case still gets the still
    /// picture and the "spectating" caption, which is all there is to say about
    /// a game nobody here is playing.
    @State private var spectatorBoard: MessageTurnController?
    @State private var lobby: Lobby?
    @State private var nameGate: NameGate?
    @State private var showSetup = false
    @State private var toast: String?
    @State private var damaged = false
    /// 1.0(6) DIAGNOSTIC: the last decode error, shown in the on-screen dump so a
    /// message that fails to open can be captured by screenshot (temporary).
    @State private var diagError: String?
    /// 1.0(6) DIAGNOSTIC: the FULL raw FMSG envelope bytes as hex (the iMessage
    /// format itself - magic/format/flags/phase/game_id/seed/digest/joins/body -
    /// not just the replay code), and the parsed header fields when decode works.
    @State private var diagHex = ""
    @State private var diagInfo = ""
    /// Round 12: the dump is showing, summoned by a 4-second hold on the gear
    /// (see `diagnosticPanel`). Deliberately NOT cleared by `reloadForInput`:
    /// leaving it up across an arriving bubble is the useful case, since the
    /// fields under it refresh to the message that just landed.
    @State private var showDiagnostics = false
    /// Round-9: the SURFACE just staged a sendable bubble (create/join/invite/
    /// start) that the human has not sent yet - what the send reminder shows
    /// for on the lobby screens, and (via `alsoStaged`) on the starter's
    /// handoff board, where the controller's own pending list is empty. Reset
    /// when the bubble is sent (sentToken), cancelled (cancelToken), or a new
    /// input reloads the surface.
    @State private var surfaceStaged = false
    /// ROUND 16: the previous session ended badly and the owner has not looked
    /// at it yet. Drives the one-line banner (`healthBanner`); cleared by
    /// tapping it (which opens the dump) or by dismissing it.
    @State private var healthAlarm: FlightSession?

    /// A style toggle keeps this key stable, so the session is NOT reloaded and
    /// the in-progress game survives. A new bubble (payloadURL) or a New game tap
    /// (newGameToken) changes it, which resets and reloads. `chatKey` is in here
    /// too, defensively: this view's state must never survive a conversation
    /// change (the chat-scoping fix's whole point), even though in practice one
    /// extension instance presents one conversation for its lifetime.
    private var loadKey: String {
        "\(newGameToken)|\(startNewGame)|\(chatKey)|\(payloadURL?.absoluteString ?? "")"
    }

    var body: some View {
        // One game per chat, one surface for both presentation styles: always the
        // table (or the New-game setup when the thread has no game yet). There is no
        // "A game in this thread / Open the game" menu — collapsing to compact just
        // shows the same table in the short strip, with Messages' Send in the
        // compose area above. Keeping a single `expandedContent` root also means the
        // board's @State + .task survive the expanded<->compact toggle.
        expandedContent
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            // Round 12: the hold-summoned dump rests on top of whatever is
            // showing - board, lobby or setup - and nothing underneath it
            // changes. Above `fToast` so a toast cannot land on top of it.
            // ROUND 16, the owner: "make it appear as a diagnostic dump in the
            // UI so I can check next time it happens." The dump itself already
            // existed behind a five-second hold on the gear, which is fine for
            // asking a question and useless for being TOLD something - so when
            // the previous session ended in a way it should not have, the
            // surface says so on its own, in one line, and that line opens the
            // dump. Only for an ALARMING end (FlightRecorder.isAlarming): an
            // ordinary abrupt teardown is something Messages does routinely, and
            // a banner that cries wolf on those would train the owner to ignore
            // the one that matters.
            .overlay(alignment: .top) { healthBanner }
            .overlay { if showDiagnostics { diagnosticPanel } }
            .fToast($toast)
            // The setup/lobby Settings + Help squares present these — same
            // sheets as the board's own pair (MessageTableView).
            #if DEBUG
            // The rig's way in, matching the board's own pair. Without it the
            // only way to ask this surface for its Settings sheet is to hit a
            // 40pt square with a synthetic tap, and a probe that can miss cannot
            // tell "the sheet is broken" from "the tap was off".
            .onAppear {
                if ProcessInfo.processInfo.environment["HARNESS_OPEN_SETTINGS"] != nil { showSettings = true }
                if ProcessInfo.processInfo.environment["HARNESS_OPEN_RULES"] != nil { showRules = true }
            }
            #endif
            .sheet(isPresented: $showSettings) {
                MessageSettingsView { showSettings = false }
            }
            .sheet(isPresented: $showRules) {
                // Round-9 (owner): this sheet only serves the PRE-GAME screens
                // (the board presents its own pair inside MessageTableView), so
                // the rulebook here is the simpler lobby page: how the lobby
                // works + the goal. The full rules stay one tap away in-game.
                RulesView(scope: .lobby) { showRules = false }
            }
            .task(id: loadKey) {
                await reloadForInput()
                await autoDriveLobby()
            }
            // Round-6 bug 4: the human just SENT the staged bubble (the host bumped
            // `sentToken` from didStartSending). Tell the live controller its move
            // is now in the thread so it drops it from `pending` - `canSend`/
            // `canUndo` go false and the collapsed drawer's Undo button, which
            // otherwise lingered and re-staged an already-sent move, disappears.
            // `sentToken` is deliberately absent from loadKey, so this fires WITHOUT
            // reloading the surface (the game is unchanged, only its staged move is
            // no longer pending).
            //
            // ROUND 16: it also carries the sent BYTES now, which rebase the
            // controller onto its own bubble. That used to happen by itself,
            // because the send tore the extension down and the next move was
            // played by a controller rebuilt from those bytes; the drawer stays
            // open now (owner: "just keep it collapsed so they can keep
            // playing"), so this signal is the only thing left that does it.
            .onChange(of: sentToken) { _ in
                AnimLog.say("surface sent token=\(sentToken) bytes=\(sentPayload?.count ?? -1)")
                // The other end of the host's `send` note: what the bytes look
                // like AFTER a root-view rebuild and a SwiftUI diff have
                // carried them here. `send 107b` followed by `send-signal none`
                // is a value lost in the view layer; both saying none is a host
                // that never had it.
                FlightRecorder.note("send-signal", sentPayload.map { "\($0.count)b" } ?? "none")
                surfaceStaged = false   // round-9: the staged bubble is sent
                // SYNCHRONOUSLY, in this same SwiftUI transaction: the Undo pill
                // goes now, not after a Task hop and a decode (owner: "should
                // probably disappear the second you hit send"). `markSent` below
                // does the rest and clears the flag.
                controller?.markSending()
                Task { await controller?.markSent(payload: sentPayload) }
            }
            // Round-9: the human deleted the staged bubble from the input field
            // (didCancelSending) - nothing is awaiting Send any more.
            .onChange(of: cancelToken) { _ in surfaceStaged = false }
            // A bubble ARRIVED while this surface is open (didReceive). Apple
            // does not move `selectedMessage` for an arrival, so loadKey does
            // not change and the .task above will not re-run - this one does.
            .task(id: incomingToken) { await maybeAdoptIncoming() }
            // Read once, on the first paint of this surface. Deliberately not in
            // `loadKey`'s task: the question "how did last time end" is answered
            // once per launch, not once per bubble.
            .task {
                guard healthAlarm == nil, let p = FlightRecorder.previousSession(),
                      FlightRecorder.isAlarming(p) else { return }
                healthAlarm = p
            }
    }

    /// The banner. One line, at the top, tappable - and gone for good once it
    /// has been acted on, because its job is to be noticed once and not to
    /// decorate the board.
    @ViewBuilder private var healthBanner: some View {
        if let alarm = healthAlarm, !showDiagnostics {
            HStack(spacing: 6) {
                Text(FlightRecorder.verdict(alarm))
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundColor(.white)
                    .lineLimit(2)
                    .minimumScaleFactor(0.7)
                Spacer(minLength: 4)
                Text("✕").font(.system(size: 12, weight: .bold)).foregroundColor(.white)
                    .onTapGesture { healthAlarm = nil }
            }
            .padding(.horizontal, FSpace.s)
            .padding(.vertical, 5)
            .background(FColor.accent.opacity(0.94))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .padding(.horizontal, FSpace.s)
            .padding(.top, 4)
            .contentShape(Rectangle())
            .onTapGesture { showDiagnostics = true; healthAlarm = nil }
            .transition(.move(edge: .top).combined(with: .opacity))
        }
    }

    // MARK: - the stale-branch gate (round 20)

    /// The newer chain this board was found to be behind, kept so the bar below
    /// has something to open. Cleared whenever an adopt comes back live.
    @State private var supersededBy: Data?
    /// The verdict from the last `adopt`, spent by `seatOnBoard` - which is
    /// where the controller finally exists, and is reached from the name gate
    /// and the DEBUG seat picker as well as straight from `adopt`.
    @State private var staleBranch = false

    /// THE BAR OVER A READ-ONLY BOARD. Says why nothing can be tapped, and
    /// offers the one thing that fixes it.
    ///
    /// Offering the newer chain by BUTTON rather than adopting it silently is
    /// the whole difference between this and the round-7 payload cache the owner
    /// removed: the extension still renders exactly the bubble you tapped, until
    /// you ask it not to.
    @ViewBuilder private func supersededBar(_ c: MessageTurnController) -> some View {
        if c.superseded {
            HStack(spacing: 8) {
                Text(FStrings.t("ios.msg.stale"))
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundColor(.white)
                    .lineLimit(2)
                    .minimumScaleFactor(0.7)
                Spacer(minLength: 4)
                if supersededBy != nil {
                    FButton(FStrings.t("ios.msg.opennewest"), kind: .wood, compact: true) {
                        Task { await openNewest() }
                    }
                }
            }
            .padding(.horizontal, FSpace.s)
            .padding(.vertical, 5)
            .background(FColor.accent.opacity(0.94))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .padding(.horizontal, FSpace.s)
            .transition(.move(edge: .top).combined(with: .opacity))
        }
    }

    /// Take the newer chain, through the SAME `adopt` a tap or an arrival goes
    /// through - so seat identity, the phase-0 lobby route and the open-replay
    /// all behave exactly as they would have if this bubble had been tapped.
    private func openNewest() async {
        guard let bytes = supersededBy,
              let env = try? await MessageEnvelope.decode(payload: bytes, viewer: -1) else { return }
        AnimLog.say("surface opens the newest chain by request")
        await adopt(winner: bytes, env: env)
    }

    /// IS THIS BOARD A BRANCH OFF AN OLD BUBBLE, and record it if it is not.
    ///
    /// The decision itself is `StaleBranchGate.rank` - two authorities, Rule P
    /// and "does the chain on file actually show more of the game", both of
    /// which must agree before the board goes read-only. It lives there and not
    /// here because it can be driven from a test with real sealed chains; this
    /// only spends the answer.
    @discardableResult
    private func rankAgainstHighWater(_ payload: Data, env: MessageEnvelope) async -> Bool {
        let verdict = await StaleBranchGate.rank(payload: payload, env: env, chatKey: chatKey)
        supersededBy = verdict.newest
        return verdict.superseded
    }

    /// Fold an ARRIVING bubble into the live surface, Rule P deciding (§7.2).
    ///
    /// Why this exists: `didReceive` fires while the extension is open, but the
    /// arrival does not become `selectedMessage`, so nothing reloaded and the
    /// surface sat on whatever chain it last adopted until the human re-tapped
    /// a bubble. Mostly that was just staleness (an opponent's move not showing
    /// until reopen; a lobby roster missing the join that just arrived). In the
    /// double-Start race it was a DEADLOCK: two players tap Start off different
    /// lobby states, two LIVE handoffs exist, Rule P (kernel rule 3) picks the
    /// fuller one - but the losing starter's own device was already sitting on
    /// its fork's board and never re-compared, so if the real game's first
    /// attacker was that player, every screen in the chat waited forever.
    ///
    /// Rule P still decides everything: a stale or duplicate arrival loses to
    /// the chain on screen and changes NOTHING (no teardown, no replay). Only a
    /// strictly-preferred arrival is adopted - through the same `adopt` a tap
    /// goes through, so seat identity and the phase-0 lobby route both
    /// hold. With nothing on screen to compare (spectator / picker / name
    /// gate), the arrival simply renders - round 7 keeps no cached chain to
    /// weigh it against. `showSetup` is exempt: the human explicitly asked for
    /// a new game.
    private func maybeAdoptIncoming() async {
        guard let url = incomingURL, !showSetup, !startNewGame,
              let bytes = try? MessageEnvelope.payloadBytes(url: url) else { return }
        let current = controller?.basePayload ?? lobby?.payload
        if bytes == current { AnimLog.say("arrival ignored - same chain"); return }
        if let current {
            // A refusal here is SILENT to the player, and that is what made the
            // rule-4 hole (a chain tying with its own child on round/turn, see
            // msg_wire.c msg_rule_p) so hard to see: the board just sat one
            // move behind until the bubble was re-tapped. So a refusal now says
            // which chains it weighed - `peek` reads the headers without
            // touching the resident game, and the whole block is skipped when
            // the trace is off.
            var pref = -999
            do { pref = try await MessageKernel.shared.preferred(current, bytes) }
            catch { AnimLog.say("arrival Rule P threw \(error)") }
            if pref <= 0 {
                if AnimLog.on {
                    let ce = try? await MessageKernel.shared.peek(payload: current)
                    let be = try? await MessageKernel.shared.peek(payload: bytes)
                    AnimLog.say("arrival ignored - Rule P pref=\(pref) "
                        + "cur=[t\(ce?.turn ?? -1) r\(ce?.round ?? -1) actor\(ce?.lastActorSeat ?? -1)] "
                        + "new=[t\(be?.turn ?? -1) r\(be?.round ?? -1) actor\(be?.lastActorSeat ?? -1)]")
                }
                return
            }
        }
        guard let env = try? await MessageEnvelope.decode(payload: bytes, viewer: -1) else {
            AnimLog.say("arrival ignored - decode failed")
            return
        }
        // This runs under `.task(id: incomingToken)`, so a NEWER arrival CANCELS
        // this one - but cancellation only lands at an await, and nothing above
        // rethrows it (`try?` + do/catch swallow it), so a superseded task used
        // to sail on and adopt with facts read BEFORE the newer arrival moved
        // the base: at best a duplicate adopt of the same chain (the stranded
        // open-replay veil this file's round-18 fix is about), at worst an OLDER
        // chain adopted over the newer one. The newer task owns the surface now.
        guard !Task.isCancelled else {
            AnimLog.say("arrival ignored - superseded by a newer arrival")
            return
        }
        AnimLog.say("surface adopts arrival phase=\(env.phase) joins=\(env.joins.count) turn=\(env.turn)")
        await adopt(winner: bytes, env: env)
    }

    /// 1.0(6): the graceful failure screen - shown ONLY when a message fails to
    /// open (decode error / damaged), never during normal play. It gives the
    /// human a way out (New game) and dumps the full payload + versions so a
    /// recurrence can be captured by screenshot. A HARD process crash cannot show
    /// any UI; this covers the graceful "gray screen" failures the extension can
    /// still render through.
    private var diagnosticFailView: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 6) {
                Text("Couldn’t open this game").font(FType.title(18)).onTableText()
                FButton(FStrings.t("ios.msg.newgame"), kind: .wood, action: onNewGame)
                diagnosticDump
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(TableBackground().ignoresSafeArea())
    }

    /// The dump itself, with no framing of its own — ONE version of these
    /// fields, shown both by the failure screen above and by the hold-summoned
    /// panel below, so the two can never drift into disagreeing about what a
    /// message contains.
    @ViewBuilder private var diagnosticDump: some View {
        let hex = dumpHex
        let url = dumpURL
        Group {
            if let e = diagError { Text("ERR: \(e)").foregroundColor(.red) }
            // The version fields: FMSG format byte (byte 1), flags byte
            // (byte 2 - 0x04 = 1.0(3) passing bit), the URL text version,
            // and the replay-body ENCODING version (5/6/7 - the real
            // cross-version signal).
            if !hex.isEmpty {
                Text("VER msgFmt=0x\(hex.dropFirst(2).prefix(2)) flags=0x\(hex.dropFirst(4).prefix(2)) urlVer=\(url?.pathComponents.last?.first.map(String.init) ?? "?")")
            }
            if let c = controller {
                Text("seat \(c.mySeat) · \(c.pending.count) staged\(c.isGenesis ? " · genesis" : "")")
            }
            if !diagInfo.isEmpty { Text("opened: \(diagInfo)") }
            if !hex.isEmpty {
                Text("HEX (\(hex.count / 2) bytes):")
                Text(hex).textSelection(.enabled)
            }
            if let u = url?.absoluteString {
                Text("URL:")
                Text(u).textSelection(.enabled)
            }
        }
        .font(.system(size: 10, design: .monospaced))
        .onTableText()
    }

    /// The chain to dump: the one the board is ACTUALLY on, not the one the
    /// surface happened to load from.
    ///
    /// They diverge routinely. An arrival is folded into the live controller
    /// without ever becoming `selectedMessage`, so `payloadURL` - and with it
    /// the `diagHex` captured at load - still describes the bubble the human
    /// last tapped, possibly several moves ago. A dump that reports that is
    /// worse than no dump: it answers a question about the wrong message while
    /// looking authoritative. `basePayload` is the controller's own adopted
    /// chain, so it moves with every adopt. Falls back to the load-time capture
    /// when there is no controller, which is exactly the damaged case the
    /// failure screen covers.
    private var dumpHex: String {
        guard let p = controller?.basePayload else { return diagHex }
        return p.map { String(format: "%02x", $0) }.joined()
    }

    private var dumpURL: URL? {
        guard let p = controller?.basePayload else { return payloadURL }
        return MessageEnvelope.link(payload: p)
    }

    /// ROUND 12 (owner): "I know we have like a last message diagnostics view
    /// that is not enabled. How about this though - if you hold the settings
    /// button for 4 seconds, it pops up. And if you tap again it goes away."
    /// Five, on a second pass - the owner's call, and it reads as an easter egg
    /// rather than a slow tap.
    ///
    /// The dump used to be reachable only by FAILING to open a bubble, which is
    /// exactly when you cannot ask it about a bubble that opened fine. This is
    /// the same fields, on demand, over whatever is on screen.
    ///
    /// Not DEBUG-gated on purpose: its whole value is reading the real bytes of
    /// a real message on a real phone, in the build that shipped. A five-second
    /// hold on an unlabelled square is not something a player finds by accident,
    /// and what it shows is the reader's own game.
    ///
    /// It floats OVER the surface rather than replacing it, so summoning it
    /// never disturbs the board underneath: no reload, no teardown, and the
    /// staged bubble is exactly where it was when you dismiss.
    private var diagnosticPanel: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 6) {
                healthDump
                Text("Last message").font(FType.title(16)).onTableText()
                diagnosticDump
                if dumpHex.isEmpty && diagInfo.isEmpty && diagError == nil {
                    // Reached from the setup screen, or after a New game tap:
                    // there is no message behind this surface to dump. Say so
                    // rather than showing an empty panel that reads as broken.
                    Text("no message open").font(.system(size: 10, design: .monospaced))
                        .onTableText()
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        // Nearly opaque, not fully: enough of the board shows through to make
        // it obvious this is a panel resting on top of a game, not a screen the
        // extension has navigated to.
        .background(TableBackground().opacity(0.97).ignoresSafeArea())
        // "Tap again it goes away" - anywhere, since the panel covers the gear
        // that summoned it. `contentShape` so the gaps between the lines are
        // dismissible too, not just the text.
        .contentShape(Rectangle())
        .onTapGesture { showDiagnostics = false }
    }

    /// ROUND 16 (owner): "sometimes it just hangs. Even on newer devices. Can't
    /// tell why... if there's a crash or something make it appear as a
    /// diagnostic dump in the UI so I can check next time it happens."
    ///
    /// The trail the PREVIOUS session left, plus this one so far, plus the live
    /// footprint - see FlightRecorder for why a trail is the only possible
    /// evidence (the failure being chased kills the process outright, so nothing
    /// survives to report itself). Rendered above the message dump because when
    /// this section has something to say it is the more urgent of the two.
    @ViewBuilder private var healthDump: some View {
        let previous = FlightRecorder.previousSession()
        VStack(alignment: .leading, spacing: 6) {
            Text("Health").font(FType.title(16)).onTableText()
            if let p = previous, FlightRecorder.isAlarming(p) {
                // The one line the owner is looking for, in the accent, so it is
                // not something to be found in a wall of monospace.
                Text(FlightRecorder.verdict(p))
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundColor(FColor.accent)
            }
            Text(FlightRecorder.report(previous: previous,
                                       current: FlightRecorder.currentSession()))
                .font(.system(size: 9, design: .monospaced))
                .onTableText()
                .textSelection(.enabled)
            FButton("Clear", kind: .wood, compact: true) {
                FlightRecorder.reset()
                showDiagnostics = false
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    @ViewBuilder private var expandedContent: some View {
        if let controller {
            MessageTableView(controller: controller,
                             onSend: { payload, fromUndo in await onSend(payload, controller.mySeat, fromUndo) },
                             // A finished game's New game is a REMATCH: same
                             // table, built right here from the board still on
                             // screen. Anything else - a mid-game board, a
                             // roster with an unnamed seat - is an ordinary new
                             // game and punishes nobody.
                             onNewGame: {
                                 guard let r = rematchRoster(from: controller) else {
                                     onNewGame(); return
                                 }
                                 onFreshChain()
                                 // The table's RULES carry over with the table.
                                 // A rematch is the same people playing again,
                                 // so it starts as the game they were just
                                 // playing - and the checkbox is still there to
                                 // change it before anyone starts.
                                 let passing = controller.passingAllowed
                                 Task { await createRematchLobby(joins: r.joins,
                                                                 foolSeat: r.foolSeat,
                                                                 passing: passing) }
                             },
                             onUnstage: onUnstage,
                             alsoStaged: surfaceStaged,
                             onDiagnostics: { showDiagnostics = true },
                             onOpenURL: onOpenURL)
                // 1.0(4) live-receive blink: a received bubble reloads the surface
                // with a NEW controller. Tying the board's identity to the
                // controller INSTANCE (not just the `if let` slot) means a reload
                // still gets a fresh board with fresh @State - so the open-move
                // replay fires exactly as before - but WITHOUT the controller ever
                // going nil, which is what flashed `Color.clear` between the old
                // board and the new one. A style toggle keeps the same controller
                // instance, so the id is stable and the in-progress board survives
                // (same guarantee as before). See reloadForInput / load.
                .id(ObjectIdentifier(controller))
                // ROUND 20: the board is read-only because a newer chain for this
                // game has already been through this device. An overlay rather
                // than a row in the stack, so the bar appearing does not
                // re-lay-out the board underneath it - a stale board is still a
                // board, and the cards must not move because it grew a caption.
                .overlay(alignment: .top) { supersededBar(controller) }
        } else if let lob = lobby {
            LobbyView(env: lob.env, mySeat: lobbySeat(lob.env),
                      nickname: MessageGameStore.shared.nickname,
                      onJoin: { name in Task { await joinLobby(lob, nickname: name) } },
                      onStart: { Task { await startGame(lob) } },
                      onExit: { Task { await leaveLobby(lob) } },
                      onInvite: { Task {
                          await onSend(lob.payload, lobbySeat(lob.env) ?? 0, false)
                          surfaceStaged = true   // round-9: the invite awaits Send
                      } },
                      onSetPassing: { on in Task { await setLobbyPassing(lob, passing: on) } },
                      passingBaseline: passingBaseline[lob.env.gameId],
                      // nil in every shipping build: the closure only exists
                      // where `addSoloSeat` is compiled at all.
                      onAddSoloSeat: soloSeatAction(lob))
                // Keep the corner pair's own footprint clear - the lobby is
                // centred in whatever height it is given and the pair is an
                // overlay, so a tall lobby lays out straight through it.
                .padding(.bottom, SettingsHelpSquares.reservedHeight)
                .overlay(alignment: .bottomLeading) { settingsHelpCorner }
                // Round-9: the send reminder covers EVERY staged bubble, not
                // just board moves - a join/invite/start left unsent stalls the
                // whole thread the same way. Collapsed view only, same as the
                // board's; full-bleed container, so the screen-edge axis.
                //
                // Round-10 #2: gated on the surface's LIVE height (the same
                // collapseFraction the board uses), NOT the `style` prop -
                // present() only runs on discrete host events, so `style` goes
                // stale across a grabber drag or an auto-transition, which is
                // exactly how the arrow leaked into the EXPANDED lobby.
                .overlay(alignment: .topTrailing) {
                    GeometryReader { g in
                        StagedSendHint(staged: surfaceStaged,
                                       visible: MessageTableView.collapseFraction(height: g.size.height) > 0.95)
                            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
                    }
                }
        } else if let g = nameGate {
            NameGateView(prefill: MessageGameStore.shared.nickname) { name in
                Task { await nameThenSeat(name, gate: g) }
            }
        } else if showSetup {
            // chatPlayers is threaded through unused (see NewGameSetup's own doc)
            // — kept only so this call site, the harness, and
            // MessagesViewController (which all compute a real participant
            // count) keep compiling unchanged.
            NewGameSetup(nickname: MessageGameStore.shared.nickname,
                         isDM: chatIsDM, chatPlayers: chatPlayers) { name in
                Task { await start(nickname: name) }
            }
            .padding(.bottom, SettingsHelpSquares.reservedHeight)
            .overlay(alignment: .bottomLeading) { settingsHelpCorner }
        } else if let a = ambiguous {
            SeatPicker(nPlayers: a.env.nPlayers, joins: a.env.joins) { seat in
                Task { await choose(seat: seat, from: a) }
            }
        } else if let s = spectator {
            // ROUND 20: A FINISHED GAME IS A RESULT, WHATEVER SEAT YOU HOLD -
            // including none (owner: "spectators should still be able to see win
            // screen"). Until now this branch drew the public board and the
            // "spectating" caption at every phase, so the one bubble a spectator
            // most wants to open - the last one - showed them a swept, empty
            // table and a line telling them they could not play on it.
            //
            // The SAME `FGameOverList` a player gets, ranked by the same
            // function: who came first and who was the fool is public (§10 - it
            // is on the bubble's own picture), so there is nothing here a
            // spectator may not see. `mySeat: -1` is what says "none of these
            // rows is yours", so no row is tagged (You).
            //
            // New game works from here for the same reason it works anywhere: a
            // spectator watching a table finish is exactly somebody who might
            // want to deal the next one.
            if let board = spectatorBoard {
                // ROUND 21: the last move, then the ranks - the same board a
                // player watches, seated at nobody's seat (see `spectatorBoard`).
                // `MessageTableView` owns both halves: it replays the chain's
                // final move and then gives way to `FGameOverList` itself, so
                // there is no second copy of "when does the result appear" here.
                //
                // `onSend` can never fire (nothing is sendable from a seat the
                // kernel will not compute a move for), and is written as a
                // no-op rather than a fatalError for the same reason every other
                // unreachable branch in this file is: a screen a watcher is
                // looking at must not be the thing that takes the extension down.
                MessageTableView(controller: board,
                                 onSend: { _, _ in },
                                 onNewGame: onNewGame,
                                 onOpenURL: onOpenURL)
                    .id(ObjectIdentifier(board))
            } else if s.view.isOver {
                // The board could not be built - the ranks alone, as round 20
                // left them. Never reached in practice; kept because losing the
                // result screen is a worse failure than losing the animation.
                FGameOverList(rows: MessageTableView.finishRows(s.view, names: s.names, mySeat: -1),
                              onNewGame: onNewGame,
                              replayURL: spectatorReplayURL,
                              onOpenURL: onOpenURL)
            } else {
                // Release-only §6.3 fallback: read-only, public (no hand), with a
                // caption explaining why there is nothing to tap (§ release security).
                VStack(spacing: 4) {
                    MessageBoardView(view: s.view, names: s.names)
                    // Round-5 M10: full-opacity ink + a LIGHT shadow, not 55%
                    // black — the busy wool weave has no fixed-opacity foreground
                    // that survives it (see the sweep note on DamagedView below).
                    // Round-6 #17 added the weight: `onTableText` (Tokens.swift).
                    Text(FStrings.t("ios.msg.spectating"))
                        .font(.footnote).onTableText()
                        .multilineTextAlignment(.center).padding(.horizontal).padding(.bottom, 8)
                }
            }
        } else if damaged || diagError != nil {
            // 1.0(6): a message that FAILS to open shows a graceful diagnostic
            // (the full payload/version dump + New game) instead of a gray screen.
            // This branch is reached ONLY on a real decode failure - normal play,
            // and the transient reload below, never show it.
            diagnosticFailView
        } else {
            // 1.0(4) live-receive blink: while a received bubble reloads the
            // surface (controller briefly nil), a ProgressView spinner flashed
            // over the wool for a frame or two - the "slight blink". The reload is
            // sub-frame in the common case, so show the steady wool (Color.clear
            // over GameSurface's TableBackground) instead of a spinner that
            // announces the reload. NOTE: the board still tears down and remounts
            // on a live receive (that remount is what drives the incoming-move
            // replay off the view nil->value transition); removing the remount
            // entirely needs frame-by-frame harness verification, tracked
            // separately, since it would otherwise kill that replay.
            Color.clear
        }
    }

    /// The Settings + Rulebook squares on the setup and lobby screens (owner
    /// ask, durak-rules-redesign): the SAME 40pt pair the board floats
    /// bottom-left (`SettingsHelpSquares`), at the same corner inset — 4 outer
    /// + the pair's own FSpace.m inner = 16pt off the edge, exactly the board's
    /// line — so Settings and the rules are reachable before a game exists at
    /// all. Round-9 (owner: "we need to bring them back"): shown in EVERY
    /// presentation style — the old expanded-only gate meant the pair was
    /// invisible in the compact drawer, which is where the extension actually
    /// opens, so in practice it read as removed.
    private var settingsHelpCorner: some View {
        SettingsHelpSquares(onSettings: { showSettings = true },
                            onHelp: { showRules = true },
                            onDiagnostics: { showDiagnostics = true })
            .padding(.leading, 4)
            .padding(.bottom, 4)
    }

    /// Reset + (re)load for a NEW input. A compact<->expanded toggle leaves
    /// loadKey unchanged, so `.task(id:)` does not fire and the game persists.
    private func reloadForInput() async {
        AnimLog.say("surface reload key=[\(loadKey)]")
        // Do NOT tear the board down to nil up front: on a live receive that
        // blank (Color.clear) between the old controller and the new one is the
        // "blink". Reset only the NON-board transient screens here; the resolved
        // screen sets `controller` - a fresh instance for a board (the `.id` in
        // expandedContent gives it fresh @State), or nil in the branches below
        // that show something other than a board.
        lobby = nil; nameGate = nil; showSetup = false
        ambiguous = nil; spectator = nil; spectatorReplayURL = nil
        spectatorBoard = nil; damaged = false
        surfaceStaged = false   // round-9: a new input owes nothing to Send yet
        await load()
        AnimLog.say("surface showing \(showingWhat)")
    }

    /// What the surface resolved to, for the trace. "Why is it showing a lobby
    /// when the thread is mid-game" is only answerable if the surface says which
    /// branch it took and off which bytes.
    private var showingWhat: String {
        if controller != nil { return "board" }
        if let l = lobby { return "lobby(joins=\(l.env.joins.count) phase=\(l.env.phase) game=\(l.env.gameId))" }
        if nameGate != nil { return "nameGate" }
        if showSetup { return "setup" }
        if ambiguous != nil { return "seatPicker" }
        if spectator != nil { return "spectator" }
        if damaged { return "damaged" }
        return "nothing"
    }

    /// Ask the router what to show, then put it on screen. The DECISION —
    /// setup vs lobby vs board, and which chain wins Rule P — is not made here
    /// any more (MessageSurfaceRouter): it is a function of the selected
    /// bubble, this chat's cache, and the New-game intent, so it can be driven
    /// in a test without a simulator. What stays here is the part that genuinely
    /// needs the host: seat identity (§6) and the name gate.
    private func load() async {
        AnimLog.say("surface load url=\(payloadURL?.absoluteString.suffix(12) ?? "nil") startNew=\(startNewGame)")
        diagError = nil; diagInfo = ""   // 1.0(6) diagnostic
        #if DEBUG || SOLO_TESTING
        // Dev hook (owner: "use build flags to skip the create game / join game /
        // start game stuff and jump straight to the game state"). With a
        // `dev.fatboard` file in the App Group, this chain IS the surface: no
        // setup screen, no lobby, no Start, seated as the DEFENDER so the very
        // first tap can be Pickup. Compiled out of every Release build; the
        // chain itself is searched offline by `msg_wire_test --fatboard` — see
        // MessageDevBoard for why it is a constant and not a search.
        if await openSeededBoard() { return }
        #endif
        var incoming: Data?
        if let url = payloadURL {
            do { incoming = try MessageEnvelope.payloadBytes(url: url) }
            catch { diagError = "payloadBytes: \(error)"; damaged = true; return }
        }
        // 1.0(6): the raw envelope bytes (the iMessage format, header + body).
        diagHex = incoming.map { $0.map { String(format: "%02x", $0) }.joined() } ?? ""
        let screen = await MessageSurfaceRouter.resolve(payload: incoming,
                                                        startNewGame: startNewGame,
                                                        chatKey: chatKey)
        AnimLog.say("surface router -> \(screen)")
        // reloadForInput no longer clears `controller` up front (blink fix), so a
        // resolution that is NOT a board must clear the old one itself, or the
        // stale board would win expandedContent's `if let controller` over the
        // lobby/setup/damaged screen.
        switch screen {
        case .setup:
            controller = nil
            showSetup = true
        case .damaged:
            controller = nil
            damaged = true
        case .lobby(let payload):
            controller = nil
            // Decoding also ADOPTS, so the lobby's locked seed is resident for a
            // join/start seal — same as before this was routed.
            guard let env = try? await MessageEnvelope.decode(payload: payload, viewer: -1) else {
                damaged = true
                return
            }
            noteRulesBaseline(env)
            lobby = Lobby(env: env, payload: payload)
        case .board(let payload):
            let env: MessageEnvelope; let bodyVer: Int
            do { (env, bodyVer) = try await MessageKernel.shared.decodeWithBodyVersion(payload: payload) }
            catch { diagError = "board decode: \(error)"; damaged = true; return }
            diagInfo = "phase \(env.phase) turn \(env.turn) round \(env.round) n \(env.nPlayers) actor \(env.lastActorSeat) game \(env.gameId) joins \(env.joins.count) · bodyVer=\(bodyVer)"
            await adopt(winner: payload, env: env)
        }
    }

    #if DEBUG || SOLO_TESTING
    /// DEV ONLY (`dev.fatboard`): open a canned chain directly, as its defender.
    /// Returns true when it took over the surface, so `load()` stops.
    ///
    /// The seat is the DEFENDER's, resolved from the chain rather than from the
    /// seat cache or the picker: this board exists to be picked up from, and
    /// only the defender may do that. That is the "seat yourself as defender"
    /// half of the owner's instruction, done for you.
    private func openSeededBoard() async -> Bool {
        guard let payload = MessageDevBoard.claimSeededPayload() else { return false }
        guard let env = try? await MessageKernel.shared.decode(payload: payload, viewer: -1),
              let view = await MessageKernel.shared.residentView(viewer: -1),
              view.defender >= 0 || view.isOver else {
            AnimLog.say("dev.fatboard present but not a decodable chain - ignoring")
            return false
        }
        // `dev.seat` overrides the chair. Default is the defender's (only they
        // may pick up); the deal case wants an ATTACKER, since it is an attacker
        // saying good that closes the bout and deals.
        // A FINISHED chain (the `endgame` board, for verifying the fool's
        // penalty) has no defender to sit at, so it defaults to seat 0.
        let seat = MessageDevBoard.seededSeat.map { max(0, min($0, view.players.count - 1)) }
            ?? (view.defender >= 0 ? view.defender : 0)
        AnimLog.say("dev.fatboard: seating as \(seat) (defender=\(view.defender)), \(view.battles.count) battles")
        showSetup = false
        lobby = nil
        // `quietOpen`: this is a seeded state, not a move anyone just watched -
        // opening it must not replay whatever its last action happened to be, or
        // the film starts with an animation nobody asked for. Unless the REPLAY
        // is the point (round 16's bubble delta: `dev.replay`), in which case
        // this opens exactly as a tapped bubble does.
        seatOnBoard(seat: seat, env: env, winner: payload,
                    quietOpen: !MessageDevBoard.seededReplays)
        return true
    }
    #endif

    /// DEV ONLY (HARNESS_AUTOGAME): press the setup/lobby buttons a human would,
    /// so an unattended run can actually reach a board. Lobby v3 put three human
    /// taps — Create game, Join, Start — between launch and a dealt game, and the
    /// harness's auto-play only knows how to make MOVES, so an auto-run just sat
    /// on the setup screen forever and the animation trace it exists to produce
    /// was four lines long. Each participant's turn through here does the one
    /// thing that seat can do; HARNESS_AUTOGAME's own deliver+become carries it
    /// to the next. Never compiled into Release.
    ///
    /// Runs INSIDE `.task(id: loadKey)`, not as a Task of its own, and that is
    /// load-bearing: a detached one outlives the surface that started it. The
    /// first version was detached, and its 400ms sleep regularly finished after
    /// the harness had already switched to the next participant — so a joiner's
    /// pending drive ran with the PREVIOUS player's captured lobby and started
    /// the game as them. An 8-player run reached a 2-player board with a seat
    /// nobody at that keyboard held. Under `.task` it is cancelled with the
    /// surface, so a stale drive cannot act at all.
    ///
    /// It also waits for the lobby to FILL. Starting at two is what a human may
    /// do, but an auto-run that does it turns "8 players" into a 2-player game
    /// and never exercises the seat count being asked about.
    private func autoDriveLobby() async {
        #if DEBUG
        guard ProcessInfo.processInfo.environment["HARNESS_AUTOGAME"] != nil else { return }
        try? await Task.sleep(nanoseconds: 400_000_000)
        if Task.isCancelled { return }
        if showSetup { await start(nickname: MessageGameStore.shared.nickname); return }
        guard let lob = lobby else { return }
        // The lobby's capacity is the WIRE's max (8) for a group, not how many
        // people are in the chat — so the target is the chat's own size.
        let target = min(lob.env.nPlayers, max(2, chatPlayers))
        if lobbySeat(lob.env) == nil {
            if lob.env.joins.count < lob.env.nPlayers {
                await joinLobby(lob, nickname: MessageGameStore.shared.nickname)
            }
        } else if lob.env.joins.count >= target {
            // Calls startGame() directly — bypasses LobbyControls.offered's
            // round-5 M9 gate (that gate only governs the UI's Start
            // button). Fine here: this is a scripted driver racing to a
            // dealt board for a screenshot, not a human who could be locked
            // out of one.
            await startGame(lob)
        }
        #endif
    }

    // MARK: the fool's penalty (Rule F)

    /// The rematch roster, read STRAIGHT OFF the finished board: the same table,
    /// in the same cycle, rotated so this device sits at seat 0. nil when this
    /// is not a game a rematch can be built from.
    ///
    /// Rotated because seat 0 is the creator's by construction (`createWaiting`)
    /// and whoever taps New game is the creator. Preserving the CYCLE is what
    /// matters, not the numbers - the wire keys a roster rotation-canonically
    /// for exactly this reason - so the same table comes back as the same
    /// table however it is spun.
    ///
    /// My own name comes from the store, not from the old game's join: this
    /// device may have been renamed since, and the name it seals now is the one
    /// its seat will be recognised by.
    private func rematchRoster(from controller: MessageTurnController)
        -> (joins: [MessageJoin], foolSeat: Int)? {
        guard let v = controller.view, v.isOver, v.gameOver >= 0 else { return nil }
        let n = v.players.count
        let me = controller.mySeat
        guard n >= 2, me >= 0, me < n, v.gameOver < n else { return nil }

        // Names BY SEAT. A seat with no name cannot be recognised by its owner
        // on the other device (SeatIdentity.seatClaimedByName is what lets a
        // prefilled lobby seat people who never tapped Join), so a roster
        // missing one is not a rematch roster at all - the tap falls back to an
        // ordinary new game rather than seating somebody as a blank.
        var joins: [MessageJoin] = []
        for s in 0..<n {
            let old = (s + me) % n
            let name = old == me ? MessageGameStore.shared.nickname : (controller.names[old] ?? "")
            guard !name.isEmpty else { return nil }
            joins.append(MessageJoin(seat: s, name: name))
        }
        return (joins, (v.gameOver - me + n) % n)
    }

    /// "New game" on a FINISHED board: create the rematch lobby HERE, from the
    /// game still on screen, and stage it. No intent is written down and
    /// nothing is read back - the roster, the fool and my seat are all in hand
    /// at the moment of the tap, and a cache of them would only be a second
    /// place for them to be wrong.
    ///
    /// It also means no teardown: the ordinary New game path bumps
    /// `newGameToken`, which re-ids this whole view and routes through the name
    /// prompt. A rematch has nothing to ask - this device just played a game
    /// under its name - so it goes straight to a lobby. `onFreshChain` is the
    /// one thing it still needs from the host: start a NEW MSSession, so the
    /// rematch's first bubble does not collapse the result card of the game it
    /// came from.
    ///
    /// The lobby stays OPEN at the usual capacity, deliberately: someone else
    /// in the chat may join a rematch, and if they do, the wire's guard sees a
    /// roster that no longer keys equal and the penalty does not fire. That is
    /// the owner's "if the players do not change at all".
    ///
    /// `passing` is the finished game's own rule, carried across: `newGame`
    /// resets the kernel's rules to the classic transfer game, so a rematch of a
    /// podkidnoy table would otherwise silently deal a perevodnoy one. The
    /// lobby's checkbox is still live - this sets where it STARTS, not what it
    /// must be.
    private func createRematchLobby(joins: [MessageJoin], foolSeat: Int,
                                    passing: Bool) async {
        var seed = Data(count: 32)
        for i in 0..<32 { seed[i] = UInt8.random(in: 0...UInt8.max) }
        let gameId = UInt64.random(in: 1...UInt64.max)
        let capacity = max(chatIsDM ? 2 : 8, joins.count)
        do {
            try await MessageKernel.shared.newGame(seed: seed, players: capacity)
            await MessageKernel.shared.setPassing(passing)
            let armed = await MessageKernel.shared.armRematchCarry(joins: joins,
                                                                   foolSeat: foolSeat)
            AnimLog.say("rematch lobby: n=\(joins.count) fool@\(foolSeat) armed=\(armed)")
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: 0, gameId: gameId,
                parent8: Data(repeating: 0, count: 8), joins: joins)
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            controller = nil
            showSetup = false
            damaged = false
            cache(seat: 0, env: env, payload: payload)
            lobby = Lobby(env: env, payload: payload)
            await onSend(payload, 0, false)
            surfaceStaged = true
        } catch {
            damaged = true
        }
    }

    // MARK: creation + lobby (§5.2)

    /// Finish the New game setup: persist the nickname (B3), then create a
    /// lobby — every chat shape now goes through the SAME lobby machinery
    /// (lobby v3, note 2: "2p — creator creates the game and sends the first
    /// chat. The other player can join, or do join+start... the same hand
    /// because the seed was set by the first chat the creator sent"). A DM
    /// used to deal LIVE straight to the board here (`startGenesis`, now
    /// removed) — that let the creator see their hand before committing and
    /// reroll by tapping New game until it was good; a locked-seed lobby
    /// closes that.
    private func start(nickname: String) async {
        // Round-5 B1: NewGameSetup only calls this from its `.ok` branch, so
        // `nickname` is already NicknameGate-valid and trimmed — the "You"
        // fallback that used to live here is unreachable now. Re-check
        // defensively anyway (never trust a caller's promise past the type
        // system) and, per M2, fall back to the STORED nickname rather than
        // a placeholder if it somehow is not: skipping the write below just
        // leaves whatever this device already had on file.
        if case .ok(let name) = NicknameGate.check(nickname) {
            MessageGameStore.shared.nickname = name
        }
        showSetup = false
        await createWaiting(nickname: MessageGameStore.shared.nickname)
    }

    /// Create a game as seat 0 and open its lobby (lobby v3): lock the seed +
    /// game id in NOW — that is the whole "seed locked at create" guarantee —
    /// and seal a WAITING bubble seating only me. The kernel is dealt at the
    /// lobby's CAPACITY, not a chosen player count: nobody has picked how many
    /// will play yet. For a group chat that capacity is the wire's max, 8 (a
    /// WAITING envelope with n_players==8 renders as an open lobby, not 8
    /// literal seats — see LobbyView) — not a real 8-player game. A DM's
    /// capacity is 2 (note 2): the chat has exactly two people, so "lobby
    /// full" must read correctly once the one possible opponent has joined,
    /// not "waiting for 6 more". Start (below) later re-derives the SAME seed
    /// at however many actually joined. Auto-stages the invite (notes 14/16):
    /// the human still presses Messages' own Send, but there is no separate
    /// "Send invite" button offering the same action a second time.
    private func createWaiting(nickname: String) async {
        var seed = Data(count: 32)
        for i in 0..<32 { seed[i] = UInt8.random(in: 0...UInt8.max) }
        #if DEBUG
        // Dev hook (owner: "a fixed seed, enabled by some flags - dev build
        // flags only"): a `dev.seed` file in the App Group container pins the
        // genesis deal, so a verification run can choose a deal where the
        // CREATOR opens the bout instead of hoping. Seed 3 is such a deal at
        // 2 players. A file, not a UserDefaults key: `defaults write` from
        // outside lands in the wrong domain and cfprefsd caches group prefs
        // until a reboot. Compiled out of every Release build.
        if let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: "group.cards.foolish.msg"),
           let raw = try? String(contentsOf: dir.appendingPathComponent("dev.seed"),
                                 encoding: .utf8),
           let n = UInt8(raw.trimmingCharacters(in: .whitespacesAndNewlines)) {
            seed = Data(repeating: n, count: 32)
        }
        #endif
        let gameId = UInt64.random(in: 1...UInt64.max)
        let capacity = chatIsDM ? 2 : 8
        do {
            try await MessageKernel.shared.newGame(seed: seed, players: capacity)
            let joins = [MessageJoin(seat: 0, name: nickname)]
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: 0, gameId: gameId,
                parent8: Data(repeating: 0, count: 8), joins: joins)
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: 0, env: env, payload: payload)
            lobby = Lobby(env: env, payload: payload)
            await onSend(payload, 0, false)
            surfaceStaged = true   // round-9: the created lobby awaits Send
        } catch {
            damaged = true
        }
    }

    /// My seat in a lobby, or nil if I have not claimed one yet (§6). Note 14:
    /// gated through `SeatIdentity.resolveInLobby`, not the plain `resolve` the
    /// live board uses — see that function's doc for the bug this closes (a
    /// stale lobby bubble granting Start/Send to a seat it doesn't list, and
    /// the flip side, a fresh join not showing as joined). This used to add
    /// that note 15's Rule-P-for-lobbies fix in `load()` showed the NEWEST
    /// bubble here in the first place; round 7 removed that (and its cache),
    /// so what arrives here is exactly the bubble that was tapped, stale or
    /// not - which is precisely why the membership gate below has to hold.
    ///
    /// Bubble-anchored lookup (`seatForBubble`): this env came off a real
    /// bubble, whose gameId identifies my seat even after a group-membership
    /// change re-keyed the chat. `recordedName` extends note 14's membership
    /// gate by name: a lobby carrying someone ELSE's name at my cached seat is
    /// a claim race this device lost - nil here brings the Join button back so
    /// I re-claim the next free seat instead of squatting on theirs. Round 7
    /// stores no claim-time name; the device nickname is what my own Join
    /// sealed (see adopt()'s note), used only once actually set.
    private func lobbySeat(_ env: MessageEnvelope) -> Int? {
        SeatIdentity.resolveInLobby(
            cachedSeat: MessageGameStore.shared.seatForBubble(gameId: env.gameId),
            senderIsLocal: senderIsLocal, nPlayers: env.nPlayers,
            lastActorSeat: env.lastActorSeat, joins: env.joins, chatIsDM: chatIsDM,
            recordedName: MessageGameStore.shared.hasSetNickname
                ? MessageGameStore.shared.nickname : nil)
    }

    /// Claim the lowest free seat (§5.2, lobby v3). Always reseals WAITING and
    /// stays in the lobby — joining NEVER starts the game, no matter how many
    /// have joined or that the lobby's own capacity (8 for a group, 2 for a DM
    /// — see `createWaiting`) is reached; Start (below) is the one, explicit
    /// action that flips the game LIVE. Auto-stages the reseal (notes 14/16):
    /// the human still presses Messages' own Send, there is no separate "Send
    /// invite" button.
    private func joinLobby(_ lob: Lobby, nickname: String) async {
        let env = lob.env
        guard let free = (0..<env.nPlayers).first(where: { s in !env.joins.contains { $0.seat == s } }),
              let gid = UInt64(env.gameId) else { return }
        // Round-5 B1: LobbyView's join button is only reachable from its
        // `.ok` branch, so `nickname` is already NicknameGate-valid and
        // trimmed — the "You" fallback that used to live here is unreachable
        // now. Re-check defensively anyway and, per M2, fall back to the
        // STORED nickname (never a placeholder) if it somehow is not.
        let nick: String
        if case .ok(let name) = NicknameGate.check(nickname) {
            nick = name
        } else {
            nick = MessageGameStore.shared.nickname
        }
        // Names must stay unique WITHIN a chain (they are the only identity
        // the payload carries, §6 — see NicknameGate.isTaken). LobbyView's
        // join button already refuses a taken name; this re-check covers the
        // fallback path above landing on a stored nickname that collides.
        guard !NicknameGate.isTaken(nick, in: env.joins) else { return }
        MessageGameStore.shared.nickname = nick   // remember it for the next game (B3)
        let joins = (env.joins + [MessageJoin(seat: free, name: nick)]).sorted { $0.seat < $1.seat }
        do {
            // Re-adopt the lobby so the LOCKED seed + open capacity are resident
            // for the seal.
            _ = try await MessageKernel.shared.decode(payload: lob.payload, viewer: -1)
            let parent = MessageTurnController.firstEight(hex: env.digest)
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: free, gameId: gid, parent8: parent, joins: joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: free, env: newEnv, payload: payload)
            await onSend(payload, free, false)
            surfaceStaged = true   // round-9: the join reseal awaits Send
            lobby = Lobby(env: newEnv, payload: payload)
        } catch {
            damaged = true
        }
    }

    /// Leave the lobby (round 16): reseal it WITHOUT me and stage that, so the
    /// thread's newest bubble is a table I am no longer at.
    ///
    /// SEATS ARE COMPACTED, not holed. Start deals at `joins.count` and the
    /// kernel seats 0..<n contiguously (`fio_reseat_game`), an invariant the
    /// whole lobby rests on - "seats are claimed lowest-first, so it is always
    /// a contiguous 0..<n". A hole would seal a join whose seat is >= the
    /// dealt player count and simply not replay. Compacting preserves the
    /// CYCLE, which is all the seat numbers ever meant; everyone finds
    /// themselves again by name (SeatIdentity.seatClaimedByName), and the
    /// numbers were never identity.
    ///
    /// WHAT `lastActorSeat` BECOMES. It has to be a seat, and mine no longer
    /// exists - so it points at the first FREE slot, which after a compaction
    /// is always in range and is never a seated player. That matters twice:
    /// nobody left behind is wrongly read as "you sent the newest bubble" and
    /// withheld from Start (M9), and "the actor is not in the joins" is exactly
    /// how a reader tells a leave from a join.
    ///
    /// THE RACE, ACCEPTED (owner's call). If someone taps Start off the lobby
    /// that still lists me at the same moment I leave, one of the two is
    /// silently dropped: Messages hands every device whichever bubble arrives
    /// last, there is no way to read past it, and Rule P ranks the fuller
    /// roster higher - so a device already sitting on the lobby keeps showing
    /// me until it reopens the newer bubble. No priority scheme is layered on
    /// top of that; it would only be a second opinion about an order the
    /// platform has already decided.
    private func leaveLobby(_ lob: Lobby) async {
        let env = lob.env
        guard let me = lobbySeat(env), let gid = UInt64(env.gameId) else { return }
        guard LobbyControls.canExit(mySeat: me, joined: env.joins.count) else { return }
        let myName = env.joins.first { $0.seat == me }?.name ?? ""

        let remaining = env.joins.filter { $0.seat != me }.sorted { $0.seat < $1.seat }
        let joins = remaining.enumerated().map { MessageJoin(seat: $0.offset, name: $0.element.name) }
        guard !joins.isEmpty else { return }

        do {
            // Re-adopt so the LOCKED seed and the open capacity are resident.
            _ = try await MessageKernel.shared.decode(payload: lob.payload, viewer: -1)
            let parent = MessageTurnController.firstEight(hex: env.digest)
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: joins.count, gameId: gid,
                parent8: parent, joins: joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            // Forget the seat I no longer hold, or the next open of this game
            // would resolve me back into a lobby I left.
            MessageGameStore.shared.forgetSeat(gameId: env.gameId)
            AnimLog.say("lobby exit: \(myName) left, \(joins.count) remain")
            // The sender names the leaver in the transcript line; the envelope
            // cannot (the join that carried the name is exactly what was
            // removed), so the one device that still knows says it.
            onAnnounceLeave(myName)
            await onSend(payload, joins.count, false)
            surfaceStaged = true
            lobby = Lobby(env: newEnv, payload: payload)
        } catch {
            damaged = true
        }
    }

    /// THE RULES THE TABLE HAS AGREED, per lobby (keyed by game id): the
    /// passing value on the newest bubble that somebody ELSE put on the chain.
    /// `LobbyControls.rulesChanged` compares it with what the lobby says now to
    /// answer "have I just changed this", which is what withholds Start from
    /// whoever moved the checkbox.
    ///
    /// A dictionary rather than one value because a chat can hold more than one
    /// lobby, and this view is reused across them; it is small (one Bool per
    /// game this device has looked at) and dies with the extension.
    @State private var passingBaseline: [String: Bool] = [:]

    /// Adopt a lobby bubble's rules as the agreed baseline - unless it is MINE,
    /// in which case it may be the change itself and the older agreement still
    /// stands. Called wherever a lobby arrives from the chain.
    private func noteRulesBaseline(_ env: MessageEnvelope) {
        guard env.lastActorSeat != lobbySeat(env) else { return }
        passingBaseline[env.gameId] = env.passingAllowed
    }

    /// CHANGE THE TABLE'S RULES: reseal this lobby with the passing checkbox
    /// moved, and stage that, so the change reaches everyone the same way a
    /// join does - as a bubble on the chain.
    ///
    /// It is `joinLobby` with the roster left alone: re-adopt the lobby (the
    /// locked seed and the open capacity have to be resident to seal), tell the
    /// kernel the rule, seal, stage. `lastActorSeat` is mine, which is what
    /// takes Start away from me until somebody else acts - the owner's rule,
    /// and the reason the reseal is a real bubble rather than a local flag: the
    /// others must be able to see the rules they are about to play under before
    /// anyone can start.
    ///
    /// A no-op if I hold no seat (the checkbox is disabled there anyway - a
    /// reseal has to name an actor seat) or if the rule is already what was
    /// asked for, so a double tap cannot stage a bubble that changes nothing.
    ///
    /// ONE AT A TIME (round 21). The checkbox now moves the instant it is
    /// touched (`LobbyView.passingWish`), which makes it easy to tap twice
    /// before the first reseal has landed - and two of these running at once
    /// would interleave through the kernel actor and seal each other's rule.
    /// `passingStaging` holds the lane and `passingWanted` holds the newest
    /// request, so taps COLLAPSE: whatever the box says when the lane frees is
    /// what gets sealed, and every tap in between costs nothing.
    private func setLobbyPassing(_ lob: Lobby, passing: Bool) async {
        passingWanted = passing
        guard !passingStaging else { return }
        passingStaging = true
        defer { passingStaging = false }
        // Re-read `lobby` each pass rather than trusting the `lob` this call was
        // handed: an earlier iteration has already replaced it, and staging
        // against the payload from before that would fork the chain.
        while let want = passingWanted {
            passingWanted = nil
            guard let current = lobby else { return }
            await stageLobbyPassing(current, passing: want)
        }
    }

    /// True while a rules reseal is in the kernel. See `setLobbyPassing`.
    @State private var passingStaging = false
    /// The newest rule asked for while the lane was busy, or nil for none.
    @State private var passingWanted: Bool?

    /// One rules reseal, start to finish. Always called from the single lane
    /// `setLobbyPassing` owns.
    private func stageLobbyPassing(_ lob: Lobby, passing: Bool) async {
        let env = lob.env
        guard let me = lobbySeat(env), let gid = UInt64(env.gameId) else { return }
        guard env.passingAllowed != passing else { return }
        do {
            // Remember what the table had agreed BEFORE this change, unless
            // this device has already staged one on this lobby (then the
            // baseline is still the older, agreed value - see
            // LobbyControls.rulesChanged, and note that ticking the box back
            // must clear the gate rather than double it).
            if passingBaseline[env.gameId] == nil {
                passingBaseline[env.gameId] = env.passingAllowed
            }
            // Decode, set, seal - ONE actor call (round 21). Three separate
            // hops left two suspension points in which any other decode could
            // repoint the resident game, which is the phantom-seal shape all
            // over again; see `MessageKernel.resealLobby`.
            let parent = MessageTurnController.firstEight(hex: env.digest)
            let payload = try await MessageKernel.shared.resealLobby(
                lob.payload, passing: passing, actingSeat: me,
                gameId: gid, parent8: parent, joins: env.joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            AnimLog.say("lobby rules: passing=\(passing) by seat \(me)")
            cache(seat: me, env: newEnv, payload: payload)
            await onSend(payload, me, false)
            surfaceStaged = true
            lobby = Lobby(env: newEnv, payload: payload)
        } catch {
            damaged = true
        }
    }

    /// The lobby's "Add player" action, or nil when solo seating is not
    /// compiled in — one `#if` here instead of one at the call site, so the
    /// view code above reads the same in every configuration.
    private func soloSeatAction(_ lob: Lobby) -> (() -> Void)? {
        #if DEBUG || SOLO_TESTING
        return { Task { await addSoloSeat(lob) } }
        #else
        return nil
        #endif
    }

    #if DEBUG || SOLO_TESTING
    /// Testing-only (MessageDebugFlags.soloSeats): seat a PUPPET player from this
    /// device so a lobby with nobody else in the chat can still reach two seats
    /// and start. Mechanically `joinLobby` minus the two things that would be
    /// wrong here:
    ///
    ///   - it does NOT overwrite this device's stored nickname (the puppet is
    ///     not me renaming myself — I keep my own name on my own seat), and
    ///   - it does NOT re-cache MY seat as the puppet's, so identity stays
    ///     whatever it already was; `pickSeatOnAdopt` is what switches which
    ///     hand you are playing, one bubble at a time.
    ///
    /// It also deliberately does not stage/send the reseal: a puppet is local
    /// scaffolding, and `startGame` seals the LIVE handoff off this same
    /// in-memory lobby payload, so the chat only ever sees the real game.
    private func addSoloSeat(_ lob: Lobby) async {
        let env = lob.env
        guard let free = (0..<env.nPlayers).first(where: { s in !env.joins.contains { $0.seat == s } }),
              let gid = UInt64(env.gameId) else { return }
        let keepSeat = lobbySeat(env) ?? 0
        let joins = (env.joins + [MessageJoin(seat: free, name: "Solo \(free + 1)")])
            .sorted { $0.seat < $1.seat }
        do {
            _ = try await MessageKernel.shared.decode(payload: lob.payload, viewer: -1)
            let parent = MessageTurnController.firstEight(hex: env.digest)
            let payload = try await MessageKernel.shared.seal(
                phase: 0, lastActorSeat: free, gameId: gid, parent8: parent, joins: joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: keepSeat, env: newEnv, payload: payload)
            lobby = Lobby(env: newEnv, payload: payload)
        } catch {
            damaged = true
        }
    }
    #endif

    /// Start the game at the ACTUAL joined count (§5.2, lobby v3). Any JOINED
    /// player may do this once 2+ have joined (LobbyView gates the button on
    /// that; nothing re-checks it here — the kernel would happily reseat and
    /// seal a 1-player "game" too, but the design never offers the button for
    /// it). Re-derives the resident game from the seed LOCKED at create, at
    /// `joins.count` seats — contiguous 0..<k because seats are always claimed
    /// lowest-free-first — then seals the LIVE handoff (turn 0, parent8 =
    /// first8(lobby digest), the same joins) and drops the starter onto the
    /// board: mechanically identical to what the OLD "last joiner auto-starts"
    /// branch of `joinLobby` used to do, just triggered explicitly instead of
    /// implicitly by seat count. Uses the shared `MessageKernel.startFromLobby`
    /// primitive so this reseat/seal is provably the deal locked at create.
    private func startGame(_ lob: Lobby) async {
        let env = lob.env
        guard let seat = lobbySeat(env), let gid = UInt64(env.gameId) else { return }
        do {
            let parent = MessageTurnController.firstEight(hex: env.digest)
            let payload = try await MessageKernel.shared.startFromLobby(
                lobbyPayload: lob.payload, gameId: gid, actingSeat: seat,
                parent8: parent, joins: env.joins)
            let newEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            cache(seat: seat, env: newEnv, payload: payload)
            await onSend(payload, seat, false)
            surfaceStaged = true   // round-9: the LIVE handoff awaits Send (alsoStaged)
            controller = MessageTurnController(parentPayload: payload, parent: newEnv, mySeat: seat)
            lobby = nil
        } catch {
            damaged = true
        }
    }

    /// Adopt `winner` as the game and open the board.
    private func adopt(winner: Data, env: MessageEnvelope) async {
        // A WAITING envelope is an INVITE, and this function opens a BOARD. They
        // are never interchangeable: a lobby seal leaves a game dealt at the
        // lobby's CAPACITY resident (8 for a group chat — see `createWaiting`),
        // so adopting one as a board shows a phantom 8-player game whose unjoined
        // seats read "Seat N", with a different first attacker than the real
        // game — the round-3 "some see a 5-player game, some see 8" fork, which
        // deadlocks the thread. Rule P now ranks any started chain above a lobby
        // (msg_rule_p rule 0), so nothing should reach here at phase 0 any more;
        // this is the structural guarantee behind that, not a second opinion
        // about which chain wins.
        if env.phase == 0 {
            controller = nil
            noteRulesBaseline(env)
            lobby = Lobby(env: env, payload: winner)
            return
        }
        // Round-9 #5: is this the chain THIS DEVICE just pressed Send on? The
        // send can tear the extension down (dismiss / VC swap), so the reopen
        // arrives here as a cold load of my own bubble - without this it
        // REPLAYED the move I had just watched myself play. One-shot: consumed
        // (cleared) whether it matches or not, so a stale marker can never
        // silence a later genuine replay.
        let justSent = MessageGameStore.shared.consumeJustSent(matching: winner)
        // ROUND 20: is this bubble the latest this device has seen of this game,
        // or a branch off something older? Asked BEFORE any early return below,
        // so every route to a board carries the same answer, and stored in
        // `@State` because `seatOnBoard` is where the controller finally exists
        // (and is reached from the name gate and the seat picker too).
        staleBranch = await rankAgainstHighWater(winner, env: env)
        // Make the resident game the winner (the round guard/ledger it used to set
        // are gone with Rule R).
        _ = try? await MessageKernel.shared.decode(payload: winner, viewer: -1)
        #if DEBUG || SOLO_TESTING
        // Single-simulator harness: both conversations share ONE App Group cache
        // and participant identity, so a received bubble always resolves to the
        // SENDER's seat and you can never view the receiver ("Waiting for Seat 2"
        // while you ARE seat 2). In DEBUG, ask who you are so both seats are
        // playable on one sim. Release resolves automatically (real devices have
        // separate caches + distinct participant UUIDs) and never shows this.
        if MessageDebugFlags.pickSeatOnAdopt { controller = nil; ambiguous = (env, winner); return }
        #endif
        // ROUND 9 (owner): the durable pending ledger and its Rule R rebase are
        // REMOVED ("caching has caused A LOT of problems... drop the pending
        // ledger altogether"). An adopt no longer replays any stored moves - a
        // staged-but-unsent move survives only in the live controller, and in
        // the staged input-field bubble itself.

        // Bubble-anchored seat (seatForBubble): the winner chain's gameId
        // identifies this device's seat even after a group-membership change
        // re-keyed the chat (round 7 keeps only the seat per game, so the seat
        // IS the whole lookup now). Seat resolution then leans on the roster's
        // NAMES, both ways:
        //  - recovery (seatClaimedByName): the seat carrying MY claim name in
        //    THIS chain is my seat here, even when a fork race left the numeric
        //    cache pointing at a claim that lost (the flow simulator's
        //    convergence/liveness stall);
        //  - the ghost guard (cacheDisownedByJoins): a roster listing somebody
        //    ELSE's name at my cached seat means my claim lost - trusting the
        //    number would put that person's hand face-up on my screen. Disowned
        //    with no name to recover reads as no-cache: §6.2's exact signals,
        //    else the Release spectator board.
        // Round 7 stores no claim-time name; the device nickname is what was
        // sealed into MY join (it only diverges if the human renamed since -
        // §6.3's trust level either way), and it only counts once actually set.
        let numericSeat = MessageGameStore.shared.seatForBubble(gameId: env.gameId)
        let recorded: String? = MessageGameStore.shared.hasSetNickname
            ? MessageGameStore.shared.nickname : nil
        let cachedSeat: Int? = SeatIdentity.seatClaimedByName(recordedName: recorded, joins: env.joins)
            ?? (SeatIdentity.cacheDisownedByJoins(cachedSeat: numericSeat, recordedName: recorded,
                                                  joins: env.joins) ? nil : numericSeat)
        switch SeatIdentity.resolve(cachedSeat: cachedSeat,
                                    senderIsLocal: senderIsLocal,
                                    nPlayers: env.nPlayers, lastActorSeat: env.lastActorSeat,
                                    chatIsDM: chatIsDM) {
        case .known(let seat):
            // §B3: a player about to be seated who has never chosen a name is
            // asked once. Since lobby v3 everyone named themselves at setup or
            // the lobby's Join field, so this fires only on §6.2 cache-loss
            // recovery (reinstall/second device — the nickname went with the
            // cache), at any player count; it never re-asks once stored.
            if !MessageGameStore.shared.hasSetNickname {
                controller = nil
                nameGate = NameGate(env: env, payload: winner, seat: seat,
                                    quietOpen: justSent)
            } else {
                seatOnBoard(seat: seat, env: env, winner: winner, quietOpen: justSent)
            }
        case .ambiguous:
            controller = nil
            #if DEBUG || SOLO_TESTING
            // Single-simulator testing keeps the real picker (see the DEBUG note
            // above in this function) — this branch is unreachable in DEBUG anyway
            // because `pickSeatOnAdopt` already returned above, but stays correct
            // if that flag is ever turned off. Round 20: unless the rig asks for
            // the Release route, which is the only way that screen can be
            // reached on a debug build at all - see `spectateWhenAmbiguous`.
            if !MessageDebugFlags.spectateWhenAmbiguous {
                ambiguous = (env, winner)
                return
            }
            #endif
            // RELEASE SECURITY: an ambiguous identity must never offer a seat
            // picker — anyone could claim any hand and see it. Show the same
            // PUBLIC spectator board a delivered bubble's snapshot uses instead
            // (§10, MessageBoardView is public-safe by construction). `winner` was
            // already decoded/adopted above, so the resident game IS this chain.
            let names = Dictionary(env.joins.map { ($0.seat, $0.name) }, uniquingKeysWith: { a, _ in a })
            // Round 20 read the §12 funnel code HERE rather than on the tap,
            // because the resident game is this chain at THIS moment and by the
            // time anyone taps the link something else may have been decoded
            // over it. Round 22 finishes that thought: the board and the code
            // were still two separate trips into the kernel, so the same
            // interloper could land BETWEEN them and hand a watcher one game's
            // table with another game's replay link. `readBoard` rebuilds this
            // chain and answers both in one call - viewer -1 is the public
            // table, which is all a watcher may see anyway.
            if let read = try? await MessageKernel.shared.readBoard(
                    .continuation(payload: winner), replaying: [], seat: -1, sentAt: 0),
               let view = read.view {
                // …with the roster attached, the same as a seated player's link
                // (MessageTurnController.replayURL): a watcher who shares the
                // finished game should not hand out a link that has forgotten
                // who played it. `names` is the joins of the chain that was just
                // decoded, `view` the table it built.
                spectatorReplayURL = read.replayCode.map {
                    MessageEnvelope.replayLink(
                        code: $0,
                        names: ReplayExtras.seatNames(names, count: view.numPlayers))
                }
                spectator = (view, names)
                // ROUND 21: a FINISHED chain also gets a real board to watch the
                // last move on, seated at nobody's seat - see `spectatorBoard`.
                // The still picture stays behind it as the running-game case and
                // as the fallback if the board cannot be built.
                spectatorBoard = view.isOver
                    ? MessageTurnController(parentPayload: winner, parent: env, mySeat: -1)
                    : nil
            } else {
                damaged = true
            }
        }
    }

    /// Open the board for a resolved seat: cache it and hand the winner chain
    /// to a fresh controller. The tail of `adopt`'s `.known` branch, shared
    /// with the name gate. `quietOpen` (round-9 #5): this is my own just-sent
    /// chain, so its last move - mine, watched live - is not replayed.
    private func seatOnBoard(seat: Int, env: MessageEnvelope, winner: Data,
                             quietOpen: Bool = false) {
        cache(seat: seat, env: env, payload: winner)
        // ROUND 12: same game, same seat, board already up -> hand the new chain
        // to the LIVE controller instead of replacing it.
        //
        // The board is keyed on the controller's identity (`expandedContent`'s
        // `.id`), so replacing the controller throws the board away and builds a
        // new one - fresh `@State`, unmeasured geometry, a first paint at
        // defaults. That teardown is what the owner sees as the board flashing
        // when a move arrives on an expanded screen. Nothing about an arriving
        // bubble requires a new board: the seat is the same, the game is the
        // same, only the chain moved on, and `adopt` moves exactly that.
        //
        // A DIFFERENT game (or a different seat in one) still gets a fresh
        // controller - there the teardown is honest, because it really is a
        // different board.
        if let live = controller, live.canAdopt(seat: seat, gameId: env.gameId) {
            // Round 20: re-asked on every adopt, in BOTH directions - the newest
            // bubble arriving on a stale board is what hands it the right to
            // play again, and it must not have to be re-tapped for that.
            live.setSuperseded(staleBranch)
            // OFFERED, not forced (the conflict model, 1.0(28)): a chain
            // arriving over a staged move is visibly retracted first - the
            // staged cards fly home in red against the OLD base - and adopted
            // only when that lands. With nothing staged this is `adopt` as it
            // always was.
            Task { await live.offerArrival(payload: winner, parent: env, quietOpen: quietOpen) }
            return
        }
        let fresh = MessageTurnController(parentPayload: winner, parent: env, mySeat: seat,
                                          suppressOpenReplay: quietOpen)
        fresh.setSuperseded(staleBranch)
        controller = fresh
    }

    /// The human answered the name gate: persist the name, then seat them. The
    /// name is baked into `joins` when they first play (sealJoins). Round-5
    /// B1: NameGateView's Continue/onSubmit are only reachable from their
    /// `.ok` branch, so `raw` is already NicknameGate-valid and trimmed — the
    /// "call me the default" blank fallback this used to have is gone (see
    /// NameGateView's own doc). Re-check defensively anyway and, per M2, fall
    /// back to the STORED nickname (never a placeholder) if it somehow is not
    /// — skipping the write below just leaves whatever this device already
    /// had on file.
    private func nameThenSeat(_ raw: String, gate g: NameGate) async {
        if case .ok(let name) = NicknameGate.check(raw) {
            MessageGameStore.shared.nickname = name
        }
        nameGate = nil
        seatOnBoard(seat: g.seat, env: g.env, winner: g.payload, quietOpen: g.quietOpen)
    }

    /// §6.3 pick resolved: remember the seat, then play. DEBUG-only
    /// single-simulator path (never compiled into Release). It used to be
    /// described as skipping the "open-delta-replay hint" that `adopt` looks
    /// up; round 43 established there was never a lookup to skip - the hint
    /// was nil at its only origin and read nowhere - so this picker opens
    /// exactly the same board every other path does.
    private func choose(seat: Int, from a: (env: MessageEnvelope, payload: Data)) async {
        cache(seat: seat, env: a.env, payload: a.payload)
        let c = MessageTurnController(parentPayload: a.payload, parent: a.env, mySeat: seat)
        c.setSuperseded(staleBranch)   // round 20 - see seatOnBoard
        controller = c
        ambiguous = nil
    }

    /// Round 7: persist ONLY this device's seat (§6.1). The preferred-chain
    /// payload, denormalized display fields and pending ledger the old record
    /// carried are gone — the extension always renders the tapped bubble now, so
    /// the one thing worth keeping is which seat is me in this game.
    private func cache(seat: Int, env: MessageEnvelope, payload: Data) {
        MessageGameStore.shared.setSeat(gameId: env.gameId, chatKey: chatKey, seat: seat)
    }
}

/// New game setup (§5.2, rewritten for lobby v3 — notes 2/19/25). The creator
/// names themselves (B3 — the one place a nickname is entered; compact is the
/// keyboard area and cannot host a field, §3.5). There is no player-count
/// picker any more: it was off-theme (a segmented `Picker` reads as glass, not
/// wood/wool) AND wrong, per the owner's own framing — "New game should just
/// stage the new game, lobby style, with unspecified player count until
/// someone hits start".
///
/// Lobby v3 (note 2) unified DM and group behind ONE path: "Create game"
/// always opens a lobby (LobbyView), never a straight-to-board deal — a DM
/// used to deal LIVE immediately here, which let the creator reroll a bad
/// hand by tapping New game until the deck favored them, since nothing
/// committed the seed until they'd already seen it. A DM's lobby capacity is
/// just 2 (`GameSurface.createWaiting`), so "Players: 2" is still shown as a
/// fact, not a picker — nobody has joined yet, and nobody needs to pick a
/// count: whoever has joined when someone taps Start (or Join and start) IS
/// the player count (§5.2/lobby v3).
private struct NewGameSetup: View {
    @State private var nickname: String
    let isDM: Bool
    /// No longer displayed (the picker it used to size is gone). Kept only so
    /// this struct's callers — this file's own call site, the harness, and
    /// MessagesViewController, all of which compute a real participant count —
    /// keep compiling unchanged (source compatibility, no Swift compiler here
    /// to re-check call sites across targets).
    let chatPlayers: Int
    let onStart: (String) -> Void

    init(nickname: String, isDM: Bool, chatPlayers: Int, onStart: @escaping (String) -> Void) {
        _nickname = State(initialValue: nickname == "Me" ? "" : nickname)
        self.isDM = isDM
        self.chatPlayers = chatPlayers
        self.onStart = onStart
    }

    /// Round-5 B1: the three-state verdict on the CURRENT field text, driving
    /// both the Create-game button's label/enabled state and — via `.ok` —
    /// the exact trimmed name `onStart` is called with. A name that fails
    /// either of NicknameGate's caps is REJECTED here, in the UI, rather than
    /// lighting the button up and failing downstream at the seal layer as
    /// "this game link is damaged" (B1's actual bug).
    private var nameVerdict: NicknameGate.Verdict { NicknameGate.check(nickname) }

    var body: some View {
        VStack(spacing: 16) {
            // Round-6 #17: `onTableText` (Tokens.swift) is the wool half of
            // the wood/wool text pairing, thickened per the owner's ask.
            Text(FStrings.t("ios.msg.newgame")).font(.headline).onTableText()
            // Round-7 #1: the "Your name" label is dropped - the field's own
            // "your nickname" placeholder already says what it is, and the two
            // together were redundant. The placeholder carries it alone now.
            TextField(FStrings.t("ios.msg.nickname_ph"), text: $nickname).textFieldStyle(.roundedBorder)
            switch nameVerdict {
            case .ok(let name):
                FButton(FStrings.t("ios.msg.creategame"), kind: .wood) { onStart(name) }
            case .empty:
                FButton(FStrings.t("ios.msg.entername"), kind: .wood, enabled: false) {}
            case .tooLong:
                FButton(FStrings.t("ios.msg.nametoolong"), kind: .wood, enabled: false) {}
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

/// The WAITING lobby, rewritten for lobby v3 (§5.2/§5.3, docs/IMESSAGE_LOBBY_V3.md,
/// notes 2/14/15/16): an OPEN lobby, not a fixed seat count. `env.nPlayers` is
/// the lobby's CAPACITY — 8 for a group (the wire's max) or 2 for a DM (see
/// `GameSurface.createWaiting`) — display convention only, never rendered as N
/// literal seats: the joined list IS the player count so far, and the game's
/// real size is decided at Start, not now.
///
/// What a viewer can do: Join (name + button, if I have not claimed a seat and
/// the lobby has room), or Start (once I'm already joined and 2+ have —
/// round-5 M9 narrows "any joined player may" to "any joined player except
/// whoever sent the newest bubble, unless the lobby is full" — see
/// `LobbyControls.offered`). Owner decision (this pass): there is NO combined
/// "Join and start" button — a joiner either Joins (which stages the WAITING
/// lobby for the human to send) or, being already joined, taps Start (which
/// stages the LIVE game). Two distinct texts, never one fused action. There is
/// likewise no "Send invite" button (notes 14/16): creating and joining both
/// AUTO-STAGE the reseal, so the human's very next tap is Messages' own Send.
/// WHICH control a lobby offers, pulled out of `LobbyView` as a pure function
/// for one reason: the view had a state with NO control at all — joined, but
/// fewer than two players, so no Start (needs 2), no Join (already in), no
/// invite (notes 14/16 removed that button as redundant with the auto-stage).
/// A lobby listing one player and offering nothing is a dead end, and it is
/// invisible in a `if/else if/else` chain until someone lands in it. As an enum
/// there is always exactly one answer, and a test can enumerate every
/// (mySeat, joined, capacity) and assert so.
public enum LobbyControls {
    /// Start the game at the joined count. Round-5 M9 narrows this further —
    /// it is withheld from whoever sent the newest bubble while the lobby
    /// still has room (see `offered`'s doc): "2+ have joined" is no longer
    /// sufficient on its own.
    case start
    /// Re-stage the WAITING chain so the human can send the invite (I'm in,
    /// nobody else is yet, and the newest invite is NOT mine).
    case invite
    /// Nothing to do but wait. Two distinct situations render this way
    /// (round-5 M9 added the second one):
    ///  1. I'm in, nobody else is yet, and the invite sitting at the head of
    ///     this chain is the one I put there — unchanged from before M9.
    ///  2. I'm in, 2+ have joined, the lobby still has room, and the newest
    ///     bubble on the chain is mine (I just joined, or just re-staged the
    ///     invite) — M9's whole point: I cannot also be the one who starts.
    /// Both read the same "Waiting for the others" text; the owner explicitly
    /// ruled out a capacity/"N of M" line to tell them apart (M9 — "no
    /// capacity text, too confusing").
    case waiting
    /// Claim a seat (I'm not in, and there is room).
    case join
    /// Nothing to do but wait (I'm not in, and there is no room).
    case full

    /// `iSentTheInvite`: is the newest bubble on this chain one I staged or
    /// sent (`lastActorSeat == mySeat`)? Kept its round-4 name even though
    /// round-5 widens what it gates (below): it is public API and
    /// `Round4Tests.swift` already binds this exact argument label, so
    /// renaming it would break that file's BUILD, not just one of its
    /// assertions, over a docstring nicety.
    ///
    /// Round-4 note 1 — "if you were the last one to send an invite,
    /// shouldn't have the Send invite pop up." Offering it then asks the
    /// human to send a second copy of the invite already sitting in the
    /// thread (or in the compose field, freshly auto-staged), which is the
    /// state a creator lands in every single time.
    ///
    /// The trade this makes, deliberately and with the owner's call on it: the
    /// `.invite` button exists as the recovery path for a lobby whose
    /// auto-staged bubble is gone (sent, deleted from the compose field, or
    /// the extension reopened later). Gating it on authorship means a creator
    /// who deletes their own draft has no in-lobby way to re-stage it and must
    /// use New game. That is the cost of not nagging everyone else.
    ///
    /// Round-5 M9 — "if you were the last to send one of those join texts,
    /// you can't send a start text... that will make it a bit more difficult
    /// to lock people out." Extends the SAME authorship check to `.start`:
    /// once 2+ have joined, whoever sent the newest bubble (the last joiner,
    /// or whoever last re-staged the invite) is withheld from Start too, as
    /// long as the lobby still has room — so whoever is currently able to act
    /// is never the same person who could instead invite one more player in.
    ///
    /// EXEMPTION: a FULL lobby (`joined == capacity`) always offers Start to
    /// its last joiner regardless of authorship. Nobody else could join
    /// instead, so withholding Start there would just strand a full lobby
    /// with no way forward — and in a 2-player DM (capacity 2) it would force
    /// an extra, pointless round-trip into every single game: the joiner
    /// filling the last seat immediately starting is the designed "join and
    /// start" flow (note 2), not the lockout M9 is guarding against.
    /// `iChangedTheRules`: the newest bubble on this chain is MINE and it moved
    /// the passing checkbox (see `rulesChanged`). The owner's rule for the
    /// variant, in their words: "whoever changes the checkbox value cannot
    /// start the game, similar to how last joined cannot start the game."
    ///
    /// It is the M9 gate WITHOUT the full-lobby exemption, and the exemption's
    /// own reasoning is why. That exemption exists so a full lobby is never
    /// stranded: nobody else could join, so withholding Start from its last
    /// joiner would leave a table with no way forward. A rules change strands
    /// nothing - the reseal is sendable, and whoever opens it can start
    /// immediately - so the exemption has no work to do here, while the thing
    /// it would allow is exactly what the rule forbids: in a two-player DM
    /// (capacity 2, full the moment both are in) the changer could otherwise
    /// flip the rules and start in the same breath, and their opponent would
    /// first learn of it from a board that will not let them pass.
    public static func offered(mySeat: Int?, joined: Int, capacity: Int,
                               iSentTheInvite: Bool = false,
                               iChangedTheRules: Bool = false) -> LobbyControls {
        if mySeat != nil {
            if joined >= 2 {
                if iChangedTheRules { return .waiting }
                if iSentTheInvite && joined < capacity { return .waiting }
                return .start
            }
            return iSentTheInvite ? .waiting : .invite
        }
        return joined < capacity ? .join : .full
    }

    /// Did THIS device change the rules on the lobby it is showing?
    ///
    /// `baseline` is the passing rule as of the last bubble somebody ELSE put
    /// on this chain (nil if there has been none - a lobby this device
    /// created), `current` is what the lobby says now, and `mine` is whether
    /// the newest bubble is this device's.
    ///
    /// Asked this way, and not as a "I tapped the box" flag, for two reasons.
    /// It is SELF-CANCELLING: a player who ticks the box and thinks better of
    /// it lands back on the rules everyone else already has, and there is
    /// nothing left to withhold Start for. And it is answered by the CHAIN
    /// rather than by a memory of a tap, so it survives the extension being
    /// closed and reopened mid-lobby, which a flag would not.
    public static func rulesChanged(baseline: Bool?, current: Bool, mine: Bool) -> Bool {
        guard mine, let baseline else { return false }
        return baseline != current
    }

    /// May I LEAVE this lobby? A seated player may, once somebody else is
    /// seated too.
    ///
    /// Orthogonal to `offered` on purpose, rather than a sixth case of it:
    /// leaving is available alongside Start (both, side by side) and alongside
    /// Waiting (exit alone), and folding two independent answers into one enum
    /// would need a case per combination. The owner's shape is exactly this -
    /// "start game and the exit game buttons side by side WHEN BOTH ARE
    /// POSSIBLE. Currently start game is not possible for the last player that
    /// joined. Thus they can only exit."
    ///
    /// THE 2+ FLOOR is the wire's, not a preference: a WAITING envelope must
    /// carry at least one join (MSG_EJOINS), so the last player standing has no
    /// bubble to leave INTO. A lone creator's exit is New game, which replaces
    /// the invite outright.
    public static func canExit(mySeat: Int?, joined: Int) -> Bool {
        mySeat != nil && joined >= 2
    }
}

private struct LobbyView: View {
    let env: MessageEnvelope
    /// Re-render this view when a setting changes (see FPrefs). Only the
    /// OBSERVATION matters - the strings still come from FStrings.t and the
    /// table surface still comes from FTextures.
    @ObservedObject private var prefs = FPrefs.shared
    let mySeat: Int?
    let onJoin: (String) -> Void
    let onStart: () -> Void
    /// Leave the lobby: reseal it without me and stage that. Round 16.
    let onExit: () -> Void
    /// Re-stage this same WAITING chain so the human can send the invite again
    /// — the recovery path for a lobby whose auto-staged bubble is gone (see
    /// the `mySeat != nil, joins < 2` branch in `body`).
    let onInvite: () -> Void
    /// Change the table's rules: reseal this lobby with the passing checkbox
    /// moved, and stage that. Whoever does it cannot then start the game (see
    /// `LobbyControls.rulesChanged`).
    let onSetPassing: (Bool) -> Void
    /// The passing rule as of the last bubble somebody ELSE put on this chain,
    /// or nil if there has been none. The lobby needs it to tell "the rules are
    /// what the table agreed" from "I have just changed them and not sent it
    /// yet" - see `LobbyControls.rulesChanged`.
    let passingBaseline: Bool?
    /// Testing-only (MessageDebugFlags.soloSeats): seat a puppet player from
    /// this device. nil in every shipping build — see `soloControls`.
    var onAddSoloSeat: (() -> Void)?

    /// The joiner's editable name (B3): compact can't host a field, so this is the
    /// place a joiner names themselves before claiming a seat. Seeded from the
    /// stored nickname, blank if it's the neutral default.
    @State private var nickname: String

    /// WHERE I JUST PUT THE TICK, ahead of the chain agreeing with me.
    ///
    /// Round 21, the owner: "in lobby, Passing checkbox is not very responsive,
    /// seems to wait for stage before it updates. Make the checkbox UI update
    /// FIRST, THEN stage the message." The box was drawn straight from
    /// `env.passingAllowed`, which is a fact about the newest BUBBLE - so the
    /// tick could not move until `setLobbyPassing` had re-decoded the lobby,
    /// re-sealed it, decoded that, and handed a new `Lobby` back. Every one of
    /// those is correct and none of them belongs between a finger and a tick.
    ///
    /// nil means "nothing of mine is outstanding - draw what the chain says",
    /// which is the state the box is in almost all the time. It is cleared the
    /// moment `env.passingAllowed` moves for ANY reason: my own reseal landing,
    /// or somebody else's bubble arriving with the other rule on it. So the
    /// wish can never outlive the truth, and a lost or rejected change heals by
    /// itself on the next paint rather than leaving the box lying.
    @State private var passingWish: Bool?

    init(env: MessageEnvelope, mySeat: Int?, nickname: String,
         onJoin: @escaping (String) -> Void,
         onStart: @escaping () -> Void,
         onExit: @escaping () -> Void = {},
         onInvite: @escaping () -> Void,
         onSetPassing: @escaping (Bool) -> Void = { _ in },
         passingBaseline: Bool? = nil,
         onAddSoloSeat: (() -> Void)? = nil) {
        self.env = env; self.mySeat = mySeat
        self.onJoin = onJoin; self.onStart = onStart; self.onExit = onExit
        self.onInvite = onInvite
        self.onSetPassing = onSetPassing
        self.passingBaseline = passingBaseline
        self.onAddSoloSeat = onAddSoloSeat
        _nickname = State(initialValue: nickname == "Me" ? "" : nickname)
    }

    /// What the box should be DRAWN as: my outstanding tap if there is one, the
    /// chain's answer otherwise.
    private var passingShown: Bool { passingWish ?? env.passingAllowed }

    /// Have I moved the checkbox on the bubble now at the head of this chain?
    ///
    /// A wish outstanding counts, and has to: this gate is what stops whoever
    /// changed the rules from also starting the game before anyone has seen the
    /// change, and round 21's optimistic tick opens a window where the box has
    /// moved but the reseal carrying it has not landed yet. Withholding Start
    /// for those few milliseconds is free; offering it is the exact thing the
    /// gate exists to prevent.
    private var iChangedTheRules: Bool {
        if passingWish != nil { return true }
        return LobbyControls.rulesChanged(baseline: passingBaseline,
                                          current: env.passingAllowed,
                                          mine: env.lastActorSeat == mySeat)
    }

    /// The lobby SCROLLS when it does not fit, and is centred when it does.
    ///
    /// It is shown in whatever height the drawer happens to have, and the tall
    /// case is real: a rematch at three or more players carries a roster, the
    /// fool's penalty in two lines and the rules checkbox, which together do not
    /// fit the COMPACT drawer - and the extension opens compact. Left to lay out
    /// unbounded it ran through the settings squares in the corner; simply
    /// clipping it instead truncated the penalty sentence to "Ann1 was the fool,
    /// so Ann1 gets attacked first -…", which is the half that matters. Both
    /// found on the simulator, 1.0(17).
    ///
    /// `minHeight: geo.size.height` is what keeps the SHORT lobby exactly where
    /// it was: the content is centred in a frame at least as tall as the drawer,
    /// so nothing moves until there is genuinely more content than room.
    var body: some View {
        GeometryReader { geo in
            ScrollView {
                content
                    .frame(maxWidth: .infinity, minHeight: geo.size.height)
            }
            .modifier(BounceOnlyWhenTooTall())
        }
    }

    private var content: some View {
        VStack(spacing: 12) {
            // Round-6 #17: `onTableText` (Tokens.swift).
            Text(FStrings.t("ios.lobby")).font(.headline).onTableText()
            // Joined players only — never env.nPlayers rows: an open lobby has
            // no "open seat" placeholders, because there is no fixed seat count
            // to fill (note 19/25's whole point, unchanged by v3).
            VStack(spacing: 6) {
                ForEach(env.joins.sorted { $0.seat < $1.seat }, id: \.seat) { j in
                    HStack {
                        // Round-5 M10: full-opacity ink + a light shadow, not
                        // 55% black (see DamagedView's sweep note). Round-6
                        // #17 thickened both columns, not just the seat number.
                        Text("\(j.seat + 1).").onTableText().monospacedDigit()
                        Text(j.name + (j.seat == mySeat ? " (\(FStrings.t("ios.you")))" : ""))
                            .onTableText()
                        Spacer()
                    }
                }
            }
            .padding(.horizontal)

            // Testing-only solo controls REPLACE the normal ones when they are
            // live, rather than sitting alongside them: the shipping lobby can
            // legitimately be offering "waiting" at the same moment solo play
            // wants to offer Start, and two contradictory controls on one
            // screen is worse than either. See `soloControls`.
            if let onAddSoloSeat, soloSeatsEnabled {
                soloControls(onAddSoloSeat)
            } else {
                standardControls
            }

            // THE TABLE'S RULES, chosen here because here is the only place
            // they CAN be chosen: the game is dealt at Start, and after that
            // the rules are a term of a chain everyone is already playing.
            //
            // BELOW the controls (owner, 1.0(17)). It is not a step on the way
            // to starting - it is a standing fact about the table that anyone
            // may change while the lobby is open, so it sits under the buttons
            // rather than between the roster and them.
            //
            // A spectator sees the box but cannot move it - the rules are as
            // much a part of "what game is this" as the player list, and
            // hiding them from the person deciding whether to join would be
            // the wrong half to keep. Moving it takes a seat, because a reseal
            // has to be sent by somebody who is at the table.
            // The tick moves NOW and the bubble is resealed behind it (round
            // 21 - see `passingWish`). Writing the wish here rather than inside
            // `onSetPassing` keeps the staging closure exactly what it was, and
            // puts the whole of the optimism in the one view that draws the box.
            FCheckbox(FStrings.t("ios.lobby.passing"),
                      isOn: passingShown,
                      enabled: mySeat != nil,
                      action: { on in
                          passingWish = on
                          onSetPassing(on)
                      })
                .padding(.horizontal)
        }
        .frame(maxWidth: .infinity)
        .padding()
        // THE WISH IS SPENT when the chain agrees with it, or when somebody
        // ELSE moves the rule out from under it.
        //
        // Not simply "on any change", which is the obvious version and flashes:
        // two taps inside one round trip stage two bubbles, so the box would
        // snap to the first rule for a frame on its way to the second, even
        // though the finger only ever asked for the second. My own intermediate
        // bubble is therefore not an answer to my wish - it is a step on the way
        // to it - and only a bubble that is not mine can overrule it.
        .onChange(of: env.passingAllowed) { now in
            if now == passingWish || env.lastActorSeat != (mySeat ?? -1) {
                passingWish = nil
            }
        }
    }

    /// Testing-only (SOLO_TESTING / DEBUG): "Add player" until the lobby has
    /// enough seats, then Start. Deliberately bypasses `LobbyControls.offered`
    /// — specifically its round-5 M9 authorship gate, which withholds Start
    /// from whoever sent the newest bubble so one human cannot lock the others
    /// out of a lobby that still has room. Seating a puppet from this device
    /// makes me the newest sender every time, so that gate would make solo play
    /// impossible; and there is by definition nobody to lock out.
    @ViewBuilder
    private func soloControls(_ addSeat: @escaping () -> Void) -> some View {
        if env.joins.count < env.nPlayers {
            FButton("Add player (testing)", kind: .wood, action: addSeat)
        }
        if env.joins.count >= 2 {
            // The SAME row the shipping lobby renders, not a lookalike: this is
            // the path a seeded simulator run actually films, and a dev control
            // that drifted from the real one would verify the wrong pixels.
            startExitRow(canExit: LobbyControls.canExit(mySeat: mySeat,
                                                        joined: env.joins.count))
        }
    }

    /// Start, and Exit beside it when both are possible.
    ///
    /// The pair spans exactly the width Start spans on its own (owner: "the
    /// distance between the left edge of the left one and the right edge of the
    /// right one should be the same as the current width"). That falls out
    /// rather than being computed: a non-compact FButton is `maxWidth
    /// .infinity`, so two of them share the padded row and `FSpace.m` of
    /// daylight sits between them - true on every device, in every locale, at
    /// every accessibility size, with no arithmetic to drift.
    @ViewBuilder
    private func startExitRow(canExit: Bool) -> some View {
        if canExit {
            HStack(spacing: FSpace.m) {
                FButton(FStrings.t("ios.msg.startgame"), kind: .wood, action: onStart)
                FButton(FStrings.t("ios.msg.exitgame"), kind: .wood, action: onExit)
            }
        } else {
            FButton(FStrings.t("ios.msg.startgame"), kind: .wood, action: onStart)
        }
    }

    @ViewBuilder
    private var standardControls: some View {
            // note 16: no "Waiting for players — N joined" line here any more —
            // the joined list above already says exactly that, and the owner's
            // read was "the lobby is too tight" for a second line saying the
            // same thing.
            let canExit = LobbyControls.canExit(mySeat: mySeat, joined: env.joins.count)
            switch LobbyControls.offered(mySeat: mySeat, joined: env.joins.count,
                                         capacity: env.nPlayers,
                                         iSentTheInvite: env.lastActorSeat == mySeat,
                                         iChangedTheRules: iChangedTheRules) {
            case .start:
                startExitRow(canExit: canExit)
            case .waiting:
                // Round-4 note 1 / round-5 M9: the newest thing on this chain
                // is mine — either my own invite (nobody else has joined yet)
                // or my own join/re-staged invite in a lobby that still has
                // room (M9) — so there is nothing to send, and no Start,
                // that isn't already mine to wait out. Round-5 M10:
                // full-opacity ink + a light shadow, not 55% black. Round-6
                // #17: `onTableText` (Tokens.swift).
                //
                // Round 16: waiting is no longer a DEAD END. This is exactly
                // the owner's "start game is not possible for the last player
                // that joined, thus they can only exit" - the M9 gate withholds
                // Start from whoever sent the newest bubble, and until now that
                // left them with no action at all. Exit alone, full width:
                // there is no second button to share the row with.
                //
                // ONE line, whichever gate is holding Start back (owner,
                // 1.0(17)): a rules change said so in its own words for a
                // moment, and it read as an error message about something the
                // player had just chosen on purpose.
                Text(FStrings.t("ios.msg.waiting"))
                    .font(.footnote).onTableText()
                    .multilineTextAlignment(.center)
                    .padding(.horizontal)
                if canExit {
                    FButton(FStrings.t("ios.msg.exitgame"), kind: .wood, action: onExit)
                }
            case .invite:
                    // I'm in, nobody else is yet. This branch used to render
                    // NOTHING — no Start (needs 2), no Join (I'm joined), no
                    // invite (notes 14/16 dropped that button as redundant with
                    // the auto-stage). Which is a dead end the moment the
                    // auto-staged invite is gone: sent already, or deleted from
                    // the input field, or the extension reopened later. The
                    // owner hit exactly that — a lobby listing one player and
                    // not a single control on it.
                    //
                    // The invite button is only redundant while the auto-staged
                    // bubble is still sitting in the compose field, so it comes
                    // back HERE and only here: re-stage the same WAITING chain
                    // so there is always a way to ask someone to join.
                    Text(FStrings.t("ios.msg.waiting"))    // round-5 M10 / round-6 #17: see .waiting above
                        .font(.footnote).onTableText()
                    FButton(FStrings.t("ios.msg.invite"), kind: .wood, action: onInvite)
            case .join:
                // Same width as the buttons below (note 29) — both rely on the
                // outer .padding() alone, no extra inset on the field. Round-5
                // B1: same three-state nickname gate as NewGameSetup (see
                // `nameVerdict`) — "Join as {name}" only appears once the
                // field holds a valid, trimmed name.
                TextField(FStrings.t("ios.msg.nickname_ph"), text: $nickname).textFieldStyle(.roundedBorder)
                switch nameVerdict {
                case .ok(let name):
                    // Names are the only identity the payload carries (§6), so
                    // each chain's names must stay distinct — the ghost-seat
                    // guard, the §6.3 picker and the "(you)" tag all key on
                    // them (NicknameGate.isTaken's doc has the full story).
                    if NicknameGate.isTaken(name, in: env.joins) {
                        FButton(FStrings.t("ios.msg.nametaken"), kind: .wood, enabled: false) {}
                    } else {
                        FButton(FStrings.t("ios.msg.joinas", ["name": name]), kind: .wood) { onJoin(name) }
                    }
                case .empty:
                    FButton(FStrings.t("ios.msg.entername"), kind: .wood, enabled: false) {}
                case .tooLong:
                    FButton(FStrings.t("ios.msg.nametoolong"), kind: .wood, enabled: false) {}
                }
            case .full:
                // Round-5 M10: full-opacity ink + a light shadow, not 55%
                // black. Round-6 #17: `onTableText` (Tokens.swift).
                Text(FStrings.t("ios.msg.lobbyfull")).font(.footnote).onTableText()
            }
    }

    /// Is solo seating compiled in AND switched on? False in every shipping
    /// build — the flag type itself does not exist there, so this is the one
    /// place the condition is spelled and the call site stays readable.
    private var soloSeatsEnabled: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDebugFlags.soloSeats
        #else
        return false
        #endif
    }

    /// Round-5 B1: the three-state verdict on the CURRENT field text (see
    /// NicknameGate). Replaces the old `displayName`, which only ever
    /// substituted the "You" placeholder for a blank field — there is no
    /// substitute name any more, a name that fails either cap is rejected
    /// outright, not replaced.
    private var nameVerdict: NicknameGate.Verdict { NicknameGate.check(nickname) }
}

/// §B3 one-time name entry for a player being seated without a stored name.
/// Since lobby v3 every player names themselves on the way in (setup or the
/// lobby's Join field), so the one REACHABLE road here is §6.2 cache-loss
/// recovery — a reinstall or second device resolves the seat from an exact
/// signal while the stored nickname is gone with the cache — at any player
/// count (m8's "not redundant with the other two name screens" survives as
/// exactly this: recovery has no setup or Join field to pass through). Shown
/// once (until a name is stored), prefilled with the current nickname if it
/// is not the neutral default.
///
/// Round-5 B1: Continue is no longer always enabled. It gates on the SAME
/// NicknameGate verdict as NewGameSetup and LobbyView's join — blank or
/// over-cap dims the button and swaps its label for the reason. There is no
/// "call me the default" fallback any more: a name is REQUIRED, never
/// substituted, and `.onSubmit` (the keyboard's own Return key) respects the
/// same gate so it cannot hand a rejected name onward either.
private struct NameGateView: View {
    @State private var name: String
    let onContinue: (String) -> Void

    init(prefill: String, onContinue: @escaping (String) -> Void) {
        _name = State(initialValue: prefill == "Me" ? "" : prefill)
        self.onContinue = onContinue
    }

    private var nameVerdict: NicknameGate.Verdict { NicknameGate.check(name) }

    var body: some View {
        VStack(spacing: 16) {
            // Round-6 #17: `onTableText` (Tokens.swift).
            Text(FStrings.t("ios.msg.nameprompt")).font(.headline)
                .onTableText().multilineTextAlignment(.center)
            // No extra .padding(.horizontal) here — the field and the button below
            // both rely solely on the VStack's outer .padding() so they render the
            // same width (note 29; the field used to be inset twice, making it
            // visibly narrower than the full-width Continue button).
            TextField(FStrings.t("ios.msg.nickname_ph"), text: $name).textFieldStyle(.roundedBorder)
                .submitLabel(.done).onSubmit {
                    if case .ok(let trimmed) = nameVerdict { onContinue(trimmed) }
                }
            switch nameVerdict {
            case .ok(let trimmed):
                FButton(FStrings.t("ios.msg.continue"), kind: .wood) { onContinue(trimmed) }
            case .empty:
                FButton(FStrings.t("ios.msg.entername"), kind: .wood, enabled: false) {}
            case .tooLong:
                FButton(FStrings.t("ios.msg.nametoolong"), kind: .wood, enabled: false) {}
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

/// §6.3 tertiary identity: N≥3, cache lost, not the last actor - ask the human.
/// Offers every seat (named where a join is known, else "Seat N") so it also
/// covers the DEBUG single-sim case, where a 2-player game has only one join.
private struct SeatPicker: View {
    let nPlayers: Int
    let joins: [MessageJoin]
    let onPick: (Int) -> Void

    private func label(_ seat: Int) -> String {
        joins.first { $0.seat == seat }?.name ?? "Seat \(seat + 1)"
    }

    var body: some View {
        VStack(spacing: 12) {
            // Round-6 #17: `onTableText` (Tokens.swift).
            Text(FStrings.t("ios.msg.pickseat")).font(.headline).onTableText()
            ForEach(0..<nPlayers, id: \.self) { seat in
                FButton(label(seat), kind: .secondary) { onPick(seat) }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

#if DEBUG || SOLO_TESTING
/// DEBUG-only knobs for single-device testing (never compiled into Release —
/// `#if DEBUG || SOLO_TESTING` means a TestFlight/App Store build cannot contain
/// any of this — the shipping Release build defines neither condition; the
/// on-device testing build opts in with SWIFT_ACTIVE_COMPILATION_CONDITIONS,
/// and the CI Release build is what proves it).
public enum MessageDebugFlags {
    /// Force the seat picker on every adopted bubble so both seats are playable
    /// on ONE simulator (which cannot otherwise distinguish sender from receiver).
    /// The FoolishHarness turns this OFF: it gives each fake participant a
    /// distinct identity + its own seat cache, so seat inference resolves
    /// automatically and the picker would be wrong to show.
    public static var pickSeatOnAdopt = true

    /// SOLO PLAY (owner ask, device testing): seat extra players from this one
    /// device so a real chat can reach a startable game with nobody else in it.
    ///
    /// The shipping lobby is deliberately un-startable alone — Start needs 2+
    /// joined, and the only way to a second join is another human on another
    /// device. That is correct for the product and useless for testing the
    /// extension on a phone: you cannot reach a board at all, so none of the
    /// board work can be checked on device. With this on, the lobby offers
    /// "Add player" (claims the next free seat with a puppet name) and offers
    /// Start as soon as two seats are filled, bypassing the round-5 M9
    /// authorship gate — that gate exists to stop one human locking others
    /// out, and in solo play there is nobody to lock out.
    ///
    /// Pairs with `pickSeatOnAdopt`: add a puppet, start, then every time you
    /// open a bubble the picker asks which seat you are, so one person plays
    /// every hand in one chat.
    public static var soloSeats = true

    /// ROUND 20: take the RELEASE route when seat identity is ambiguous - the
    /// public spectator board - instead of DEBUG's seat picker.
    ///
    /// That screen is `#if DEBUG`/`#else`, so until now it was the one surface
    /// in the app no rig could reach and no screenshot could be taken of: every
    /// harness build is a debug build. It had therefore never been looked at
    /// with a FINISHED game on it, which is exactly how it came to draw a swept
    /// empty table and the line "spectating - open the game from your own bubble
    /// to play" over a game that was over (owner: "spectators should still be
    /// able to see win screen").
    ///
    /// Off by default, so nothing about the normal harness changes; the
    /// `spectator-over` scenario turns it on.
    public static var spectateWhenAmbiguous = false
}
#endif


/// Round-5 M1: "This game link is damaged" used to be a dead end with no
/// action on it at all (docs/APP_REVIEW_NOTES.md M1). Owner's fix — "just
/// throw in the 'create a new game' button back, which when pressed will
/// initialize a new lobby" — is the SAME New-game affordance every other
/// dead end in this file already offers, not a bespoke retry/dismiss flow.
///
/// The owner also asked to exclude whoever sent the damaged link from the
/// fresh lobby. Not implementable as asked: participant identities are
/// device-scoped and never travel in the payload (see SeatIdentity's header —
/// there is no "sender" field to read here, let alone exclude by). A fresh
/// lobby that everyone, including whoever sent the bad link, re-joins by
/// choice is the version of this fix that can actually be built.
private struct DamagedView: View {
    let onNewGame: () -> Void

    var body: some View {
        VStack(spacing: 8) {
            // Round-6 #17: `onTableText` (Tokens.swift).
            Text("Foolish").font(.headline).onTableText()
            // Round-5 M10: full-opacity ink + a light shadow, not 55% black —
            // the busy wool weave has no fixed-opacity foreground that
            // survives it (M10's fix, applied throughout this file, mirrors
            // the plank rank column's BONE text on WOOD, which uses a DARK
            // shadow; ink text on the lighter wool needs the inverse, a LIGHT
            // one). Round-6 #17 added the weight both treatments share.
            Text(FStrings.t("ios.msg.damaged")).font(.footnote).onTableText()
                .multilineTextAlignment(.center).padding(.horizontal)
            FButton(FStrings.t("ios.msg.newgame"), kind: .wood, action: onNewGame)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}
