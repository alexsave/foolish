// ActionScreen.swift — play (docs/WATCHOS_APP_PLAN.md §5.2). A 4-column token
// grid of your hand; the CROWN moves the selection through the cards (it does
// not scroll), and a tap selects too. The bottom pill(s) are the kernel's legal
// menu for the current selection — nothing else. Index −1 = nothing selected
// (the defender's Pickup / the attacker's Done rest state).

import SwiftUI
import WatchKit

struct ActionScreen: View {
    @ObservedObject var game: MockGame

    @State private var crown: Double = -1
    private let columns = Array(repeating: GridItem(.flexible(), spacing: 6), count: 4)

    private var selIndex: Int { min(max(Int(crown.rounded()), -1), game.hand.count - 1) }

    var body: some View {
        VStack(spacing: 6) {
            LazyVGrid(columns: columns, spacing: 8) {
                ForEach(Array(game.hand.enumerated()), id: \.element) { idx, card in
                    TokenCard(card: card, size: 22, selected: idx == selIndex)
                        .onTapGesture { crown = (selIndex == idx) ? -1 : Double(idx) }
                }
            }
            .padding(.horizontal, 4)
            .padding(.top, 2)

            Spacer(minLength: 0)
            pillBar
        }
        .background(WColor.bg)
        .focusable()
        .digitalCrownRotation($crown, from: -1, through: Double(max(game.hand.count - 1, 0)),
                              by: 1, sensitivity: .low, isContinuous: false, isHapticFeedbackEnabled: true)
        .onChange(of: crown) { _ in syncSelection() }
        .onAppear { crown = game.selected.flatMap { s in game.hand.firstIndex(of: s) }.map(Double.init) ?? -1 }
    }

    private func syncSelection() {
        game.selected = selIndex >= 0 ? game.hand[selIndex] : nil
    }

    // MARK: the kernel pill bar

    private var pillBar: some View {
        HStack(spacing: 6) {
            ForEach(Array(game.legalMoves.enumerated()), id: \.offset) { _, move in
                Button {
                    game.play(move)
                    crown = -1
                    WKInterfaceDevice.current().play(.click)
                } label: {
                    Text(move.label)
                        .font(WFont.label(15))
                        .frame(maxWidth: .infinity, minHeight: 32)
                }
                .buttonStyle(PillStyle(primary: isPrimary(move)))
            }
        }
        .padding(.horizontal, 6)
        .padding(.bottom, 2)
    }

    private func isPrimary(_ move: WMove) -> Bool {
        switch move { case .pass: return false; default: return true }
    }
}

/// Brass for the primary action, dark outline for secondary.
struct PillStyle: ButtonStyle {
    let primary: Bool
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .foregroundStyle(primary ? WColor.bg : WColor.ink)
            .background(Capsule().fill(primary ? WColor.brass : Color(white: 0.14)))
            .overlay(Capsule().strokeBorder(primary ? .clear : WColor.dim, lineWidth: 1))
            .opacity(configuration.isPressed ? 0.7 : 1)
    }
}
