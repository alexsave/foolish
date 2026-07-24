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
import UIKit
import Messages
import SwiftUI
import FoolishKit

final class MessagesViewController: MSMessagesAppViewController {

    private var host: UIHostingController<MessagesRootView>?
    /// Set when the user taps New game so the next expanded present deals a
    /// genesis game rather than routing a selected bubble.
    private var startingNewGame = false
    /// Incremented on each New game tap. Threaded into MessagesRootView so an
    /// explicit New game resets the session, while a compact<->expanded style
    /// toggle (same token) preserves the in-progress game.
    private var newGameToken = 0
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

    // MARK: - Lifecycle (§11.1)

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
        present(conversation, style: presentationStyle)
    }

    /// A message arrived while we are on screen — an opponent may be live-playing.
    /// Rule P decides progress vs stale (in MessageTurnController), never delivery
    /// order (§7.2). A fresh receive cancels any half-started New game.
    override func didReceive(_ message: MSMessage, conversation: MSConversation) {
        startingNewGame = false
        present(conversation, style: presentationStyle)
    }

    /// The user tapped Send on our staged bubble: our chain is now the thread's,
    /// so commit it to the cache (§7.6). This is the ONLY place the cache learns
    /// a chain was actually sent — insert alone is not a commit.
    override func didStartSending(_ message: MSMessage, conversation: MSConversation) {
        startingNewGame = false
        lastSentPayload = pendingStage?.payload
        commitPendingStage(chatKey: ChatKey.make(
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
        // Round-6 bug 5 ("sending should completely close, if possible"). This is
        // the closest Messages offers: dismiss() asks it to tear the extension down
        // and return to the conversation transcript. It is NOT guaranteed to fully
        // close in every host state (Messages may keep the app in the compact
        // drawer), which is exactly why the markSent signal above exists as the
        // belt-and-braces fix for bug 4 - if the drawer stays open, the Undo button
        // is already gone. There is no API for a harder close than this.
        dismiss()
    }

    /// The user deleted the staged bubble before sending: drop the pending record
    /// so the cache never claims a chain nobody will see (§17.2). Nothing was
    /// written to the game cache yet, but a staged move DOES leave a durable
    /// pending-ledger row (MessageTurnController.persistLedger, written on every
    /// staged action) so Rule R can survive a mid-staging interruption. An explicit
    /// cancel is the human backing the move all the way out, so that ledger row must
    /// go too - otherwise a reopen replays the never-sent move as a Rule R survivor
    /// and the board renders as if the move/start already happened (round-6 bugs 1 &
    /// 2: "stage then cancel makes it look like the game already started"). This is
    /// the CANCEL path only; a plain deactivation with a bubble still legitimately
    /// staged never reaches here, so the ledger's survive-an-interruption purpose is
    /// preserved.
    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
        if let gameId = pendingStage?.gameId { MessageGameStore.shared.clearPending(gameId: gameId) }
        pendingStage = nil
    }

    override func willTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.willTransition(to: presentationStyle)
        // Present with the INCOMING style, not `self.presentationStyle`: during a
        // transition the property still reports the OLD style, so reading it here
        // renders the compact drawer at full expanded height (and then New game,
        // which only requests expansion, no-ops because we are already expanded).
        if let c = activeConversation { present(c, style: presentationStyle) }
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
            chatKey: chatKey,
            chatIsDM: isDM,
            chatPlayers: participants,
            requestExpand: { [weak self] in self?.requestPresentationStyle(.expanded) },
            onNewGame: { [weak self] in
                guard let self else { return }
                self.startingNewGame = true
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
            onSend: { [weak self] payload, mySeat in
                await self?.stage(payload: payload, mySeat: mySeat)
            },
            onUnstage: { [weak self] in
                // Messages provides no API to remove an already-inserted input-field
                // bubble — the human deletes it manually, or the next stage() call
                // replaces it. All we can retract is our own bookkeeping, so a
                // resumed undo-to-empty doesn't later commit a stale chain on send.
                // Round-6 bugs 1 & 2: an undo-to-empty is the human backing the move
                // all the way out, so clear the durable ledger too (the controller's
                // own undo already empties it via persistLedger; clearing here keeps
                // the extension's retract path correct on its own terms). Same intent
                // as didCancelSending.
                if let gameId = self?.pendingStage?.gameId {
                    MessageGameStore.shared.clearPending(gameId: gameId)
                }
                self?.pendingStage = nil
            })
        setRoot(root)
    }

    /// Compose the staged bubble and insert it into the input field (§11.3/§11.4).
    /// The picture is the PUBLIC table (§10) rendered from the resident game the
    /// seal just left in place. Insert only STAGES — the human sends.
    @MainActor
    private func stage(payload: Data, mySeat: Int) async {
        guard let conversation = activeConversation else { return }
        // Re-decode (idempotent — re-adopts the same state) for the joins/summary.
        let env = try? await MessageEnvelope.decode(payload: payload, viewer: -1)
        let names = Dictionary((env?.joins ?? []).map { ($0.seat, $0.name) },
                               uniquingKeysWith: { a, _ in a })
        // The board the seal produced, spectator view (no hand) — bubble-safe,
        // and (below) the fool announcement's source.
        let publicView = await MessageKernel.shared.residentView(viewer: -1)
        // The picture is BubbleSnapshot's call, not this file's: a WAITING lobby
        // previews as its roster, everything else as the public table. Shared
        // with the harness's transcript so a preview can never disagree with the
        // extension (see BubbleSnapshot.render(env:)).
        // Round-7 #3: bake the bubble in THIS device's scheme so a dark-mode
        // sender's bubble is dark. traitCollection is the extension's live
        // appearance; map it onto SwiftUI's ColorScheme for BubbleSnapshot.
        let scheme: ColorScheme = traitCollection.userInterfaceStyle == .dark ? .dark : .light
        var image: UIImage?
        if let env { image = await BubbleSnapshot.render(env: env, scheme: scheme) }

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
        let url: URL
        let summary: String
        if env?.phase == 3 {
            url = MessageEnvelope.link(payload: payload)
            let fool = publicView?.gameOver ?? -1
            summary = fool >= 0
                ? FStrings.t("ios.msg.fool", ["name": names[fool] ?? "Seat \(fool + 1)"])
                : FStrings.t("ios.msg.tap")
        } else if env?.phase == 0 {
            // A WAITING lobby (§5.2): the summary invites the thread to join.
            url = MessageEnvelope.link(payload: payload)
            summary = FStrings.t("ios.msg.joininvite")
        } else if env?.phase == 2, env?.turn == 0 {
            // The last-joiner LIVE handoff carries no move yet - "game on".
            url = MessageEnvelope.link(payload: payload)
            summary = FStrings.t("ios.msg.gameon")
        } else {
            url = MessageEnvelope.link(payload: payload)
            summary = FStrings.t("ios.msg.tap")
        }

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
            session: startingNewGame ? nil : conversation.selectedMessage?.session)

        // gameId comes from the same decode above so a later CANCEL can clear this
        // game's pending ledger without re-decoding (round-6 bugs 1 & 2). "" only
        // if the payload failed to decode, in which case there is no valid game to
        // clear and clearPending("") is a harmless no-op.
        pendingStage = (payload, mySeat, env?.gameId ?? "")
        conversation.insert(msg) { _ in }

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
        requestPresentationStyle(.compact)
    }

    /// Commit a sent chain to the App Group cache (§6.1/§7.6): our seat becomes
    /// durable and this chain is the preferred one for the game. `chatKey` comes
    /// from `didStartSending`'s own conversation, not a stored property, because
    /// it must be the SAME conversation the bubble was staged/sent into (the
    /// whole point of the chat-scoping fix).
    private func commitPendingStage(chatKey: String) {
        guard let (payload, mySeat, _) = pendingStage else { return }
        pendingStage = nil
        Task {
            guard let env = try? await MessageEnvelope.decode(payload: payload, viewer: mySeat)
            else { return }
            let names = Dictionary(env.joins.map { ($0.seat, $0.name) },
                                   uniquingKeysWith: { a, _ in a })
            MessageGameStore.shared.put(MessageGameRecord(
                gameId: env.gameId, chatKey: chatKey, mySeat: mySeat, nPlayers: env.nPlayers, round: env.round,
                turn: env.turn, phase: env.phase, finished: env.phase == 3, names: names,
                payloadBase32: Base32.encode(payload),
                updatedAt: Date().timeIntervalSince1970))
            // The staged moves are now in the sent chain (this device's preferred
            // chain). They are no longer unacked, so drop them from the pending
            // ledger — Rule R must never replay a move on top of itself (§7.6).
            MessageGameStore.shared.clearPending(gameId: env.gameId)
        }
    }

    private func setRoot(_ root: MessagesRootView) {
        if let host {
            host.rootView = root
        } else {
            let h = UIHostingController(rootView: root)
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
