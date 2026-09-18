// MessagesRootView - WHERE the extension's surface is, per presentation style
// (§10), and the tween that carries it between the two.
//
// Compact is the KEYBOARD AREA (§3.5): no text field, no horizontal scrollers —
// so it is a label and buttons. Expanded is the table. What this view owns is
// the BOX: it measures the drawer, arms and drives the collapse
// (CollapseDriver/CollapseTween/CollapseLayers, round 10d), publishes the
// drawer's live height and the host's own expanded answer into the
// environment, and hands the result to one child.
//
// WHAT IT SHOWS inside that box is GameSurface.swift — the §5/§6/§7 machine
// wearing a UI: a selected bubble is decoded + adopted, my seat is resolved
// (§6), and I either play (MessageTableView, staging a reply) or, when
// three-plus players leave my seat ambiguous, pick who I am (§6.3). The
// pre-game screens it routes to are LobbyScreens.swift and their decision
// layer is LobbyControls.swift. The two used to share this file: ~180 lines of
// drawer geometry above ~1000 lines of surface routing, with no reason beyond
// history for either half to be able to reach into the other's state.
//
// No Durak rule is answered in any of them — MessageTurnController relays the
// kernel and MessageComposer only stages. Seat identity is the one non-kernel
// decision, and it is SeatIdentity's pure §6 logic, fed the conversation's
// `senderIsLocal`.
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
    /// THE `style` PROP IS GONE (1.1(68)) and nothing should bring it back.
    /// present() only runs on discrete host events (activate, receive, send,
    /// New game), so across a grabber drag or an auto-collapse it went STALE -
    /// which is how the send reminder leaked into the expanded lobby. Anything
    /// that must know how tall the drawer is reads its own live geometry
    /// instead (MessageTableView's `collapseFraction`), and this had been
    /// written and never read ever since.
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
    /// THE SLIDE (see `CollapseTween.slideDuration`): hand the collapse to the
    /// layer. Called with the box's travel and how long it takes; the second
    /// closure takes it back off again. Closures rather than a plumbed layer
    /// because the layer belongs to the hosting controller and this is the view
    /// inside it - the same shape, and for the same reason, as `requestExpand`.
    let slideCollapse: (CGFloat, Double, Double) -> Void
    let endSlide: () -> Void
    /// Is the HOST's sheet expanded, right now? A live read of
    /// `MSMessagesAppViewController.presentationStyle`, not the `style` prop -
    /// see `NameFieldAutofocus`, the only thing that asks.
    let hostIsExpanded: () -> Bool
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

    public init(payloadURL: URL?, senderIsLocal: Bool,
                startNewGame: Bool, newGameToken: Int = 0, sentToken: Int = 0,
                sentPayload: Data? = nil, chatKey: String,
                chatIsDM: Bool, chatPlayers: Int,
                incomingURL: URL? = nil, incomingToken: Int = 0, cancelToken: Int = 0,
                collapseSignal: CollapseSignal = CollapseSignal(),
                requestExpand: @escaping () -> Void,
                slideCollapse: @escaping (CGFloat, Double, Double) -> Void = { _, _, _ in },
                endSlide: @escaping () -> Void = {},
                hostIsExpanded: @escaping () -> Bool = { false },
                onNewGame: @escaping () -> Void,
                onFreshChain: @escaping () -> Void = {},
                onAnnounceLeave: @escaping (String) -> Void = { _ in },
                onSend: @escaping (Data, Int, Bool) async -> Void,
                onUnstage: @escaping () -> Void = {},
                onOpenURL: @escaping (URL) async -> Bool = { _ in false }) {
        self.payloadURL = payloadURL; self.senderIsLocal = senderIsLocal
        self.startNewGame = startNewGame; self.newGameToken = newGameToken; self.sentToken = sentToken
        self.sentPayload = sentPayload
        self.chatKey = chatKey; self.chatIsDM = chatIsDM; self.chatPlayers = chatPlayers
        self.incomingURL = incomingURL; self.incomingToken = incomingToken
        self.cancelToken = cancelToken; self.collapseSignal = collapseSignal
        self.requestExpand = requestExpand; self.hostIsExpanded = hostIsExpanded
        self.slideCollapse = slideCollapse; self.endSlide = endSlide
        self.onNewGame = onNewGame
        self.onFreshChain = onFreshChain; self.onAnnounceLeave = onAnnounceLeave
        self.onSend = onSend
        self.onUnstage = onUnstage
        self.onOpenURL = onOpenURL
    }

    // ROUND-10 #1's `stageHeight` IS GONE (1.1(68)), and the finding it carried
    // is worth more than the property was: the "self cards dip under the
    // screen" during an auto-collapse was never this view's geometry. A debug
    // ruler drawn on the surface proved the flying rect in the films was the
    // just-inserted STAGED BUBBLE's snapshot - the public table, no hand - that
    // Messages animates into the compose slot OVER the drawer. It was fixed at
    // the source instead: `MessagesViewController.stage()` collapses FIRST and
    // inserts the bubble after the transition settles. The height this held was
    // never read again after that, and `CollapseTween` owns the tweening it was
    // written for.


    /// Round-10c's `lastCompactHeight` and `extentHold` are GONE (round 22):
    /// both belonged to the pre-collapse pack that round 10d removed, and both
    /// had been write-only ever since - state that is set on every layout pass
    /// and read by nothing reads as a live input when the next person changes
    /// this. The compact threshold moved to `CollapseTween`, which is now where
    /// the whole rule lives.

    /// Round-10d: the box's height while the collapse tween runs; 0 = follow
    /// the model box exactly (every other moment, including manual drags).
    ///
    /// AND THE MODEL BOX, COLLAPSED, IS TWO HEIGHTS - BOTH OF THEM THE HOST'S.
    /// 1.1(69), owner: "Collapsed should be the same height as the keyboard
    /// height in iMessages, always", against a lobby that settled at a drawer
    /// top of 584 in one film and 568 in another on the same binary.
    ///
    /// Measured on the rig (6.9", 440x956pt, real Messages): Messages gives the
    /// compact drawer 388.7pt while its OWN compose field holds no first
    /// responder and 372pt while it does - the whole input stack drops 17pt with
    /// it, and `geo.size.height` here reads 340 or 323 to match. Create, leave,
    /// toggle and start all land on 340 from a base that left the thread and
    /// came back and all four land on 323 from a base that tapped the compose
    /// field first, so the action is irrelevant and the host's first-responder
    /// state decides it. Nothing in this target can ask for either: the drawer
    /// is Messages', the hosting controller is pinned to its edges, no
    /// `preferredContentSize` is set and no plist key touches the size.
    ///
    /// So there is no compact height for this file to get right - only the
    /// host's, rendered verbatim, which is what `CompactRestHeightTests` pins
    /// and `ios/Tools/rig/README.md` trap 10 records in full.
    @State private var boxHeight: CGFloat = 0
    /// The clock that moves `boxHeight` through an auto-collapse - a timer
    /// evaluating the host's own curve, not a SwiftUI animation. See
    /// CollapseTween's file note for the three filmed reasons. `@State` so it
    /// survives the body re-evaluations its own ticks cause.
    @State private var driver = CollapseDriver()
    /// The slide's own release, so a second collapse cannot be released by the
    /// first one's timer - the driver has `stop()` for the same job.
    @State private var slideRelease: Task<Void, Never>?
    /// The whole of the slide's travel while one runs, zero otherwise. Only the
    /// wool reads it, and a CONSTANT is all the wool needs: bottom-anchored to
    /// the box's fixed bottom edge and this much taller, its top sits at the
    /// drawer's top edge on the flip and above it for the rest of the run,
    /// where the drawer clips it. The per-frame value this used to be was a
    /// timer writing state at 60Hz for the table group to cancel the slide
    /// with, a frame late; the table group rides `CollapseLayers` now.
    @State private var slideTravel: CGFloat = 0
    /// The slide as the BACKGROUND sees it - the same number, named apart so the
    /// wool's dependency on it is legible where it is used.
    private var collapseSlideNow: CGFloat { slideTravel }
    /// Every view riding the collapse on a layer of its own - the table cards,
    /// deck, discard, opponent ring - and the run they share. Started in the
    /// same runloop turn as the hosting layer's keyframes, so both land in one
    /// transaction. See CollapseLayer.
    @State private var layers = CollapseLayers()
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
    /// ROUND 31 - THE SAME COLLAPSE, RE-MEASURED AT 60fps, and what survived.
    /// The ten numbers above came off a take resampled to 30fps, which is half
    /// the frames the device composited; `msgrig.sh film` now keeps all of them
    /// (`-fps_mode passthrough` plus per-frame timestamps), and `msgrig.sh
    /// ruler` draws the ruler again, so this is repeatable rather than a
    /// remembered afternoon. Three takes, iPhone 14 Plus, real Messages:
    ///
    ///   rest expanded   box top  77   box bottom 892   (drawer top 57)
    ///   flip frame      box top  99   box bottom 912   <- no teleport
    ///   +1..+18 frames  top 141, 193, 249, 289, 317, 357, 377, 403, 421, 441,
    ///                       455, 471, 493, 493, 509, 509, 516, 523
    ///                   settling on 558 by +440ms
    ///
    /// WHAT STANDS, and is now measured rather than argued: the box's top rides
    /// the drawer's descending top edge to within 1.4pt in EVERY frame of the
    /// collapse. That is the premise this whole scheme rests on.
    ///
    /// WHAT DOES NOT: "tracks those ten sampled points to within a couple of
    /// points" is a fit to the TAIL. Over the first 70ms - the part 30fps could
    /// not resolve - the host's curve runs up to 21pt away from a quartic-out
    /// over 0.45s, and up to 68pt away from the bezier this file actually runs
    /// (which is 0.38s, not the 0.45s the paragraph above says). What that
    /// costs is visible in the box's BOTTOM: through the collapse's first
    /// ~130ms it hangs as much as 23pt below the drawer's bottom edge and
    /// jitters ~10pt frame to frame, where a manual grabber drag - the look
    /// this is copying - holds the same gap to 1.4pt. The excess is clipped, so
    /// nothing is exposed and no wool shows; the hand is simply that far under
    /// the drawer's edge for four or five frames. Left alone at the time: this
    /// animation was tuned against the owner's explicit spec, and three earlier
    /// approaches were filmed failing before it.
    ///
    /// ROUND 32 - THE BEZIER IS GONE. Seventeen more variants were filmed
    /// against the bezier and none beat it, because every one of them shared
    /// three things with it that nobody had spotted: `withAnimation` paints
    /// its START value on its first frame (the expanded box under a drawer
    /// that had already moved - the one off-screen frame in the shipped
    /// take); no `.spring(response:)` can say how far into ITS spring the host
    /// already is when our report arrives; and the host's transaction is
    /// re-composited by the render server between our frames, so a stale
    /// height shows up as a sawtooth on the hand whatever the curve. The box
    /// is now driven by `CollapseDriver` evaluating the fitted host spring
    /// (`CollapseTween.height`, response 0.338s, lead 10ms) - see the note at
    /// the top of CollapseTween for all three, measured. On the rig, against
    /// the shipped bezier's 1 off-screen frame / 39.4pt excursion / 47.4pt
    /// max step: 0 / 20pt / 30pt at lead 5ms, the 30pt being the drawer's own
    /// 8ms travel at peak, which is the floor.
    ///
    /// The EXPAND direction is composited bottom-referenced by the host (the
    /// owner: it "works much better... cards stay at the bottom"), so up-snaps
    /// are followed instantly, exactly as before.
    /// The DECISION is `CollapseTween.step` - a pure function, so the host's
    /// noisy transition reports can be replayed as a test rather than re-filmed
    /// (CollapseTweenTests). This is the part that cannot be pure: the
    /// driver, and the release that hands the box back to the model.
    /// The collapse driver's three knobs.
    ///
    /// The SHIPPED values are `CollapseTween`'s own constants. `MessageDevBoard`
    /// exists only so a filmed sweep can override them from a file, and that
    /// whole file is `#if DEBUG || SOLO_TESTING` - so this reads it behind the
    /// same gate. Read unconditionally, as it was, it compiled in every Debug
    /// build anyone runs and failed ONLY the Release archive: the one build
    /// nobody makes by hand, and the only one that ships.
    ///
    /// A property and not three lines inside `follow`'s `.start` case on
    /// purpose - CollapseTweenTests reads the first 1200 characters of that case
    /// looking for the release, so anything added inside it can push the release
    /// out of the window and fail a test that is about something else entirely.
    private static var collapseKnobs: (lead: Double, hz: Double, response: Double, slide: Bool) {
        #if DEBUG || SOLO_TESTING
        let k = MessageDevBoard.collapseKnobs
        return (k.lead, k.hz, k.response, k.slide)
        #else
        return (CollapseTween.hostLead, CollapseTween.driveHz, CollapseTween.hostResponse,
                CollapseTween.slideByDefault)
        #endif
    }

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
            // The host's own curve on a clock, not a SwiftUI animation (why:
            // CollapseTween's note). Animations off: the tick IS the animation.
            let k = Self.collapseKnobs
            // THE SLIDE: pin the box and let the layers carry the motion, so
            // the frames we never render are still in the right place. It
            // releases through the same re-measure nudge as the driver does.
            if k.slide { startSlide(from: from, to: to); return }
            driver.start(from: from, to: to, lead: k.lead, hz: k.hz, response: k.response,
                         tick: { h in
                             var tx = Transaction()
                             tx.disablesAnimations = true
                             withTransaction(tx) { boxHeight = h }
                         },
                         onDone: {
                             collapsing = false
                             CollapseTween.isTweening = false
                             Task { await handBackToModel() }
                         })
        // The host settled TALLER than the snap this tween started on. Ease up
        // rather than rest short of the drawer and expose the wool under it -
        // see CollapseTween for why this correction is upward only.
        case .retarget(let to):
            collapseTarget = to
            driver.retarget(to: to)
        case .hold:
            break
        case .follow:
            // The release first (CompactRestHeightTests reads it here): the
            // box goes back to the host's own height...
            boxHeight = 0
            driver.stop()
            // ...and a manual drag mid-collapse has to take the layers back
            // with it - a translation left running would slide a box the
            // grabber is now placing by hand.
            slideRelease?.cancel(); slideRelease = nil
            endSlide()
            layers.end()
            slideTravel = 0
        }
    }

    /// The slide: the box laid out at its DESTINATION and pushed down by the
    /// drawer's remaining travel on the hosting layer, with the table group's
    /// own layers taking their share back - both evaluated by the render
    /// server on every frame it composites. See `CollapseTween.slideOffsets`
    /// for the hosting layer's half and `CollapseLayer` for the table's.
    private func startSlide(from: CGFloat, to: CGFloat) {
        // THE DESTINATION, laid out once and moved - not the origin, held and
        // slid away.
        boxHeight = to
        slideTravel = from - to
        // THE KNOB REACHES THE CURVE, which it did not. Both halves of the
        // slide called `slideOffsets` without a response, so both always ran on
        // the 0.338 constant while `dev.collapse`'s `resp=` moved only the old
        // driver - i.e. the one path still in use could not be swept at all,
        // and the figure it runs on was fitted against a different mechanism.
        let response = Self.collapseKnobs.response
        slideCollapse(from - to, CollapseTween.slideDuration, response)
        // And the table group's layers, in the SAME turn, so every keyframe set
        // is committed in one transaction and the first composited frame
        // already has both motions in it.
        layers.begin(travel: from - to, duration: CollapseTween.slideDuration,
                     response: response)
        slideRelease?.cancel()
        slideRelease = Task { @MainActor in
            try? await Task.sleep(nanoseconds: UInt64(CollapseTween.slideDuration * 1_000_000_000))
            guard !Task.isCancelled else { return }
            collapsing = false
            CollapseTween.isTweening = false
            // ONE TURN, all of them. The box rectangle is identical either way
            // round - expanded height translated up by its own travel is the
            // compact height in place - so what changes here is the content
            // inside it, not where it is. Split across two turns it is a 535pt
            // jump for one frame.
            slideTravel = 0
            endSlide()
            layers.end()
            await handBackToModel()
        }
    }

    /// End the tween and hand the box back to the model - THROUGH a height
    /// that is not the one it rests at, or the release publishes nothing and
    /// every landmark stays measured on the expanded board. The rule and the
    /// owner's two reports it answers are `CollapseTween.remeasureNudge`.
    @MainActor
    private func handBackToModel() async {
        // Nothing overriding the model box: `follow` already released it (a
        // manual drag mid-tween), and a nudge here would put a compact height
        // back onto a drawer that has moved on.
        guard boxHeight > 0 else { return }
        boxHeight = CollapseTween.handBack(target: boxHeight)
        try? await Task.sleep(nanoseconds: 32_000_000)   // a frame, for that layout to land
        boxHeight = 0
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
                // ROUND 32: while the collapse runs, the wool HANGS BELOW the
                // box by `CollapseTween.woolOverhang`. The driver deliberately
                // keeps the box a little SHORT of the drawer (the lead's
                // margin against a dropped frame - see `hostLead`), and the
                // host re-composites each of our pictures once more before
                // the next, a further ~26pt short at peak. Both used to show
                // as a strip of the host's flat fallback colour under the
                // hand; now that strip is wool, which is the one thing on
                // this surface nobody can see move. Top-aligned so the
                // overhang is at the bottom, where the drawer clips it. At
                // rest the box is the model and this is a no-op.
                // THE TEXTURE IS PINNED TO THE BOTTOM AND UNCOVERED FROM THE TOP.
                //
                // Owner, round 47: "think of it that the ENTIRE texture is
                // visible in the expanded view, and only the bottom half is
                // visible in the collapsed view. But it never like moves like
                // that." It did not: top-anchored, the wool's own origin rode
                // whatever the box was doing, so during a slide it dropped with
                // the box, got cropped when the drawer arrived, and then shifted
                // again when the box was handed back - three movements of a
                // surface that should never move at all.
                //
                // Bottom-anchored it cannot. Its bottom edge is the box's bottom
                // edge, which under the slide is the one line on screen that is
                // fixed, and its height is the box the TABLE thinks it is in - so
                // the texture stays put and the drawer simply stops covering more
                // of it. The overhang stays for the reason it was added: a box
                // kept deliberately short would otherwise show the host's flat
                // fallback colour under the hand rather than wool.
                .background(alignment: .bottom) {
                    TableBackground()
                        .frame(height: (boxHeight > 0 ? boxHeight : geo.size.height)
                                       + collapseSlideNow
                                       + (collapsing ? CollapseTween.woolOverhang : 0))
                }
                // The debug ruler (`dev.ruler`, DEBUG only, otherwise an
                // EmptyView) - on the SIZED BOX, so a filmed frame reports
                // where `boxHeight` actually put our two edges. See
                // CollapseRuler.
                .overlay(CollapseRuler())
                // TOP-anchored through the collapse: the host glues our content
                // to the drawer's descending top edge, so a box of the drawer's
                // visible height starting there fills it exactly. Bottom
                // otherwise (the expand is composited bottom-referenced). At
                // rest the box fills the frame and the two agree.
                .frame(maxWidth: .infinity, maxHeight: .infinity,
                       alignment: collapsing ? .top : .bottom)
                .onAppear { lastGeoHeight = geo.size.height }
                .onChange(of: geo.size.height) { follow(height: $0) }
                // The drawer's LIVE height, published for the one descendant
                // that cannot measure it itself - see NameFieldAutofocus. A
                // view's own GeometryReader reports its own box, and a name
                // field's box is 34pt tall whatever the drawer is doing.
                .environment(\.surfaceHeight, geo.size.height)
                // The bus the table group's layers ride, and only when the
                // slide is on: with nothing here `collapseLayer` renders its
                // view in place and a shipping board has no nested hosts.
                .environment(\.collapseLayers, Self.collapseKnobs.slide ? layers : nil)
                .environment(\.hostIsExpanded, hostIsExpanded)
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
