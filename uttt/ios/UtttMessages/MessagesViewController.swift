import Messages
import SwiftUI
import UtttKit

/// The extension. It owns the conversation and nothing else - every rule and
/// every coordinate is the kernel's, and the screens are UtttKit's.
final class MessagesViewController: MSMessagesAppViewController {

    private var host: UIHostingController<AnyView>?

    override func willBecomeActive(with conversation: MSConversation) {
        super.willBecomeActive(with: conversation)
        present(conversation)
    }

    override func didSelect(_ message: MSMessage, conversation: MSConversation) {
        present(conversation)
    }

    private func present(_ conversation: MSConversation) {
        host?.willMove(toParent: nil)
        host?.view.removeFromSuperview()
        host?.removeFromParent()

        /* THE SEED IS THE FIRST BOARD'S TIMESTAMP, and nothing else.
         * Not the nicknames: a nickname is under its owner's control, so
         * anybody who dislikes what the seed gave them could rename and
         * reload. The creator picks the moment, and at that moment they do
         * not yet know who they are playing. */
        let (seed, code) = Self.read(conversation.selectedMessage)
        let model: UtttModel
        if let code, !code.isEmpty {
            Uttt.newGame(seed: seed)
            Uttt.load(code, seed: seed)
            model = UtttModel(seed: seed, you: .x, solo: false)
        } else {
            model = UtttModel(seed: seed, you: .x, solo: true)
        }

        let vc = UIHostingController(rootView: AnyView(UtttGameScreen(model: model)))
        addChild(vc)
        vc.view.frame = view.bounds
        vc.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        vc.view.backgroundColor = .clear
        view.addSubview(vc.view)
        vc.didMove(toParent: self)
        host = vc
    }

    /// The body is the whole game; there is nothing else to carry.
    private static func read(_ message: MSMessage?) -> (Int32, Data?) {
        guard let url = message?.url,
              let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?
                  .queryItems else {
            return (Int32(truncatingIfNeeded: Int(Date().timeIntervalSince1970) / 60), nil)
        }
        let seed = items.first { $0.name == "s" }?.value.flatMap { Int32($0) } ?? 1
        let code = items.first { $0.name == "g" }?.value
            .flatMap { Data(base64Encoded: $0) }
        return (seed, code)
    }
}
