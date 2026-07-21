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
    /// The payload we last staged (via `insert`), awaiting the human's send/cancel
    /// (§7.6). Committed to the cache on didStartSending, dropped on cancel.
    private var pendingStage: (payload: Data, mySeat: Int)?

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
        commitPendingStage(chatKey: conversation.localParticipantIdentifier.uuidString)
    }

    /// The user deleted the staged bubble before sending: drop the pending record
    /// so the cache never claims a chain nobody will see (§17.2). Nothing was
    /// written yet, so there is nothing to roll back — just forget it.
    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
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
        // §6.2 S1's exact half: did THIS device send the tapped bubble? Only the
        // extension can answer — the participant UUIDs never travel in the payload.
        let senderIsLocal = selected?.senderParticipantIdentifier != nil
            && selected?.senderParticipantIdentifier == conversation.localParticipantIdentifier

        // Chat shape (B4 feedback): a 1:1 DM can only ever be a 2-player game; a
        // group chat defaults to its participant count but still allows 2-8. Total
        // participants = remote + me, clamped to the wire's 2-8.
        let participants = min(max(conversation.remoteParticipantIdentifiers.count + 1, 2), 8)
        let isDM = conversation.remoteParticipantIdentifiers.count <= 1

        // The chat-scoping security fix: Apple documents `localParticipantIdentifier`
        // as a UUID unique to THIS device within THIS conversation (the same person
        // gets a different identifier in a different conversation), so it is the
        // right key to scope every MessageGameStore lookup by. Without it, `games()`
        // was device-wide — opening the extension in chat B with no bubble selected
        // could reopen chat A's newest game, resolve `.known` off its cached seat,
        // and stage chat A's deal-seed-bearing payload into chat B (see
        // MessageGameStore's type doc for the full bug).
        let chatKey = conversation.localParticipantIdentifier.uuidString

        let root = MessagesRootView(
            payloadURL: startingNewGame ? nil : selected?.url,
            style: style == .compact ? .compact : .expanded,   // map onto FoolishKit's enum
            senderIsLocal: senderIsLocal,
            startNewGame: startingNewGame,
            newGameToken: newGameToken,
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
        // The board the seal produced, spectator view (no hand) — bubble-safe.
        let publicView = await MessageKernel.shared.residentView(viewer: -1)
        // Re-decode (idempotent — re-adopts the same state) for the joins/summary.
        let env = try? await MessageEnvelope.decode(payload: payload, viewer: -1)
        let names = Dictionary((env?.joins ?? []).map { ($0.seat, $0.name) },
                               uniquingKeysWith: { a, _ in a })
        let image = publicView.flatMap { BubbleSnapshot.render(publicView: $0, names: names) }

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

        pendingStage = (payload, mySeat)
        conversation.insert(msg) { _ in }

        // Drop the user straight at Messages' Send (§11.4): the expanded board has
        // no send control of its own — Send lives in the compose area — so once a
        // move is staged, collapse to compact instead of making them drag down. To
        // add more cards (throw-ins, a second cover) they just re-open the game;
        // the staged chain survives the style change (GameSurface @State). Hold so
        // the move's animation plays out (card flight; a bout-ending good adds a
        // discard + draws), then a ~500ms rest so the result reads — not a flicker.
        try? await Task.sleep(nanoseconds: 900_000_000)
        requestPresentationStyle(.compact)
    }

    /// Commit a sent chain to the App Group cache (§6.1/§7.6): our seat becomes
    /// durable and this chain is the preferred one for the game. `chatKey` comes
    /// from `didStartSending`'s own conversation, not a stored property, because
    /// it must be the SAME conversation the bubble was staged/sent into (the
    /// whole point of the chat-scoping fix).
    private func commitPendingStage(chatKey: String) {
        guard let (payload, mySeat) = pendingStage else { return }
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
