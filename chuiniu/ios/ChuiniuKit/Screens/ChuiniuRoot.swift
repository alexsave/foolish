// ChuiniuRoot.swift - the one SwiftUI root the extension hosts, the host
// object that owns the kernel seam, and which screen shows: the model's
// phase, and nothing Swift works out.

import SwiftUI

@MainActor
public final class ChuiniuHost: ObservableObject {
    public let kernel: Kernel
    /// The resident game as last read from the kernel.
    @Published public private(set) var table: TableModel = .empty
    /// A link the kernel refused, with its error; the unreadable screen.
    @Published public var unreadable: Int?
    /// The newest roll this phone has played, so a screen change does not
    /// replay it (DiceRoll).
    @Published public var playedRoll = 0
    /// A touch left a bubble to stage, captioned with the kernel's caption.
    public var onStage: ((String) -> Void)?

    public init(kernel: Kernel) {
        self.kernel = kernel
        refresh()
    }

    public func word(_ w: Word) -> String { kernel.word(w) }

    /// Read the resident again.
    public func refresh() { table = kernel.table }

    /// One kernel call; when it staged, hand the caption to the conversation.
    private func act(_ staged: Bool) {
        refresh()
        if staged { onStage?(table.caption) }
    }

    public func join(name: String) { act(kernel.join(name: name)) }
    public func start() { act(kernel.start()) }
    public func raise(_ bid: Bid) { act(kernel.raise(quantity: bid.quantity, face: bid.face)) }
    public func call() { act(kernel.call()) }
    public func nextRound() { act(kernel.nextRound()) }

    /// Adopt a bubble's link: 0, or the kernel's error (and the unreadable
    /// screen).
    @discardableResult
    public func adopt(_ url: URL) -> Int {
        let e = kernel.adoptBubble(url)
        unreadable = e == 0 ? nil : e
        refresh()
        return e
    }
}

public struct ChuiniuRoot: View {
    @ObservedObject var host: ChuiniuHost

    public init(host: ChuiniuHost) { self.host = host }

    public var body: some View {
        Group {
            if let e = host.unreadable {
                UnreadableScreen(code: e, title: host.word(.gameTitle))
            } else {
                switch host.table.phase {
                case .lobby:
                    LobbyScreen(host: host)
                case .bidding:
                    TableScreen(host: host)
                case .revealed, .over:
                    RevealScreen(host: host)
                }
            }
        }
        .animation(FMotion.chrome, value: host.table.phase)
    }
}

/// A link the kernel refused. The error's words are the tie-together's
/// (the kernel's error table); the scaffold shows the code.
public struct UnreadableScreen: View {
    let code: Int
    let title: String

    public var body: some View {
        VStack(spacing: 8) {
            Text(title).font(.system(size: 17, weight: .heavy)).onFeltText()
            Text(verbatim: "\(code)").font(.system(size: 13, weight: .semibold)).onFeltText(FColor.textDim)
            Spacer(minLength: 0)
        }
        .padding(22)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(FeltBackground())
    }
}
