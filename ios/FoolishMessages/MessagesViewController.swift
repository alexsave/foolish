// MessagesViewController — the iMessage extension's entry point (design §11).
//
// Messages instantiates this class (Info.plist NSExtensionPrincipalClass) and
// drives it through the lifecycle below. What it renders comes from FoolishKit:
// the extension ships inside the host app and reuses the app's engine, design
// system and board components rather than growing a second copy of any of them.
//
// THE RULE, restated because this is where it is most tempting to break: no
// Durak rule is answered here. Whose move, whether a move is legal, which of two
// chains wins, whether a staged move survives a rebase — all of it is C
// (msg_wire.c, reached through EngineC). This file marshals and renders.
// A hand-rolled "is it my turn" boolean is a bug by policy (§17.16).
//
// M1 scope: the target exists, links FoolishKit, and stands up. The drawer and
// table (M2) and the send/receive wiring (M3) land on top of this.
import UIKit
import Messages
import SwiftUI
import FoolishKit

final class MessagesViewController: MSMessagesAppViewController {

    private var host: UIHostingController<MessagesRootView>?

    // MARK: - Lifecycle (§11.1)
    //
    // The callback table is exact and the order matters: willBecomeActive gives
    // us the conversation, and it is the ONLY moment we are handed the selected
    // message. There is no background execution and no push — didReceive fires
    // only while we are visible (§3.5) — so every adoption happens here.

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
        present(conversation)
    }

    override func didResignActive(with conversation: MSConversation) {
        super.didResignActive(with: conversation)
    }

    /// A message arrived while we are on screen. Rule P decides whether it is
    /// progress or a stale bubble — never delivery order (§7.2).
    override func didReceive(_ message: MSMessage, conversation: MSConversation) {
        present(conversation)
    }

    /// The user tapped Send on our staged bubble: our chain is now the thread's.
    override func didStartSending(_ message: MSMessage, conversation: MSConversation) {
        // M3: cache := our envelope; pending ledger := its unacked tail (§7.6).
    }

    /// The user deleted the staged bubble before sending: roll back to the
    /// pre-stage snapshot, or the ledger claims a move nobody will ever see.
    override func didCancelSending(_ message: MSMessage, conversation: MSConversation) {
        // M3: restore cache/ledger from the pre-stage snapshot (§17.2).
    }

    override func willTransition(to presentationStyle: MSMessagesAppPresentationStyle) {
        super.willTransition(to: presentationStyle)
    }

    // MARK: - Presentation

    private func present(_ conversation: MSConversation) {
        // The payload IS the game: a selected message's URL carries the whole
        // chain, so there is nothing to fetch and no row to look up.
        let payload = conversation.selectedMessage?.url
        let root = MessagesRootView(payloadURL: payload,
                                    style: presentationStyle,
                                    requestExpand: { [weak self] in
                                        self?.requestPresentationStyle(.expanded)
                                    })
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
