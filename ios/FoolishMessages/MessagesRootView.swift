// MessagesRootView — what the extension shows, per presentation style.
//
// Compact is the KEYBOARD AREA (§3.5): no text field is possible there (there is
// no keyboard to type into) and no horizontal scrollers. So compact is a list
// and a button; anything needing typing — the nickname on join — must
// requestPresentationStyle(.expanded) first. That constraint is why this split
// exists at all, rather than one adaptive view.
//
// M1 scope: the shell, and proof that the extension can read a payload through
// the SAME kernel the app and the website use. The drawer's game list and the
// expanded table are M2 (docs/imessage-layout.html M3/M4).
import SwiftUI
import Messages
import FoolishKit

struct MessagesRootView: View {
    let payloadURL: URL?
    let style: MSMessagesAppPresentationStyle
    let requestExpand: () -> Void

    var body: some View {
        switch style {
        case .compact:
            CompactView(hasGame: payloadURL != nil, requestExpand: requestExpand)
        default:
            ExpandedView(payloadURL: payloadURL)
        }
    }
}

private struct CompactView: View {
    let hasGame: Bool
    let requestExpand: () -> Void

    var body: some View {
        VStack(spacing: 10) {
            Text(hasGame ? "Foolish — a game in this thread" : "Foolish")
                .font(.headline)
            Button(hasGame ? "Open the game" : "New game", action: requestExpand)
                .buttonStyle(.borderedProminent)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct ExpandedView: View {
    let payloadURL: URL?
    @State private var board: GameView?
    @State private var names: [Int: String] = [:]
    @State private var message: String?

    var body: some View {
        Group {
            if let board {
                MessageBoardView(view: board, names: names)
            } else if let message {
                VStack(spacing: 8) {
                    Text("Foolish").font(.headline)
                    // One sentence. Never a partial recovery, never a stack trace.
                    Text(message).font(.footnote).multilineTextAlignment(.center).padding(.horizontal)
                }
            } else {
                ProgressView()
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .task { await load() }
    }

    private func load() async {
        guard let url = payloadURL else { message = "No game selected."; return }
        do {
            // The whole game, out of a URL, through the C kernel — the same
            // msg_wire.c the website runs in wasm. Decoding VALIDATES and ADOPTS:
            // a hand-edited link throws rather than half-loading (§7.3), and the
            // resident game is now this payload's. We render the PUBLIC board
            // (viewer -1) — the seat-identity layer that unmasks a hand is M3.
            let env = try await MessageEnvelope.decode(url: url, viewer: -1)
            names = Dictionary(env.joins.map { ($0.seat, $0.name) }, uniquingKeysWith: { a, _ in a })
            guard let view = await MessageKernel.shared.residentView(viewer: -1) else {
                message = "This game link is damaged."; return
            }
            board = view
        } catch {
            message = "This game link is damaged."
        }
    }
}
