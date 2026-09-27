// TallybonesRoot.swift - the one SwiftUI root the extension hosts, and which
// screen it shows (pickemup's PickemupRoot). The controller decides the
// screen from the kernel's view and says so; this only draws it.

import SwiftUI
import UIKit

@MainActor
public final class TallybonesHost: ObservableObject {
    public enum Screen: Equatable {
        case blank
        case nameGate
        case lobby
        case table
        case unreadable(Int)
    }

    @Published public var screen: Screen = .blank
    public let kernel: TallyKernel
    public let model: TallyTable
    /// The name gate is done: make the new game.
    public var onNamed: (() -> Void)?

    /// The kernel behind the seam: the bridge (tb_api.h), always.
    public init(kernel: TallyKernel? = nil) {
        let k = kernel ?? BridgeKernel()
        self.kernel = k
        self.model = TallyTable(kernel: k)
        model.onScreen = { [weak self] in self?.pickScreen() }
    }

    /// THE ONE GATE ON A MISMATCHED PAIR (pickemup I22, I34): a stale
    /// xcframework or a stale Generated/ shows the unreadable screen and
    /// nothing is read at a wrong offset.
    public var readable: Bool {
        guard kernel.layoutMatches else {
            screen = .unreadable(tallyErrorFormat)
            return false
        }
        return true
    }

    /// The screen for whatever is resident now.
    public func showResident(animate: Bool = false) {
        guard readable else { return }
        model.refresh(animate: animate)
        pickScreen()
    }

    private func pickScreen() {
        let v = model.view
        screen = v.readable ? (v.phase == .lobby ? .lobby : .table) : .blank
    }

    /// ADOPT `text` and show it: 0, or the negative error the read refused
    /// with, and then nothing changed.
    @discardableResult
    public func adopt(_ text: String, arrival: Bool) -> Int {
        guard readable else { return tallyErrorFormat }
        let e = kernel.adopt(text, arrival: arrival)
        guard e == 0 else { return e }
        showResident(animate: true)
        return 0
    }

    /// The bubble picture for the resident, as it is now.
    public func bubbleImage(scheme: ColorScheme) -> UIImage? {
        BubbleSnapshot.render(BubbleContent.of(kernel.view(), title: kernel.string(.lobbyTitle)), scheme: scheme)
    }
}

public struct TallybonesRoot: View {
    @ObservedObject var host: TallybonesHost

    public init(host: TallybonesHost) { self.host = host }

    public var body: some View {
        switch host.screen {
        case .blank:
            FeltBackground()
        case .nameGate:
            NameGateScreen(kernel: host.kernel) { host.onNamed?() }
        case .lobby:
            LobbyScreen(model: host.model)
        case .table:
            TableScreen(model: host.model)
        case .unreadable(let code):
            UnreadableScreen(kernel: host.kernel, code: code)
        }
    }
}
