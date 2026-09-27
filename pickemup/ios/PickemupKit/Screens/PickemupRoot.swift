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

    public init() {
        // channel A of the start bubble: the starter's own deal, live
        model.onDealt = { [weak self] in
            guard let self else { return }
            self.showResident()
            self.model.player.play(Pk.beats(from: -1, to: 0, open: false))
        }
    }

    /// Whether the library and the generated readers are one layout (D46).
    /// Replaceable by a test; nothing else sets it.
    public var layoutMatches: () -> Bool = { Pk.layoutMatches }

    /// THE ONE GATE ON A MISMATCHED PAIR (I22, I34): every read of the
    /// resident goes through `showResident` or `adopt`, and both refuse here,
    /// so a stale xcframework or a stale Generated/ shows the unreadable
    /// screen and nothing is read at a wrong offset.
    public var readable: Bool {
        guard layoutMatches() else {
            screen = .unreadable(PK_EFORMAT)
            return false
        }
        return true
    }

    /// The screen for whatever is resident now.
    public func showResident() {
        guard readable else { return }
        model.refresh()
        screen = model.table?.readable == 1
            ? (model.phase == PK_PHASE_WAITING ? .lobby : .table)
            : .blank
    }

    /// ADOPT `text` and play what it brings (the motion grid's channels C, D
    /// and E; a lost race is the Conflict row). `arrival`: it landed while the
    /// board was up. 0, or the negative PK_E* the read refused with, and then
    /// nothing changed.
    ///
    /// WHICH EVENTS PLAY is the kernel's (pk_api_adopt, I29): it compares the
    /// chain on screen with the one adopted, in the same call that adopts, so
    /// nothing here holds the resident across the read.
    @discardableResult
    public func adopt(_ text: String, arrival: Bool) -> Int {
        guard readable else { return PK_EFORMAT }
        let e = Pk.adopt(text, arrival: arrival)
        guard e == 0 else { return e }
        showResident()
        model.player.play(Pk.beatsNow())
        return 0
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
