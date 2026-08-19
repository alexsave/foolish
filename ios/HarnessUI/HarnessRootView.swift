// HarnessRootView — the FoolishHarness chrome around the real MessagesRootView.
// NOT SHIPPING CODE (see FoolishHarnessApp.swift).
//
// This is a from-scratch reskin (the previous attempt on this branch stalled
// mid-way and was deliberately NOT resumed from its stash — starting clean
// from the committed files instead) to make the harness look and BEHAVE like
// the real Messages app hosting an iMessage extension, per the owner's
// reference screenshot. Four explicit requirements drove the shape:
//
//   1. Only the transcript scrolls. Real Messages has a fixed top nav bar, a
//      scrolling message list, and a fixed bottom area (compose bar + the
//      extension drawer). The old harness put EVERYTHING — dev controls,
//      transcript pills, the board — inside one `.safeAreaInset`-wrapped
//      column, which read as "scroll everything" (the owner's own complaint).
//      Now the chat screen is a strict VStack: fixed top, `ScrollView`
//      middle, fixed bottom. Nothing in the fixed sections scrolls.
//   2. The old "Chat A"/"Chat B" button pair is GONE. Reaching the other
//      simulated conversation is now via the nav bar's back chevron -> a chat
//      LIST screen (ChatListView.swift), exactly like real Messages routes
//      "back" from an open thread to the conversation list.
//   3. The participant ("who am I") swap — real Messages has no equivalent of
//      this at all — stays pinned in the TOP BAR, visually distinct (orange
//      dev chrome) from the simulated phone below it.
//   4. EVERYTHING that reads as "the simulated Messages app" — the contact nav
//      bar, the chevron, the transcript, the compose bar, AND the extension —
//      lives inside ONE fixed iPhone-SE-sized frame (`SEScreen`, 375x667pt).
//      An earlier pass on this redesign only constrained the extension to SE
//      size while the Messages chrome around it freely filled the host
//      simulator's own (larger) screen — the owner caught that split and
//      asked for the whole simulated phone, not just the board, to be one
//      consistently-sized unit. Only `DevBar` (harness-only chrome with no
//      real-phone equivalent at all) stays outside that frame.
//
// The extension itself (MessagesRootView, hosted via its own
// UIHostingController — see `HostedStage`'s doc for why that indirection
// exists) is untouched: same interface, same compact/expanded transition,
// same wool board. What's new is host-driven drag gestures on the grabber —
// swipe up to expand, swipe down to collapse, swipe down again to dismiss
// entirely — mirroring real Messages, where that gesture belongs to the
// SYSTEM chrome around the extension, not the extension's own view
// (confirmed: FoolishKit threads `requestExpand` through but never calls it
// anywhere itself).

import SwiftUI
import UIKit
import FoolishKit

/// The one fixed size everything "inside the phone" is built to — iPhone SE
/// (3rd gen)'s logical screen, 375x667pt. Requirement 4: this now bounds the
/// WHOLE simulated Messages app (nav bar through extension), not just the
/// extension's own viewport.
private enum SEScreen {
    static let width: CGFloat = 375
    static let height: CGFloat = 667
}

/// Named, EXPLICIT heights for the two fixed chrome rows that sandwich the
/// scrolling transcript — `ContactNavBar` and `ComposeBar` are each given
/// `.frame(height:)` pinned to these constants (not left to intrinsic
/// font/padding sizing), specifically so `StageHeight.expanded` below can be
/// a real subtraction from `SEScreen.height` instead of another guessed
/// constant. Web research (2026-07-22 session — developer.apple.com/forums/
/// thread/52049: "the size of the space between the header and the bottom
/// bar" is `view.bounds` after `topLayoutGuide`/`bottomLayoutGuide` are
/// applied; ustwo's iMessage design writeup: expanded is "almost a full
/// screen viewport, except header and footer chrome persist") confirms the
/// real MSMessagesAppViewController is handed EXACTLY the leftover space
/// between Messages' nav bar and its compose bar, i.e. a SUBTRACTIVE model —
/// not the "whole screen, chrome overlaid via insets" reading an earlier pass
/// took from one bare `view.bounds.height` forum data point (that number was
/// almost certainly read before layout settled; a sibling thread on the same
/// forums is literally titled "iMessage Navigation bar covering top of view
/// bug" over exactly this kind of premature-read gotcha).
private enum ChatChrome {
    static let navBarHeight: CGFloat = 64
    static let composeBarHeight: CGFloat = 40
    static let dividerHeight: CGFloat = 1
}

struct HarnessRootView: View {
    @StateObject private var model = HarnessModel()
    /// The harness's whole "navigation stack" — one level deep, because that's
    /// all real Messages' thread <-> conversation-list toggle is. Nothing but
    /// the back chevron (`ContactNavBar`) and a chat-list row (`ChatListScreen`)
    /// ever changes this.
    @State private var screen: Screen = .chat
    private enum Screen { case chat, chatList }
    /// The animation trace panel (AnimLog). Only reachable when the trace is
    /// switched on (HARNESS_ANIMLOG), because it is empty otherwise.
    @State private var showAnimLog = false

    var body: some View {
        VStack(spacing: 0) {
            DevBar(model: model, showAnimLog: $showAnimLog)   // harness-only chrome — the ONE thing outside the SE frame
            Divider().overlay(Color.orange.opacity(0.3))
            ZStack {
                simulatedPhone
                if showAnimLog { AnimLogPanel() }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)   // center in whatever room is left
        }
        .background(Color.black.ignoresSafeArea())
        .task {
            // DEV: `HARNESS_SEED=1` deals a board immediately for screenshotting
            // (interactive tapping needs Accessibility perms the CI shell lacks).
            if ProcessInfo.processInfo.environment["HARNESS_SEED"] != nil {
                await model.seedDemoGame()
            }
            // Blink repro (HARNESS_RECEIVE_LIVE): after a seeded board, simulate a
            // bubble arriving from ANOTHER seat while THIS viewer stays put and
            // expanded - the real "live receive" (a loadKey reload of the mounted
            // board), the case `become` cannot model. Watch the AnimLog for a
            // "surface reload" NOT followed by "surface BLANK render".
            if ProcessInfo.processInfo.environment["HARNESS_RECEIVE_LIVE"] != nil {
                try? await Task.sleep(nanoseconds: 2_500_000_000)
                await model.simulateLiveReceive()
            }
            // REVIEW RIG: land on one named state and stop (HarnessScenario.swift).
            if let s = HarnessModel.scenarioName {
                if s == "chatlist" { screen = .chatList } else { await model.runScenario(s) }
            }
        }
    }

    /// The whole simulated Messages app, clipped to one SE-sized, rounded,
    /// bordered "device" — requirement 4. `screen` picks which Messages
    /// SCREEN is showing inside that fixed frame; the frame itself never
    /// changes size when it does (a real phone's screen doesn't resize when
    /// you navigate).
    private var simulatedPhone: some View {
        Group {
            switch screen {
            case .chat:
                ChatScreen(model: model, onBack: { screen = .chatList })
            case .chatList:
                // Selecting a row switches the simulated conversation (still
                // `HarnessModel.switchChat`, unchanged — see its doc for why it
                // deliberately does NOT rebind the store) and returns to it,
                // exactly like tapping a row in the real Messages list.
                ChatListScreen(model: model, onSelect: { idx in
                    model.switchChat(idx)
                    screen = .chat
                })
            }
        }
        .frame(width: SEScreen.width, height: SEScreen.height)
        .clipShape(RoundedRectangle(cornerRadius: 30, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 30, style: .continuous)
                .strokeBorder(Color.white.opacity(0.2), lineWidth: 1)
        )
    }
}

/// The open-conversation screen, sized to exactly fill `SEScreen`.
///
/// The extension drawer is NOT a boxed-in VStack row that just resizes under
/// a fixed nav bar — the owner caught that mismatch against the real app:
/// "when I expand it in a real iMessage app, it expands OVER the rest of the
/// iMessage stuff. It doesn't remain boxed in under the chat textarea...
/// position should be like absolute". So the nav bar + transcript are the
/// BASE layer of a `ZStack`, and the extension is a bottom-anchored sibling
/// that overlays them — growing tall enough while expanded to cover both
/// entirely (see `StageHeight`'s doc), animating smoothly between that and a
/// small bottom strip while compact (the "graceful transition" ask).
///
/// The compose bar is not part of that overlay, and it does not sit UNDER the
/// drawer either — the owner's correction, twice: "the compose bar, with the +
/// and the send and the text area, should be ON TOP, above, lower y value
/// whatever you want to call it, over the compressed view. NOT overlapping it
/// with a higher z index. And when we're in expanded view, it shouldn't be
/// there at all."
///
/// So it is a row at the BOTTOM of the base layer, and the base layer reserves
/// exactly the compact drawer's height beneath itself (`reservedForDrawer`).
/// The compose bar's bottom edge then lands on the drawer's top edge: adjacent,
/// never overlapping, and its Y is genuinely smaller rather than merely being
/// drawn later. Expanded reserves nothing and drops the compose bar entirely,
/// so the drawer covers the whole screen — which is what the real app does.
/// A dismissed drawer reserves nothing but KEEPS the compose bar, since its "+"
/// is the only way back in.
private struct ChatScreen: View {
    @ObservedObject var model: HarnessModel
    let onBack: () -> Void

    /// `model.stageIsExpanded`, NOT `model.presentation == .expanded`: a
    /// dismissed drawer is not expanded no matter what the style says, and
    /// reading the style alone here is exactly what stranded the screen with
    /// neither drawer nor compose bar. See HarnessModel.stageIsExpanded.
    private var isExpanded: Bool { model.stageIsExpanded }
    /// The space the base layer leaves free at its bottom for the drawer. Zero
    /// when the drawer is expanded (it covers everything) or dismissed (there
    /// is no drawer).
    private var reservedForDrawer: CGFloat {
        if model.drawerDismissed || isExpanded { return 0 }
        return ExtensionStage.compactHeight
    }

    var body: some View {
        ZStack(alignment: .bottom) {
            VStack(spacing: 0) {
                ContactNavBar(model: model, onBack: onBack)
                Divider().overlay(Color.white.opacity(0.12))
                TranscriptScroll(model: model)     // the ONLY scrolling region
                if !isExpanded {
                    Divider().overlay(Color.white.opacity(0.12))
                    ComposeBar(model: model)
                }
            }
            .padding(.bottom, reservedForDrawer)

            ExtensionStage(model: model)
        }
        .clipped()   // the sliding drawer must never spill past the phone frame
        .background(Color(white: 0.05))
    }
}

/// Dev-only chrome, always visible, always orange-tinted so it never reads as
/// part of the simulated Messages UI beneath it. Houses the player-count
/// picker, the New-game reset, and — per requirement 3 — the "who am I" swap.
/// That swap is genuinely not something a real phone can do (a device IS a
/// participant); keeping it here, separate from the chat list, is what makes
/// it read as harness tooling rather than a feature of the fake Messages host.
private struct DevBar: View {
    @ObservedObject var model: HarnessModel
    @Binding var showAnimLog: Bool

    var body: some View {
        // Lives OUTSIDE the simulated SE screen (see HarnessRootView's
        // `simulatedPhone`, requirement 4), so — unlike everything below —
        // this bar's own height doesn't compete with the phone frame's fixed
        // 667pt budget. Kept reasonably tight anyway just so it doesn't dwarf
        // the actual phone simulation underneath it.
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("FoolishHarness — NOT the shipping extension")
                    .font(.system(size: 10, weight: .bold)).foregroundStyle(.orange)
                Spacer()
                if AnimLog.on {
                    Button { showAnimLog.toggle() } label: {
                        Text(showAnimLog ? "hide log" : "log").font(.system(size: 11, weight: .semibold))
                            .padding(.horizontal, 8).padding(.vertical, 4)
                            .background(Color.orange.opacity(showAnimLog ? 1 : 0.35))
                            .foregroundStyle(.black).clipShape(Capsule())
                    }
                }
                Button { model.newGame() } label: {
                    Text("New").font(.system(size: 11, weight: .semibold))
                        .padding(.horizontal, 8).padding(.vertical, 4)
                        .background(Color.white.opacity(0.14)).foregroundStyle(.white).clipShape(Capsule())
                }
                Picker("", selection: Binding(get: { model.playerCount },
                                              set: { model.setCount($0) })) {
                    ForEach(2...8, id: \.self) { Text("\($0)p").tag($0) }
                }.pickerStyle(.menu).tint(.orange)
            }

            // Requirement 3: the participant swap lives in the top bar, styled
            // as obviously-dev chrome (orange label), NOT on the chat list —
            // "who am I" and "which conversation am I in" are different axes
            // (see HarnessModel's type doc), and only the latter moved to the
            // chevron/list flow.
            HStack(spacing: 6) {
                Text("you are:")
                    .font(.system(size: 10, weight: .semibold)).foregroundStyle(.orange.opacity(0.85))
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        ForEach(Array(model.participants.enumerated()), id: \.element.id) { idx, p in
                            Button { model.become(idx) } label: {
                                Text(p.name)
                                    .font(.system(size: 11, weight: idx == model.localIndex ? .bold : .regular))
                                    .padding(.horizontal, 10).padding(.vertical, 5)
                                    .background(idx == model.localIndex ? Color.orange : Color.white.opacity(0.14))
                                    .foregroundStyle(idx == model.localIndex ? .black : .white)
                                    .clipShape(Capsule())
                            }
                        }
                    }
                }
            }
            // model.debugInfo (seat/turn/legal-moves after a HARNESS_SEED)
            // deliberately isn't rendered here anymore — the owner asked for
            // the room back now that the whole simulated phone has to fit a
            // fixed 375x667 SE frame (requirement 4); still readable via the
            // debugger/model if actually needed.
        }
        .padding(.horizontal, 8).padding(.vertical, 3)
        .background(Color(white: 0.09))
    }
}

/// The Messages-style nav bar: back chevron (-> the chat list, requirement 2),
/// contact avatar, and contact name. This is the piece that makes the harness
/// actually resemble the reference screenshot instead of a bare control strip
/// — and the chevron is the ONE control that replaces the old Chat A/B buttons.
private struct ContactNavBar: View {
    @ObservedObject var model: HarnessModel
    let onBack: () -> Void

    var body: some View {
        ZStack {
            VStack(spacing: 3) {
                Circle()
                    .fill(Color.blue.opacity(0.55))
                    .frame(width: 40, height: 40)
                    .overlay(
                        Text(model.contactInitials)
                            .font(.system(size: 15, weight: .semibold)).foregroundStyle(.white)
                    )
                Text(model.contactLabel)
                    .font(.system(size: 13, weight: .semibold)).foregroundStyle(.white)
            }
            HStack {
                Button(action: onBack) {
                    Image(systemName: "chevron.left")
                        .font(.system(size: 18, weight: .semibold))
                        .foregroundStyle(.blue)
                        .frame(width: 34, height: 34)
                }
                .accessibilityLabel("Back to chat list")
                Spacer()
            }
            .padding(.leading, 4)
        }
        .frame(height: ChatChrome.navBarHeight)   // named, not intrinsic — see ChatChrome's doc
        .background(Color(white: 0.05))
    }
}

/// The transcript — the ONLY scrolling region on the chat screen (requirement
/// 1). Each delivered bubble reads like a real Messages bubble (blue/right if
/// whichever participant is currently "you" sent it); the currently
/// staged-but-undelivered bubble, if any, renders as the LAST item and scrolls
/// WITH the transcript — exactly like a real Messages draft does, it is not
/// part of the fixed bottom area (only the compose bar and the extension
/// drawer itself are fixed).
private struct TranscriptScroll: View {
    @ObservedObject var model: HarnessModel

    var body: some View {
        // A plain ScrollView top-aligns sparse content, so with only 1-2
        // bubbles they'd float under the nav bar with a dead gap above the
        // compose bar — the opposite of real Messages, where a short
        // transcript still hugs the bottom. The GeometryReader measures the
        // scroll viewport so the inner VStack can be given `minHeight:` equal
        // to it with `alignment: .bottom`: once content is shorter than the
        // viewport, the leading Spacer absorbs the slack and pushes the
        // bubbles down to meet the compose bar; once content overflows, the
        // minHeight is moot and it scrolls normally.
        GeometryReader { outer in
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(spacing: 8) {
                        Spacer(minLength: 0)
                        ForEach(model.transcript) { msg in
                            MessageBubble(model: model, msg: msg, isMine: msg.senderId == model.localId)
                                .id(msg.id)
                        }
                        if model.staged != nil {
                            StagedPreviewBubble(model: model, onUnstage: { model.unstage() })
                                .id("staged")
                        }
                    }
                    .padding(.horizontal, 10).padding(.vertical, 12)
                    .frame(minHeight: outer.size.height, alignment: .bottom)
                }
                .onChange(of: model.transcript.count) { _ in scrollToEnd(proxy) }
                .onChange(of: model.staged) { _ in scrollToEnd(proxy) }
            }
        }
        .background(Color(white: 0.13))
    }

    private func scrollToEnd(_ proxy: ScrollViewProxy) {
        withAnimation {
            if model.staged != nil { proxy.scrollTo("staged", anchor: .bottom) }
            else if let last = model.transcript.last { proxy.scrollTo(last.id, anchor: .bottom) }
        }
    }
}

/// A GENUINE (not placeholder) preview of a Foolish game bubble: the IMAGE the
/// extension snapshotted when the move was staged (`HarnessModel.Msg.preview`),
/// which is literally what a real MSMessage carries — Messages renders the
/// extension once at insert time and that picture never changes afterwards. The
/// owner rejected a fake gradient here ("it should show the actual game preview
/// as in the real chat app"); this is the actual one, from the same
/// `BubbleSnapshot` entry the shipping extension composes with.
///
/// It used to mount a whole live `MessagesRootView` per bubble instead. That was
/// not just expensive, it was WRONG in a way that produced two of the round-3
/// bugs: each of those boards decoded its own (often stale) payload into the one
/// static resident kernel the open board was using, cached its own seat rows,
/// and ran its own animation stream. See `HarnessModel.Msg`.
private struct GamePreviewCard: View {
    let image: UIImage?

    /// The bubble picture's OWN aspect (BubbleSnapshot.size, 300x195), just
    /// scaled to bubble width — not a crop. The card is what Messages shows:
    /// the whole snapshot, top to bottom.
    private static let thumbWidth: CGFloat = 190
    private static let thumbHeight: CGFloat = thumbWidth * BubbleSnapshot.size.height / BubbleSnapshot.size.width

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Group {
                if let image {
                    Image(uiImage: image).resizable().aspectRatio(contentMode: .fill)
                } else {
                    // Only reachable if ImageRenderer failed at stage time.
                    Color(white: 0.25)
                }
            }
            .frame(width: Self.thumbWidth, height: Self.thumbHeight)
            .clipped()
            .allowsHitTesting(false)

            Text("Foolish")
                .font(.system(size: 13, weight: .semibold)).foregroundStyle(.black.opacity(0.85))
                .padding(.horizontal, 10).padding(.vertical, 7)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color(white: 0.93))
        }
        .frame(width: Self.thumbWidth)
        .clipShape(RoundedRectangle(cornerRadius: 13, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 13, style: .continuous)
            .strokeBorder(Color.white.opacity(0.15)))
    }
}

/// One delivered transcript entry, styled like a real Messages bubble —
/// its own `GamePreviewCard` points at `msg.url`, that PARTICULAR bubble's
/// own payload (not necessarily the live/current one), matching how a real
/// past message's preview never changes after the fact.
private struct MessageBubble: View {
    @ObservedObject var model: HarnessModel
    let msg: HarnessModel.Msg
    let isMine: Bool

    var body: some View {
        HStack {
            if isMine { Spacer(minLength: 36) }
            VStack(alignment: isMine ? .trailing : .leading, spacing: 2) {
                if !isMine {
                    Text(msg.senderName)
                        .font(.system(size: 10)).foregroundStyle(.white.opacity(0.5))
                }
                Button { model.openBubble(msg) } label: {
                    GamePreviewCard(image: msg.preview)
                }
                .buttonStyle(.plain)
            }
            if !isMine { Spacer(minLength: 36) }
        }
        .frame(maxWidth: .infinity, alignment: isMine ? .trailing : .leading)
    }
}

/// The staged-but-undelivered bubble — a stand-in for the real extension's
/// `insert`, previewing `model.stagedURL` (the draft that hasn't been sent
/// yet). The X mirrors the real extension's unstage/undo (§10, batch-1 note
/// 32).
private struct StagedPreviewBubble: View {
    @ObservedObject var model: HarnessModel
    let onUnstage: () -> Void

    var body: some View {
        HStack {
            Spacer(minLength: 36)
            GamePreviewCard(image: model.stagedPreview)
                .overlay(alignment: .topTrailing) {
                    Button(action: onUnstage) {
                        Image(systemName: "xmark")
                            .font(.system(size: 9, weight: .bold)).foregroundStyle(.white)
                            .frame(width: 20, height: 20)
                            .background(Circle().fill(Color.black.opacity(0.45)))
                    }
                    .padding(6)
                }
        }
    }
}

/// The fixed "Add comment or Send" row. In real Messages the compose field
/// lives exactly here — above the extension drawer, never inside the
/// scrolling transcript. The blue arrow is the harness's existing
/// `model.deliver()` control, just re-homed from the old bottom-of-controls
/// row into the spot it actually occupies on a real phone. The "+" button is
/// real Messages' own app-drawer icon: this is the ONLY way back into a
/// dismissed extension (see ExtensionStage's doc) — there is no separate
/// "reopen" affordance taking up its own row, exactly like the real app.
private struct ComposeBar: View {
    @ObservedObject var model: HarnessModel

    var body: some View {
        HStack(spacing: 8) {
            Button { model.reopenDrawer() } label: {
                Image(systemName: "plus.circle.fill")
                    .font(.system(size: 24))
                    .foregroundStyle(model.drawerDismissed ? Color.blue : .white.opacity(0.3))
            }
            Text(model.staged != nil ? "Send" : "Add comment or Send")
                .font(.system(size: 14)).foregroundStyle(.white.opacity(0.45))
                .padding(.horizontal, 12).padding(.vertical, 7)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Capsule().fill(Color.white.opacity(0.08)))
            Button { model.deliver() } label: {
                Image(systemName: "arrow.up")
                    .font(.system(size: 13, weight: .bold)).foregroundStyle(.white)
                    .frame(width: 26, height: 26)
                    .background(Circle().fill(model.staged != nil ? Color.blue : Color.white.opacity(0.15)))
            }
            .disabled(model.staged == nil)
        }
        .padding(.horizontal, 10)
        .frame(height: ChatChrome.composeBarHeight)   // named, not intrinsic — see ChatChrome's doc
        .background(Color(white: 0.07))
    }
}

/// The real extension's drawer, filling the bottom of the shared `SEScreen`
/// frame (requirement 4 — this no longer owns its own width or centering;
/// `HarnessRootView.simulatedPhone` fixes the WHOLE simulated app to
/// 375x667pt and this just fills whatever width/height that leaves it).
///
/// Heights, researched (2026-07-22 session) rather than re-guessed: an old
/// Apple Developer Forums thread (developer.apple.com/forums/thread/50497)
/// measured a live MSMessagesAppViewController's own `view.bounds.height` at
/// 568pt expanded / 253pt compact on an iPhone SE — i.e. EXPANDED IS ~THE
/// WHOLE SCREEN (568pt was that SE's entire logical height at the time), not
/// "screen minus all the chrome"; the nav bar/compose bar apparently overlay
/// via safe-area insets rather than shrinking the extension's own frame.
/// `compactHeight` (261) is close to that same ratio and the owner confirmed
/// it reads right by eye, so it's kept. `expandedHeight` (550) is nearly all
/// of what's left of the fixed 667pt `SEScreen` budget once `ContactNavBar` +
/// both dividers + `ComposeBar` (~103pt total) are subtracted — the owner
/// found an earlier, more conservative value (520) left the drawer's top
/// edge visibly too low; 550 leaves only a sliver (~14pt) of transcript above
/// the compose bar, matching the reference screenshot's own proportions (it
/// shows a staged bubble barely peeking through, not a fat gap).
///
/// Real Messages lets you DRAG the drawer — swipe up from compact to expand,
/// swipe down from expanded to collapse, swipe down again from an already-
/// compact drawer to dismiss it entirely. Dismissing takes the drawer's
/// space away completely (no leftover row) — the ONLY way back in is the
/// compose bar's "+" button, exactly like real Messages' app-drawer icon;
/// see `ComposeBar`. `requestExpand` is threaded into MessagesRootView but is
/// never actually CALLED anywhere in FoolishKit today (confirmed by grep) —
/// production expand/collapse is entirely host-driven, which is exactly what
/// the drag gesture below reproduces; nothing in FoolishKit needed to change.
private struct ExtensionStage: View {
    @ObservedObject var model: HarnessModel
    /// Live drag offset — a bit of rubber-band feedback while dragging so the
    /// grabber doesn't feel dead before the threshold triggers a transition.
    @State private var dragOffset: CGFloat = 0

    /// The compact drawer's height, read by `ChatScreen` too — it reserves
    /// exactly this much room under the compose bar so the two are adjacent
    /// rather than overlapping (see that type's doc).
    static let compactHeight: CGFloat = StageHeight.compact
    /// The drawer's top corners, rounded in both styles like the real app's.
    /// Smaller than the phone frame's own 30 so an expanded drawer reads as a
    /// sheet sitting inside the screen, not as the screen itself.
    static let topCornerRadius: CGFloat = 14

    private enum StageHeight {
        /// The entire SE screen. Expanded covers literally everything — nav
        /// bar and transcript included — matching the owner's real-device
        /// observation ("it expands OVER the rest of the iMessage stuff").
        /// The compose bar is not covered but REMOVED while expanded (see
        /// `ChatScreen`), so there is nothing left to subtract here.
        static let expanded: CGFloat = SEScreen.height
        /// Independently defined (both by Apple's docs and by the forum
        /// measurement in `ChatChrome`'s doc) as roughly a filled keyboard's
        /// height, which doesn't scale with screen size the way `expanded`
        /// does. Owner-verified by eye.
        static let compact: CGFloat = 261
    }

    var body: some View {
        // Dismissed = gone, no leftover row (the owner's explicit ask — the
        // compose bar's "+" is the only way back in, not a second control
        // taking up its own space). EmptyView contributes zero layout size.
        if model.drawerDismissed { EmptyView() } else { dockedStage }
    }

    private var dockedStage: some View {
        HostedStage {
            MessagesRootView(
                payloadURL: model.payloadURL,
                style: model.presentation,
                senderIsLocal: model.senderIsLocal,
                startNewGame: model.startNewGame,
                chatKey: model.chatKey,
                chatIsDM: model.chatIsDM,
                chatPlayers: model.playerCount,
                requestExpand: { model.expand() },
                onNewGame: { model.newGame() },
                onSend: { payload, seat, fromUndo in await model.stage(payload, seat: seat, fromUndo: fromUndo) },
                onUnstage: { model.unstage() }
            )
            .id(model.viewKey)   // reset @State when player/chat/transcript/intent changes
        }
        // No rounding/centering of its own anymore (requirement 4) — the
        // outer SEScreen frame (HarnessRootView.simulatedPhone) is what's
        // rounded/bordered/clipped now, the same "device silhouette" this
        // used to draw itself. `.clipped()` is NOT optional, though: the
        // outer frame's clipShape only clips at the SE box's own OUTER edge —
        // it does nothing to stop one VStack row from painting over its
        // siblings INSIDE that box. And WoolBackground's `.ignoresSafeArea()`
        // (deep inside MessagesRootView, several SwiftUI layers below
        // HostedStage's UIHostingController) does exactly that if left
        // unclipped: it ignores the height this view was actually given and
        // paints over the ENTIRE remaining box, burying the nav bar/
        // transcript/compose bar rows above it — found empirically
        // (screenshotted, the grabber landed mid-board instead of at its
        // top edge). The clip forces the rendered content back to this view's
        // own 375 x `height` rect regardless of what's happening inside the
        // hosted controller.
        //
        // That clip is a TOP-ROUNDED rect, not a plain `.clipped()`: real
        // Messages rounds the app drawer's top corners in BOTH styles (the
        // owner's ask), and doing it here — rather than with a decorative
        // overlay — means the wool is genuinely absent from the corners
        // instead of covered up.
        .frame(width: SEScreen.width, height: height)
        .clipShape(UnevenRoundedRectangle(topLeadingRadius: Self.topCornerRadius,
                                          topTrailingRadius: Self.topCornerRadius,
                                          style: .continuous))
        .offset(y: dragOffset)
        .animation(.easeOut(duration: 0.15), value: dragOffset)
        // Round-4 note 2 / round-10 #1: the drawer's HEIGHT changes in one
        // step, never over a tween - and that ONE-STEP SNAP is now this fake
        // drawer's most faithful piece of device modelling. The real Messages
        // does exactly the same on an animated style change: it sets the
        // hosting view's model height to the target in one step and animates
        // only the visible frame (the round-10 frame-by-frame film). The
        // production fix for the resulting jump lives INSIDE MessagesRootView
        // (its `stageHeight` smoothing tweens through a big height step), so
        // keeping the snap here is what exercises that code the way the
        // device does. Round-4's original worry - the tween re-flying a
        // just-played card - is settled: the round-6 continuous
        // collapseFraction made the transit layout the owner-approved look
        // (the manual swipe, which IS a tween, "transitions just fine").
        //
        // The drag gesture's own `dragOffset` above is untouched and still
        // animates: it is a TRANSLATION, so it never resizes the hosting
        // controller and never reflows anything.
        .overlay(alignment: .top) { grabber }
    }

    /// The visible grabber capsule PLUS a generously oversized invisible hit
    /// target around it (real Messages lets you grab anywhere near the handle,
    /// not just its exact 5pt-tall pixels) — this is the one draggable region;
    /// the rest of the stage is left alone so in-game taps still reach the
    /// board underneath.
    private var grabber: some View {
        Capsule()
            .fill(Color.white.opacity(0.35))
            .frame(width: 36, height: 5)
            .padding(.top, 6)
            .frame(width: 140, height: 34)
            .contentShape(Rectangle())
            .gesture(dragGesture)
    }

    private var dragGesture: some Gesture {
        DragGesture(minimumDistance: 6)
            .onChanged { dragOffset = $0.translation.height }
            .onEnded { value in
                let dy = value.translation.height
                dragOffset = 0
                // Written as a switch (not `==`) so it doesn't depend on
                // `MsgPresentation` (FoolishKit) being Equatable — same reason
                // as `height` below.
                switch model.presentation {
                case .expanded:
                    if dy > 60 { model.togglePresentation() }        // swipe down -> collapse
                case .compact:
                    if dy < -40 { model.expand() }                   // swipe up -> expand
                    else if dy > 40 { model.dismissDrawer() }         // swipe down again -> dismiss
                }
            }
    }

    /// `model.presentation`'s stage height. Written as a `switch` (not `==`)
    /// so it doesn't depend on `MsgPresentation` (FoolishKit) being Equatable.
    private var height: CGFloat {
        switch model.presentation {
        case .expanded: return StageHeight.expanded
        case .compact: return StageHeight.compact
        }
    }
}

/// Hosts `content` in its OWN `UIHostingController`, exactly the way the real
/// extension is hosted: `MSMessagesAppViewController` is a genuinely separate
/// view controller with no ambient safe area to bleed in from a host app's
/// chrome. The harness used to embed `MessagesRootView` as a plain SwiftUI
/// child of `HarnessRootView`'s own hierarchy — a `.frame(width:height:)`-boxed
/// ZStack child, nowhere near a physical screen edge. That still worked for
/// everything EXCEPT `WoolBackground`'s `.ignoresSafeArea()` (needed so the
/// wool bleeds edge-to-edge in the real, edge-to-edge extension): nested that
/// many SwiftUI layers deep, `.ignoresSafeArea()` expanded the wool's internal
/// aspect-fill proposal against the HARNESS APP's own ambient safe area, not
/// against the fixed stage box the tester actually sees — so the wool image
/// fell short of the stage's own edge, a gap the fixed-size clip then exposed
/// as bare black. A real `UIHostingController`, added as a proper child
/// controller, computes its OWN `safeAreaInsets` from its OWN view's position,
/// so `.ignoresSafeArea()` inside it has nothing left over to reach for and
/// just fills the box it was actually given — verified empirically
/// (screenshotted before/after; this was the fix that closed the gap).
private struct HostedStage<Content: View>: UIViewControllerRepresentable {
    @ViewBuilder let content: () -> Content

    func makeUIViewController(context: Context) -> UIHostingController<Content> {
        let vc = UIHostingController(rootView: content())
        vc.view.backgroundColor = .clear
        // 2026-07-22 session: nesting the stage inside the new SE-frame
        // container (requirement 4 — the WHOLE simulated Messages app, not
        // just the extension, now lives inside one fixed 375x667 box)
        // reintroduced a variant of the exact bug this type's doc already
        // describes once: WoolBackground's `.ignoresSafeArea()`, several
        // SwiftUI layers below this hosting controller, was aspect-filling
        // against the wrong (much larger) ambient safe area instead of this
        // controller's own small given frame — not a black gap this time,
        // but the opposite: the wool image scaled to fill some huge implied
        // canvas, so the small box we actually see is just one over-zoomed,
        // nearly-solid-colored corner of it (screenshotted, compared against
        // a real device — confirmed NOT an iPhone SE quirk, a genuine host
        // bug). `safeAreaRegions = []` (iOS 16.4+) tells the hosting
        // controller to stop propagating ANY ambient safe area into its
        // SwiftUI content at all, so `.ignoresSafeArea()` inside has nothing
        // left to expand against beyond this controller's own actual bounds.
        if #available(iOS 16.4, *) {
            vc.safeAreaRegions = []
        }
        return vc
    }

    func updateUIViewController(_ vc: UIHostingController<Content>, context: Context) {
        vc.rootView = content()
    }
}

/// The AnimLog trace, on screen. The board's animation triggers are SwiftUI
/// lifecycle events, so "did that run twice, and why" is only answerable from a
/// real run — and on a phone there is no console to read it in. Tap a move, then
/// read (or screenshot) this. Dev-only; see AnimLog.
private struct AnimLogPanel: View {
    @ObservedObject private var store = AnimLogStore.shared

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("ANIMLOG").font(.system(size: 11, weight: .bold)).foregroundStyle(.orange)
                Spacer()
                Button("clear") { store.clear() }
                    .font(.system(size: 11)).foregroundStyle(.orange)
            }
            .padding(.horizontal, 8).padding(.vertical, 4)
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 1) {
                        ForEach(Array(store.lines.enumerated()), id: \.offset) { i, l in
                            Text(l)
                                .font(.system(size: 9, design: .monospaced))
                                .foregroundStyle(l.contains("stream#") ? .green
                                                 : (l.contains("->") ? .yellow : .white))
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .id(i)
                        }
                    }
                    .padding(.horizontal, 6)
                }
                .onChange(of: store.lines.count) { n in
                    withAnimation { proxy.scrollTo(n - 1, anchor: .bottom) }
                }
            }
        }
        .frame(width: 330, height: 420)
        .background(Color.black.opacity(0.9))
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(.orange.opacity(0.5)))
        .allowsHitTesting(true)
    }
}
