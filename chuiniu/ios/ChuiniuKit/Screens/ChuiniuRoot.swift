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
    /// The newest round whose throw this phone has played to its end, so a
    /// screen change does not throw it again (the stage's roll).
    @Published public var playedRoll = 0
    /// A touch left a bubble to stage, captioned with the kernel's caption;
    /// `collapse` for a move (the drawer goes down once it has rested), not
    /// for a lobby bubble.
    public var onStage: ((_ caption: String, _ collapse: Bool) -> Void)?

    public init(kernel: Kernel) {
        self.kernel = kernel
        refresh()
    }

    public func word(_ w: Word) -> String { kernel.word(w) }

    /// How far the kernel's newest plan has run, ms (the clock the stage and
    /// the beats share, I21); nil when no plan is playing.
    public var planMs: Int? { kernel.motionStart.map { max(0, Int(Date().timeIntervalSince($0) * 1000)) } }

    /// Read the resident again.
    public func refresh() { table = kernel.table }

    /// One kernel call; when it staged, hand the bubble's caption to the
    /// conversation.
    private func act(_ staged: Bool, collapse: Bool) {
        refresh()
        if staged { onStage?(table.bubbleCaption, collapse) }
    }

    public func join(name: String) { act(kernel.join(name: name), collapse: false) }
    public func start() { act(kernel.start(), collapse: false) }
    public func raise(_ bid: Bid) { act(kernel.raise(quantity: bid.quantity, face: bid.face), collapse: true) }
    public func call() { act(kernel.call(), collapse: true) }
    public func nextRound() { act(kernel.nextRound(), collapse: false) }

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
                UnreadableScreen(title: host.word(.gameTitle), reason: host.kernel.errorText(e))
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
#if DEBUG
        .task {
            // StageHarness: a table of any size, once per extension process,
            // after the conversation's own first read has landed
            guard !Harness.ran, let spec = Harness.spec else { return }
            try? await Task.sleep(nanoseconds: 800_000_000)
            Harness.ran = true
            if Harness.play(spec) { host.unreadable = nil; host.refresh() }
        }
#endif
    }
}

/// A link the kernel refused, in the kernel's words for why.
public struct UnreadableScreen: View {
    let title: String
    let reason: String

    public var body: some View {
        VStack(spacing: 8) {
            Text(title).font(.system(size: 17, weight: .heavy)).onFeltText()
            Text(reason).font(.system(size: 13, weight: .semibold)).onFeltText(FColor.textDim)
            Spacer(minLength: 0)
        }
        .padding(22)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(FeltBackground())
    }
}
