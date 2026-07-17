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
    /// The payload we last staged (via `insert`), awaiting the human's send/cancel
    /// (§7.6). Committed to the cache on didStartSending, dropped on cancel.
    private var pendingStage: (payload: Data, mySeat: Int)?

    // MARK: - Lifecycle (§11.1)

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
        present(conversation)
    }

    /// A message arrived while we are on screen — an opponent may be live-playing.
    /// Rule P decides progress vs stale (in MessageTurnController), never delivery
    /// order (§7.2). A fresh receive cancels any half-started New game.
    override func didReceive(_ message: MSMessage, conversation: MSConversation) {
        startingNewGame = false
        present(conversation)
    }

    /// The user tapped Send on our staged bubble: our chain is now the thread's,
    /// so commit it to the cache (§7.6). This is the ONLY place the cache learns
    /// a chain was actually sent — insert alone is not a commit.
    override func didStartSending(_ message: MSMessage, conversation: MSConversation) {
        startingNewGame = false
        commitPendingStage()
    }

    /// The user deleted the staged bubble before sending: drop the pending record
    /// so the cache never claims a chain nobody will see (§17.2). Nothing was
    /// written yet, so there is nothing to roll back — just forget it.
    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
        pendingStage = nil
    }

    override func willTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.willTransition(to: presentationStyle)
        if let c = activeConversation { present(c) }
    }

    // MARK: - Presentation

    private func present(_ conversation: MSConversation) {
        let selected = conversation.selectedMessage
        // §6.2 S1's exact half: did THIS device send the tapped bubble? Only the
        // extension can answer — the participant UUIDs never travel in the payload.
        let senderIsLocal = selected?.senderParticipantIdentifier != nil
            && selected?.senderParticipantIdentifier == conversation.localParticipantIdentifier

        let root = MessagesRootView(
            payloadURL: startingNewGame ? nil : selected?.url,
            style: presentationStyle,
            senderIsLocal: senderIsLocal,
            startNewGame: startingNewGame,
            requestExpand: { [weak self] in self?.requestPresentationStyle(.expanded) },
            onNewGame: { [weak self] in
                self?.startingNewGame = true
                self?.requestPresentationStyle(.expanded)
            },
            onSend: { [weak self] payload, mySeat in
                await self?.stage(payload: payload, mySeat: mySeat)
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

        let msg = MessageComposer.message(
            payload: payload,
            snapshot: image,
            caption: "Foolish",
            summary: FStrings.t("ios.msg.tap"),
            session: conversation.selectedMessage?.session)   // reuse ⇒ collapse old bubble

        pendingStage = (payload, mySeat)
        conversation.insert(msg) { _ in }
    }

    /// Commit a sent chain to the App Group cache (§6.1/§7.6): our seat becomes
    /// durable and this chain is the preferred one for the game.
    private func commitPendingStage() {
        guard let (payload, mySeat) = pendingStage else { return }
        pendingStage = nil
        Task {
            guard let env = try? await MessageEnvelope.decode(payload: payload, viewer: mySeat)
            else { return }
            let names = Dictionary(env.joins.map { ($0.seat, $0.name) },
                                   uniquingKeysWith: { a, _ in a })
            MessageGameStore.shared.put(MessageGameRecord(
                gameId: env.gameId, mySeat: mySeat, nPlayers: env.nPlayers, round: env.round,
                turn: env.turn, phase: env.phase, finished: env.phase == 3, names: names,
                payloadBase32: Base32.encode(payload),
                updatedAt: Date().timeIntervalSince1970))
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
