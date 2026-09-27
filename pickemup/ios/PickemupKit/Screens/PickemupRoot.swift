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

    /// The screen for whatever is resident now.
    public func showResident() {
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
    /// WHICH EVENTS PLAY is a comparison of two chains the kernel makes: the
    /// board on screen and the one adopted. Of the same game and further on,
    /// from the bubble on screen to the new tip; opened cold, the newest
    /// bubble only (and the deal for a start bubble); my staged play lost to
    /// another chain, the retraction and then the winner from the common
    /// prefix; anything else, no motion.
    @discardableResult
    public func adopt(_ text: String, arrival: Bool) -> Int {
        let prior = Pk.table()
        let priorLive = prior.map { $0.readable == 1 && $0.phase != PK_PHASE_WAITING } ?? false
        let priorText = prior?.readable == 1 ? Pk.text : nil
        let staged = model.stagedPlay
        let e = Pk.read(text)
        guard e == 0 else { return e }
        showResident()
        guard let now = model.table, now.phase != PK_PHASE_WAITING else {
            model.player.clear()
            return 0
        }
        let to = now.bubbles
        let same = priorText.map { Pk.sameGame($0, text) } ?? false
        if same, priorLive, let prior, let priorText {
            if let staged, prior.draft != 0 {
                let common = Pk.common(priorText, text)
                if common >= 0, common <= to, common < prior.bubbles + 1 {
                    model.player.play(Pk.beatsConflict(card: staged.card, pos: staged.pos, from: common, to: to))
                    return 0
                }
            }
            if to > prior.bubbles {
                model.player.play(Pk.beats(from: prior.bubbles, to: to, open: !arrival))
            } else {
                model.player.clear()
            }
            return 0
        }
        // cold, or the lobby this game was dealt from: the newest bubble, or
        // the deal when the start bubble is the newest (bubble 0, from -1)
        model.player.play(Pk.beats(from: to - 1, to: to, open: !arrival || !same))
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
