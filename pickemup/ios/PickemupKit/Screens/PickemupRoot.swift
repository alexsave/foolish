// PickemupRoot.swift - the one SwiftUI root the extension hosts, and which
// screen it shows. The controller decides the screen from the kernel's frame
// (phase, my seat) and says so; this only draws it.

import SwiftUI

@MainActor
public final class PickemupHost: ObservableObject {
    public enum Screen: Equatable {
        case blank
        case nameGate
        case lobby
        case table
        case unreadable(Int)
    }

    @Published public var screen: Screen = .blank
    @Published public var rulesShown = false
    public let model = TableModel()
    /// The name gate is done: make the new game.
    public var onNamed: (() -> Void)?

    public init() {}

    /// The screen for whatever is resident now.
    public func showResident() {
        model.refresh()
        screen = model.table?.readable == 1
            ? (model.phase == PK_PHASE_WAITING ? .lobby : .table)
            : .blank
    }
}

public struct PickemupRoot: View {
    @ObservedObject var host: PickemupHost

    public init(host: PickemupHost) { self.host = host }

    public var body: some View {
        Group {
            switch host.screen {
            case .blank:
                FeltBackground()
            case .nameGate:
                NameGateScreen { host.onNamed?() }
            case .lobby:
                LobbyScreen(model: host.model)
            case .table:
                TableScreen(model: host.model, onRules: { host.rulesShown = true })
            case .unreadable(let code):
                UnreadableScreen(code: code)
            }
        }
        .onPreferenceChange(PkAnchorKey.self) { PkAnchors.latest = $0 }
        .sheet(isPresented: $host.rulesShown) { RulesSheet() }
    }
}

/// The newest frames of every named anchor, for the flight layer and the
/// rig (Anchors.swift). Written by the root on every layout.
@MainActor
public enum PkAnchors {
    public static var latest: [String: CGRect] = [:]
}
