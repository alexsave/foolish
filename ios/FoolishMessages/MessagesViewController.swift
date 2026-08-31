// MessagesViewController — the iMessage extension's entry point (design §11).
//
// Messages instantiates this class (Info.plist NSExtensionPrincipalClass) and
// drives it through the lifecycle below. What it renders comes from FoolishKit
// (the shared board + engine); what it stages comes from MessageComposer. This
// file owns the three things SwiftUI must not: the MSConversation, the insert of
// a staged bubble (§11.4 — the human always presses send), and the App Group
// cache commit that happens exactly when a send actually starts.
//
// THE RULE, restated because this is where it is most tempting to break: no Durak
// rule is answered here. Whose move, whether a move is legal, which chain wins,
// whether a staged move survives — all C (msg_wire.c via MessageKernel). Seat
// identity is the one non-kernel call, and it is SeatIdentity's pure §6 logic.
import Combine
import UIKit
import Messages
import SwiftUI
import FoolishKit

final class MessagesViewController: MSMessagesAppViewController {

    private var host: UIHostingController<MessagesRootView>?
    /// Set when the user taps New game so the next expanded present deals a
    /// genesis game rather than routing a selected bubble.
    private var startingNewGame = false
    /// The next bubble opens a NEW MSSession, rather than collapsing into the
    /// card of the game it came from.
    ///
    /// Separate from `startingNewGame`, which is a claim about the SURFACE ("the
    /// user asked for the New game screen") and routes straight to setup. A
    /// REMATCH needs the session half and not the surface half: it has already
    /// built its lobby out of the finished board, and asking for setup threw
    /// that lobby away - the rematch bubble staged correctly and the extension
    /// showed New game / Create game behind it. Found on the simulator, 1.0(17).
    private var freshSession = false
    /// Incremented on each New game tap. Threaded into MessagesRootView so an
    /// explicit New game resets the session, while a compact<->expanded style
    /// toggle (same token) preserves the in-progress game.
    private var newGameToken = 0
    /// The name of the player whose LEAVE is about to be staged (round 16), or
    /// nil. Set by the lobby's Exit and consumed by the very next `stage` -
    /// one bubble, one announcement.
    private var pendingLeftName: String?
    /// Incremented on each real SEND (didStartSending). Threaded into
    /// MessagesRootView so the live board can drop the just-sent move from its
    /// pending list (`markSent`) - otherwise the Undo button lingered in the
    /// collapsed view and re-staged an already-sent move (round-6 bug 4).
    private var sentToken = 0
    /// The payload we last staged (via `insert`), awaiting the human's send/cancel
    /// (§7.6). Committed to the cache on didStartSending, dropped on cancel. Carries
    /// the game id so an explicit CANCEL can clear that game's pending ledger
    /// synchronously (round-6 bugs 1 & 2), without a second async decode.
    private var pendingStage: (payload: Data, mySeat: Int, gameId: String)?
    /// note 11: the `payloadURL` the last `present()` call actually used —
    /// what `StagedBubbleRouting.resolvedPayloadURL` falls back to when the
    /// newly-selected message turns out to be our OWN just-staged bubble
    /// (see that type's doc), so the live GameSurface's `loadKey` doesn't
    /// change and the board isn't torn down and rebuilt out from under itself.
    private var lastPayloadURL: URL?
    /// The payload this device most recently SENT. `pendingStage` is cleared at
    /// `didStartSending`, but Messages leaves that bubble selected — so without
    /// this the next `present()` reloaded the whole surface from my own just-sent
    /// chain and replayed the move I had just watched myself play (round-3's
    /// double animation). See StagedBubbleRouting.
    private var lastSentPayload: Data?
    /// The bubble that last ARRIVED while we are on screen (`didReceive`), and a
    /// token bumped per arrival. Apple does not make an arrival the
    /// `selectedMessage`, so `payloadURL`/loadKey never move for it — the surface
    /// folds it in separately (GameSurface.maybeAdoptIncoming), Rule P deciding.
    /// Without this a player stranded on a losing Start fork stayed stranded
    /// until they happened to re-tap a bubble (the 4-player double-Start
    /// deadlock).
    private var incomingURL: URL?
    private var incomingToken = 0
    /// Round-9: bumped when the human deletes the staged bubble
    /// (didCancelSending), so the surface can drop its send reminder.
    private var cancelToken = 0
    /// Round-10d: the collapse arm, delivered in place (no re-present, which
    /// would reload the board mid-transition) - see CollapseSignal.
    private let collapseSignal = CollapseSignal()

    // MARK: - Lifecycle (§11.1)

    /// ROUND 16, the flight recorder. Opened as the very first thing this
    /// extension does, because everything before it is invisible to the trail -
    /// and the failure being chased (the drawer freezing, "even on newer
    /// devices") leaves no other evidence: a memory kill is a SIGKILL, so
    /// nothing runs afterwards to report it. See FlightRecorder.
    /// Keeps the host's fallback colour in step with the TABLE MATERIAL, which
    /// is a preference and so cannot ride the trait collection. Without it,
    /// switching wool<->felt in Settings leaves the old material's colour behind
    /// the drawer until the next `present()`.
    private var prefsSink: AnyCancellable?

    override func viewDidLoad() {
        super.viewDidLoad()
        FlightRecorder.begin("style \(presentationStyle == .compact ? "compact" : "expanded")")
        prefsSink = FPrefs.shared.objectWillChange.sink { [weak self] _ in
            // objectWillChange fires BEFORE the value lands, so read it next turn.
            DispatchQueue.main.async { self?.applyTableFallback() }
        }
    }

    /// Paint the host and its hosting view with the current table's flat colour.
    private func applyTableFallback() {
        let colour = Self.tableFallback
        view.backgroundColor = colour
        host?.view.backgroundColor = colour
    }

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
        FlightRecorder.note("active", "\(conversation.remoteParticipantIdentifiers.count + 1)p chat")
        // A NEW ACTIVATION IS A NEW AUDIENCE. Any just-sent marker still lying
        // about belongs to a session that has ended - the player closed the
        // drawer and came back - and a reopen they chose is a request to watch
        // the bubble, not to be shown a blank board. See
        // MessageGameStore.clearJustSent for the owner report and why this sits
        // on the way IN rather than on the way out.
        if MessageGameStore.shared.clearJustSent() {
            FlightRecorder.note("quiet-drop", "a new activation replays the bubble")
        }
        present(conversation, style: presentationStyle)
    }

    /// The extension is going away in an orderly fashion. This is the goodbye
    /// line whose ABSENCE is how the next launch knows the previous session was
    /// killed rather than closed.
    override func didResignActive(with conversation: MSConversation) {
        super.didResignActive(with: conversation)
        FlightRecorder.end("resigned")
    }

    /// The one warning iOS gives before it starts killing extensions - and until
    /// round 16 this app did not implement it at all, so the warning arrived,
    /// nothing was given back, and the next allocation was fatal.
    ///
    /// Two things happen. The trail records it, which is what turns a later
    /// "ended abruptly" from a shrug into a diagnosis (FlightRecorder.verdict
    /// keys on exactly this). And the baked textures that are NOT on screen are
    /// handed back: measured at ~17.9 MB with every variant resident, of which a
    /// session only ever draws one table's worth - see
    /// FoolishTests/MemoryProfileTests, and FTextures.purgeUnusedTextures for
    /// what it costs to reload one (a file read; the reason it is safe to drop).
    override func didReceiveMemoryWarning() {
        super.didReceiveMemoryWarning()
        let scheme: ColorScheme = traitCollection.userInterfaceStyle == .dark ? .dark : .light
        let dropped = FTextures.purgeUnusedTextures(keeping: FTextures.Variant(scheme))
        FlightRecorder.note("memory-warning", "dropped \(dropped) textures")
    }

    /// A message arrived while we are on screen — an opponent may be live-playing.
    /// Rule P decides progress vs stale (never delivery order, §7.2): the arrival
    /// is threaded to the surface as `incomingURL` (it does NOT become the
    /// `selectedMessage`, so the ordinary payloadURL/loadKey path cannot see it)
    /// and GameSurface adopts it only if it strictly out-ranks what is showing.
    /// A fresh receive cancels any half-started New game.
    override func didReceive(_ message: MSMessage, conversation: MSConversation) {
        // ROUND 12 #11: an arrival that IS my own chain is not an arrival.
        // Messages delivers a sent bubble back to its sender on the simulator,
        // and to a second device on the same iCloud account for real. Threaded
        // on as new, the surface adopts it and arms the open-replay for the move
        // I just made - so the card I played vanishes under the veil and the
        // attack animates again. The live board already holds this exact chain
        // (these are the bytes it sealed), so there is nothing to fold in:
        // dropping it is not a shortcut, it is the whole of the correct action.
        // See StagedBubbleRouting.isMine.
        if StagedBubbleRouting.isMine(Self.payload(of: message),
                                      pendingStage: pendingStage?.payload,
                                      lastSentPayload: lastSentPayload) { return }
        startingNewGame = false
        freshSession = false
        FlightRecorder.note("receive")
        incomingURL = message.url
        incomingToken += 1
        present(conversation, style: presentationStyle)
    }

    /// The user tapped Send on our staged bubble: our chain is now the thread's,
    /// so commit it to the cache (§7.6). This is the ONLY place the cache learns
    /// a chain was actually sent — insert alone is not a commit.
    override func didStartSending(_ message: MSMessage, conversation: MSConversation) {
        startingNewGame = false
        freshSession = false
        // ROUND 12 #11: the chain being sent comes from the MESSAGE Messages
        // hands us, not from our own `pendingStage` bookkeeping. They are
        // normally the same bytes, but `pendingStage` can be gone by now (an
        // extension torn down between insert and send; a cancel report for a
        // bubble we already replaced), and when it is, the quiet-open marker
        // below was never written - so reopening my own sent chain replayed the
        // move I had just watched. The message is always authoritative.
        let sent = Self.payload(of: message) ?? pendingStage?.payload
        // WITH THE BYTE COUNT, because a send that reached this function
        // without its bytes is what walked the board back a bubble in 1.0(26)
        // (MessageTurnController.markSent). The board no longer depends on them
        // arriving, but WHERE they go missing - never here, or lost between
        // here and the surface's `onChange` - is still worth knowing, and the
        // trail can only say so if this line counts them.
        FlightRecorder.note("send", sent.map { "\($0.count)b" } ?? "NO PAYLOAD")
        lastSentPayload = sent
        commitPendingStage(sent: sent, chatKey: ChatKey.make(
            local: conversation.localParticipantIdentifier.uuidString,
            remotes: conversation.remoteParticipantIdentifiers.map(\.uuidString)))
        // Round-6 bug 4: signal the live board that its staged move is now sent, so
        // it drops it from `pending` and the collapsed drawer's Undo button (which
        // could otherwise re-stage and re-send the same move) goes away. Bump the
        // token and re-present so the new value reaches MessagesRootView;
        // StagedBubbleRouting keeps the payloadURL (hence loadKey) stable off
        // `lastSentPayload`, so the board is SIGNALLED, not torn down and reloaded.
        sentToken += 1
        present(conversation, style: presentationStyle)
        // ROUND 16 (owner): "if it's collapsed, then sending shouldn't close the
        // extension and go to the keyboard. Just keep it collapsed so they can
        // keep playing without having to tap and reopen anything."
        //
        // Round-6 bug 5 asked for the opposite ("sending should completely
        // close, if possible") and `dismiss()` is the closest Messages offers -
        // it tears the extension down and returns to the transcript, which is
        // also what raises the keyboard. That was the right answer for a send
        // made from the EXPANDED board, where the drawer is covering the thread
        // and the human is done. It is the wrong one from the compact drawer:
        // the ordinary move flow already ends there (a staged move collapses to
        // reach Messages' Send), so every send was closing a strip the player
        // was still using and charging them a tap to get back into the game.
        //
        // So the close is now scoped to the style it was asked for. Nothing
        // else about a send changes: `sentToken` above already tells the live
        // board its move is in the thread (markSent -> `canSend` false), which
        // is what clears the Undo button and the send hint - written as
        // belt-and-braces for the case where Messages kept the drawer open
        // anyway, and now the case that always happens.
        if presentationStyle != .compact { dismiss() }
    }

    /// The user deleted the staged bubble before sending: drop the pending record
    /// so the cache never claims a chain nobody will see (§17.2). ROUND 9: the
    /// durable pending ledger this used to clear is gone entirely (owner call).
    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
        // ROUND 12 #11: clear the staging only if THIS is the bubble that was
        // cancelled. Staging a second move (a throw-in, a re-stage after Undo)
        // replaces the input-field bubble, and Messages reports the replaced one
        // as cancelled - after we have already recorded its successor. Clearing
        // unconditionally threw away a live staging, and with it the quiet-open
        // marker its send would have written.
        if let p = pendingStage,
           let cancelled = Self.payload(of: message), cancelled != p.payload { return }
        pendingStage = nil
        // Round-9: tell the surface nothing awaits Send any more, so the send
        // reminder (which now also covers lobby join/invite/start bubbles)
        // doesn't keep pointing at a bubble the human just deleted.
        cancelToken += 1
        present(conversation, style: presentationStyle)
    }

    override func willTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.willTransition(to: presentationStyle)
        // Round-7 #5 ("when we auto-collapse it should be the same as if we swiped
        // to collapse"; "it rearranges the display right before the auto collapse").
        // A plain compact<->expanded toggle must NOT re-present: the board is laid
        // out purely from the HEIGHT Messages gives its GeometryReader (a continuous
        // `collapseFraction`), never from the `style` prop, so it follows the drawer
        // resize smoothly on its own. Re-presenting here swapped the entire hosting
        // rootView at the START of the transition - that swap is the "display
        // rearranges right before the collapse" jump, and it is what made the
        // auto-collapse look different from a manual grabber swipe (a swipe the
        // human drives frame-by-frame hit the same swap but masked it under the
        // drag). Now BOTH just resize.
        //
        // The ONE case that still needs a routed present is New game: it flips
        // `startingNewGame` and requests .expanded, and the incoming transition is
        // where that intent has to be rendered (reading `self.presentationStyle`
        // mid-transition would still report the old style). So present only then.
        if startingNewGame, let c = activeConversation {
            present(c, style: presentationStyle)
        }
    }

    /// Round-10b: continuations parked by `awaitTransitionSettled`, resumed
    /// when `didTransition` reports the style change has completed.
    private var transitionWaiters: [Int: CheckedContinuation<Void, Never>] = [:]
    private var transitionWaiterSeq = 0

    override func didTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.didTransition(to: presentationStyle)
        FlightRecorder.note("style", presentationStyle == .compact ? "compact" : "expanded")
        let waiters = transitionWaiters
        transitionWaiters.removeAll()
        waiters.values.forEach { $0.resume() }
    }

    /// Wait until the in-flight presentation-style transition finishes
    /// (didTransition), or a timeout if none ever fires - the caller must
    /// never hang on a transition Messages decided not to run.
    @MainActor
    private func awaitTransitionSettled(timeoutNs: UInt64 = 1_200_000_000) async {
        transitionWaiterSeq += 1
        let id = transitionWaiterSeq
        await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
            transitionWaiters[id] = c
            Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: timeoutNs)
                if let waiter = self?.transitionWaiters.removeValue(forKey: id) {
                    waiter.resume()
                }
            }
        }
    }

    // MARK: - Presentation

    private func present(_ conversation: MSConversation, style: MSMessagesAppPresentationStyle) {
        let selected = conversation.selectedMessage
        // note 11: `conversation.insert` (in `stage`, below) makes the
        // just-staged bubble `selectedMessage`; the auto-collapse that
        // follows fires `willTransition` -> here with THAT bubble now
        // selected. Route it through `StagedBubbleRouting` so a match against
        // `pendingStage` reuses `lastPayloadURL` instead of tearing down the
        // live board to "adopt" the move it just watched itself play — see
        // that type's doc for the full chain.
        let payloadURL = StagedBubbleRouting.resolvedPayloadURL(
            selectedURL: selected?.url, startingNewGame: startingNewGame,
            pendingStage: pendingStage.map { (payload: $0.payload, mySeat: $0.mySeat) },
            lastPayloadURL: lastPayloadURL,
            lastSentPayload: lastSentPayload)
        lastPayloadURL = payloadURL
        // §6.2 S1's exact half: did THIS device send the tapped bubble? Only the
        // extension can answer — the participant UUIDs never travel in the payload.
        let senderIsLocal = selected?.senderParticipantIdentifier != nil
            && selected?.senderParticipantIdentifier == conversation.localParticipantIdentifier

        // Chat shape (B4 feedback): a 1:1 DM can only ever be a 2-player game; a
        // group chat defaults to its participant count but still allows 2-8. Total
        // participants = remote + me, clamped to the wire's 2-8.
        let participants = min(max(conversation.remoteParticipantIdentifiers.count + 1, 2), 8)
        let isDM = conversation.remoteParticipantIdentifiers.count <= 1

        // The chat-scoping security fix: scope every MessageGameStore lookup to
        // THIS conversation, so chat B can never reopen chat A's board (and never
        // stage chat A's deal-seed-bearing payload into it). Keyed on the whole
        // PARTICIPANT SET, not `localParticipantIdentifier` alone — that one is
        // the same UUID in every thread on a device, so it scoped by device and
        // the leak survived. See ChatKey for the full reasoning.
        let chatKey = ChatKey.make(local: conversation.localParticipantIdentifier.uuidString,
                                   remotes: conversation.remoteParticipantIdentifiers.map(\.uuidString))

        let root = MessagesRootView(
            payloadURL: payloadURL,
            style: style == .compact ? .compact : .expanded,   // map onto FoolishKit's enum
            senderIsLocal: senderIsLocal,
            startNewGame: startingNewGame,
            newGameToken: newGameToken,
            sentToken: sentToken,
            // ROUND 16: the chain that went out, so the live controller can
            // rebase onto it. It used to be rebuilt from these same bytes by the
            // teardown a send caused; the drawer survives now, so the bytes have
            // to travel instead of the teardown.
            sentPayload: lastSentPayload,
            chatKey: chatKey,
            chatIsDM: isDM,
            chatPlayers: participants,
            incomingURL: incomingURL,
            incomingToken: incomingToken,
            cancelToken: cancelToken,
            collapseSignal: collapseSignal,
            requestExpand: { [weak self] in self?.requestPresentationStyle(.expanded) },
            onNewGame: { [weak self] in
                guard let self else { return }
                self.startingNewGame = true
                self.freshSession = true
                self.newGameToken += 1
                // If already expanded, requesting .expanded fires no transition, so
                // present now; otherwise expand and let willTransition present with
                // startNewGame set.
                if self.presentationStyle == .expanded {
                    if let c = self.activeConversation { self.present(c, style: .expanded) }
                } else {
                    self.requestPresentationStyle(.expanded)
                }
            },
            // A REMATCH starts a new game without a teardown: it builds its
            // lobby in place from the finished board. All it needs from here is
            // the session half of `onNewGame` - a FRESH MSSession, so the
            // rematch's first bubble does not collapse the result card of the
            // game it grew out of (see the session note in `stage`). Cleared by
            // didStartSending/didReceive, exactly like the New game tap's.
            onFreshChain: { [weak self] in self?.freshSession = true },
            // Round 16: who just walked out of the lobby. Only the leaver's own
            // device knows - the join that carried the name is what the leave
            // removed - so it says so here, and `stage` puts it in the
            // transcript line before clearing it.
            onAnnounceLeave: { [weak self] name in self?.pendingLeftName = name },
            onSend: { [weak self] payload, mySeat, fromUndo in
                await self?.stage(payload: payload, mySeat: mySeat, fromUndo: fromUndo)
            },
            onUnstage: { [weak self] in
                // Messages provides no API to remove an already-inserted input-field
                // bubble — the human deletes it manually, or the next stage() call
                // replaces it. All we can retract is our own bookkeeping, so a
                // resumed undo-to-empty doesn't later commit a stale chain on send.
                // (ROUND 9: the durable ledger this also used to clear is gone.)
                self?.pendingStage = nil
            },
            // The result screen's Replay Link. `extensionContext.open` is the
            // ONLY way out of an iMessage extension - there is no
            // `UIApplication.shared` to ask (this target is built
            // extension-API-only, so reaching for one would not even compile),
            // and SwiftUI's `openURL` has no host to fall back on here. The
            // extension is torn down as Safari comes up, which is why the code
            // behind the link is captured when the game ends rather than read
            // on the way out (MessageTurnController.publish).
            //
            // ROUND 20 stopped throwing the ANSWER away. The completion handler
            // was `nil`, so a system that declined to open the URL - which is
            // what iOS does with an arbitrary https link from an extension, see
            // FGameOverList.onOpenURL - was indistinguishable from one that
            // opened it, and the tap did nothing with nothing to say. It is
            // reported up now, and the board falls back to the pasteboard.
            onOpenURL: { [weak self] url in
                FlightRecorder.note("open-url", url.host ?? "?")
                guard let ctx = self?.extensionContext else { return false }
                return await withCheckedContinuation { k in
                    ctx.open(url) { ok in k.resume(returning: ok) }
                }
            })
        setRoot(root)
    }

    /// Compose the staged bubble and insert it into the input field (§11.3/§11.4).
    /// The picture is the PUBLIC table (§10) rendered from the resident game the
    /// seal just left in place. Insert only STAGES — the human sends.
    @MainActor
    private func stage(payload: Data, mySeat: Int, fromUndo: Bool = false) async {
        guard let conversation = activeConversation else { return }
        // NEWEST STAGE WINS, and the losers stop where they stand.
        //
        // This function is re-entrant and its expanded tail is over a second
        // long (a settle wait, a collapse, a transition wait), while a second
        // move - a throw-in, a re-stage after Undo, a fast double tap - starts
        // another one immediately. Nothing serialised them, so two runs raced to
        // `conversation.insert`, and the input field kept whichever landed LAST:
        // routinely the older bubble, because a run that starts while the drawer
        // is already collapsing skips the whole tail and inserts at once. That
        // is not a flicker - `pendingStage` and the inserted message are the
        // bytes Send actually transmits, so the move the player watched
        // themselves make would not be the move that went out.
        //
        // A generation counter, checked after every suspension: the run that
        // has been superseded neither records itself nor inserts.
        stageGeneration += 1
        let generation = stageGeneration
        func current() -> Bool { stageGeneration == generation }
        // READ the bubble and describe it, in one kit call (MessageSummary.
        // forStagedBubble): the read must not ADOPT - see there - and keeping
        // it beside the caption is what lets a test walk this exact path. The
        // leave name is this device's alone (round 16), and is spent here.
        let (env, publicView, summary) = await MessageSummary.forStagedBubble(
            payload: payload, leftName: pendingLeftName)
        // Spent only by the bubble that can say it - a lobby re-seal. Any other
        // bubble leaves it standing for the one that follows.
        if env?.phase == 0 { pendingLeftName = nil }
        // The picture is BubbleSnapshot's call, not this file's: a WAITING lobby
        // previews as its roster, everything else as the public table. Shared
        // with the harness's transcript so a preview can never disagree with the
        // extension (see BubbleSnapshot.render(env:)).
        // Round-7 #3: bake the bubble in THIS device's scheme so a dark-mode
        // sender's bubble is dark. traitCollection is the extension's live
        // appearance; map it onto SwiftUI's ColorScheme for BubbleSnapshot.
        let scheme: ColorScheme = traitCollection.userInterfaceStyle == .dark ? .dark : .light
        var image: UIImage?
        if let env { image = BubbleSnapshot.render(env: env, publicView: publicView, scheme: scheme) }

        // §12, revised by batch 6 item B: the FINISHED bubble stays a normal /m/
        // payload link, NOT `MessageEnvelope.replayLink`'s bare foolish.cards/<code>.
        // That bare link is unparseable by `MessageEnvelope.payloadBytes` (it has
        // no `/m/1<base32>` shape), so the RECEIVER of the final move tapped it
        // into the damaged-link screen and never saw the final board or its
        // animation (batch-3 finding). The replay funnel moves one hop out
        // instead: the web `/m/` page (src/app/m/[payload]/page.tsx) decodes the
        // FINISHED payload itself — it already runs the same kernel — and derives
        // the replay code THERE, rendering its own "Watch the replay" CTA
        // alongside the install/play ones. This bubble only needs the fool
        // announcement — `residentReplayCode()`/`replayLink` (sdk/swift/
        // MessageEnvelope.swift) still exist and are still exercised by
        // MessageTurnControllerTests (the underlying kernel capability the web
        // page's replay derivation mirrors), just no longer called from here.
        let url = MessageEnvelope.link(payload: payload)

        // §11.3/note 21: ONE session per game, and a NEW game must never collapse
        // the PREVIOUS game's final bubble. Messages collapses every older bubble
        // in the same `MSSession` down to its summaryText, keeping only the
        // latest interactive — which is exactly what we want WITHIN one game
        // (so the thread doesn't fill with 60 bubbles), but is wrong across two:
        // reusing `selectedMessage?.session` for the first bubble of a brand-new
        // game folds the just-finished game's result card into it, so the fool
        // announcement vanishes from the transcript the instant the next game
        // starts. `startingNewGame` is exactly "is this the first bubble of a
        // game that didn't exist a moment ago" — it is set on the New game tap
        // and only cleared by didReceive/didStartSending (see the property doc
        // above) — so passing `session: nil` there starts Messages a FRESH
        // session/bubble; every continuation after that (startingNewGame already
        // false) still reuses `selectedMessage?.session` to collapse within the
        // SAME game, unchanged.
        let msg = MessageComposer.message(
            url: url,
            snapshot: image,
            caption: "Foolish",
            summary: summary,
            session: freshSession ? nil : conversation.selectedMessage?.session)

        // gameId comes from the same decode above so didStartSending's commit
        // can persist the seat without re-decoding. "" only if the payload
        // failed to decode - the commit then skips the seat write.
        // Superseded while the picture was being baked: a newer move is already
        // staged, and this one must not claim the input field back off it.
        guard current() else { return }
        pendingStage = (payload, mySeat, env?.gameId ?? "")

        // An UNDO re-stages only to refresh the input bubble - it is NOT a move the
        // player is trying to send, it is them backing up to pick a DIFFERENT move.
        // Collapsing the board out from under them there is exactly wrong (owner:
        // "undo should NOT collapse the screen... best to keep it expanded for
        // moves"), so insert now, stay expanded, and skip the drop-to-Send tail.
        if fromUndo { conversation.insert(msg) { _ in }; return }

        // Already in the compact drawer (an ordinary in-drawer move): no style
        // transition will run, so there is no preview flyover to avoid - stage
        // the bubble immediately, exactly the pre-round-10b timing.
        if presentationStyle != .expanded {
            conversation.insert(msg) { _ in }
            return
        }

        // Drop the user straight at Messages' Send (§11.4): the expanded board has
        // no send control of its own — Send lives in the compose area — so once a
        // move is staged, collapse to compact instead of making them drag down. To
        // add more cards (throw-ins, a second cover) they just re-open the game;
        // the staged chain survives the style change (GameSurface @State).
        //
        // note 8: this USED TO be a flat 900ms sleep, tuned for a single card's
        // spring settle — a bout-ending "good" plays a whole discard+draw
        // cascade (one step per drawing player) that routinely runs longer,
        // and got guillotined mid-flight. A short lead-in first (the
        // `BoardAnimator.sequenceDepth` increment for such a cascade happens
        // inside a Task the SwiftUI `onChange` callback schedules, which can
        // lag a beat behind this function starting, so checking `isSequencing`
        // with zero lead-in would sometimes race it and see false); THEN
        // `waitForSettle()` for however long the real sequence takes (a plain
        // attack/cover has no sequence at all, so this returns almost at
        // once); THEN a rest so the settled result reads, not a flicker.
        try? await Task.sleep(nanoseconds: 250_000_000)
        await BoardAnimator.waitForSettle()
        try? await Task.sleep(nanoseconds: 500_000_000)
        guard current() else { return }

        // Round-10b (the residual "self cards go a bit under the screen"):
        // COLLAPSE FIRST, insert AFTER the transition settles. Inserting while
        // still expanded made Messages animate the brand-new input-field
        // bubble from a large preview into its compose slot ON TOP of the
        // collapsing drawer - and since the bubble's picture is the PUBLIC
        // table (no hand, no buttons), the board's bottom looked like it dove
        // under the screen until the preview landed. Filmed with a debug
        // ruler drawn on the live surface: the flying rect carried no ruler
        // lines, so it was never our view - it was the bubble preview. With
        // the insert deferred until didTransition, the collapse animates the
        // LIVE board alone (exactly like a manual swipe), and the bubble
        // simply appears in its slot at the end. (The already-compact case
        // returned above - this path is expanded-only.)
        //
        // Round-10c, the LAST piece: ruler-instrumented films proved the
        // style transition itself is SNAPSHOT compositing we cannot influence
        // (mid-flight imagery our live tree cannot produce), and it visibly
        // dropped the board's bottom half. So first PACK the board into a
        // compact-sized box at the drawer's bottom under our own animation -
        // the visible collapse, fully controlled, hand pinned - and only then
        // change style: both snapshot endpoints now share an identical bottom
        // strip, and all that shrinks away above it is featureless wool.
        // Round-10d: ARM the surface (no pre-animation of any kind - the
        // round-10c "pack the board up first" WAS the owner's "goes up, then
        // goes back down"), then request the collapse. The surface's own
        // height tween takes it from there; see MessagesRootView.follow.
        collapseSignal.token += 1
        requestPresentationStyle(.compact)
        await awaitTransitionSettled()
        // The last gate, and the one that matters: the newer run has already
        // put its own bubble in the field, so inserting here would replace it
        // with this older one.
        guard current() else { return }
        conversation.insert(msg) { _ in }
    }

    /// Which `stage` run owns the input field - see the note at the top of it.
    private var stageGeneration = 0

    /// The chain a message Messages reports actually carries. The message is the
    /// authority on its own bytes; our `pendingStage` bookkeeping is not (round
    /// 12 #11).
    private static func payload(of message: MSMessage) -> Data? {
        guard let u = message.url else { return nil }
        return try? MessageEnvelope.payloadBytes(url: u)
    }

    /// Commit a sent chain to the App Group cache (§6.1/§7.6): our seat becomes
    /// durable and this chain is the preferred one for the game. `chatKey` comes
    /// from `didStartSending`'s own conversation, not a stored property, because
    /// it must be the SAME conversation the bubble was staged/sent into (the
    /// whole point of the chat-scoping fix).
    ///
    /// ROUND-9 #5: fully SYNCHRONOUS. This used to decode the payload in a
    /// fire-and-forget Task just to learn the gameId - but `pendingStage`
    /// already carries it (round 6), and the Task raced the `dismiss()` /
    /// VC-swap teardown that follows a send. When the clear lost that race, the
    /// reopen of my OWN sent chain still saw the ledger rows, Rule R discarded
    /// them against a chain that already contains those moves, and every send
    /// ended in a "move superseded" toast + a replay of my own move. Three
    /// UserDefaults writes need no Task and cannot lose the race.
    ///
    /// ROUND 12 #11: `sent` is the chain the MESSAGE carries. The seat write
    /// still needs `pendingStage` (only it knows which seat and game we staged
    /// as), but the quiet-open marker does not - and it is the one that must
    /// never be skipped, so it is written from `sent` outside the guard.
    private func commitPendingStage(sent: Data?, chatKey: String) {
        // The durable half of `lastSentPayload` (round-9 #5): if the send tears
        // this VC down, the reopen consumes this and opens my own chain QUIETLY
        // (no self-replay) instead of treating it as a new arrival.
        if let sent { MessageGameStore.shared.markJustSent(payload: sent) }
        guard let (_, mySeat, gameId) = pendingStage else { return }
        pendingStage = nil
        if !gameId.isEmpty {
            // Round 7: the preferred-chain cache is gone — commit only the durable
            // SEAT (§6.1). The chain the human just sent is now the thread's, and
            // reopening it re-renders it from its own bytes. (ROUND 9: the pending
            // ledger this also used to clear is gone entirely - owner call.)
            MessageGameStore.shared.setSeat(gameId: gameId, chatKey: chatKey, seat: mySeat)
            // ROUND 20: and it is the newest chain this device has seen, by
            // construction - it was built ON the board that was open, which had
            // already been ranked against whatever was on file (GameSurface
            // .rankAgainstHighWater). Without this, tapping back to an older
            // bubble in the same session would not be recognised as a branch
            // until the next bubble arrived. No Rule P call: nothing this device
            // can hold beats a chain it just extended.
            if let sent { MessageGameStore.shared.setLatestChain(gameId: gameId, chatKey: chatKey, payload: sent) }
        }
    }

    /// Round-7 (background gap): a FALLBACK table colour painted on the host view
    /// behind the SwiftUI content, so if the table ever fails to reach an edge
    /// for a frame - e.g. the post-send reopen with the keyboard up - the
    /// exposed strip reads as a duller patch of the SAME board, never a
    /// system-black void.
    ///
    /// ROUND 22: it asks `FTextures`, which is the ONE place that knows what the
    /// table looks like right now, instead of naming wool's hexes itself.
    /// FTextures' own header says nothing outside it may name a resource or a
    /// hex, and this file was the exception - so a player on the FELT table got
    /// a strip of WOOL BROWN across the top of a green drawer (owner, 1.0(24):
    /// "little brown in the lobby top"). The material is a preference, not a
    /// trait, which is why the dynamic provider alone could not have been right:
    /// it re-resolves on a scheme change and never on a settings change.
    private static var tableFallback: UIColor {
        UIColor { tc in
            let scheme: ColorScheme = tc.userInterfaceStyle == .dark ? .dark : .light
            let hex = FTextures.tableFallbackHex(FTextures.Variant(scheme))
            return UIColor(red: CGFloat((hex >> 16) & 0xFF) / 255.0,
                           green: CGFloat((hex >> 8) & 0xFF) / 255.0,
                           blue: CGFloat(hex & 0xFF) / 255.0, alpha: 1)
        }
    }

    private func setRoot(_ root: MessagesRootView) {
        applyTableFallback()
        if let host {
            host.rootView = root
        } else {
            let h = UIHostingController(rootView: root)
            h.view.backgroundColor = Self.tableFallback
            addChild(h)
            h.view.translatesAutoresizingMaskIntoConstraints = false
            view.addSubview(h.view)
            NSLayoutConstraint.activate([
                h.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
                h.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
                h.view.topAnchor.constraint(equalTo: view.topAnchor),
                h.view.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            ])
            h.didMove(toParent: self)
            host = h
        }
    }
}
