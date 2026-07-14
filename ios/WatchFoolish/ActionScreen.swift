// ActionScreen.swift — play (docs/WATCHOS_APP_PLAN.md §5.2). A 4-column token
// grid of your hand (Crown scrolls), tap toggles selection, and the bottom
// pill(s) are the kernel's legal menu for the current selection — nothing else.
// Single pill in the common case; the real Durak Cover/Pass fork renders two.

import SwiftUI
import WatchKit

struct ActionScreen: View {
    @ObservedObject var game: MockGame

    private let columns = Array(repeating: GridItem(.flexible(), spacing: 6), count: 4)

    var body: some View {
        VStack(spacing: 4) {
            ScrollView {
                LazyVGrid(columns: columns, spacing: 8) {
                    ForEach(game.hand) { card in
                        TokenCard(card: card, size: 22,
                                  selected: game.selected == card,
                                  trump: card.suit == game.trumpSuit)
                            .onTapGesture { game.toggle(card) }
                    }
                }
                .padding(.horizontal, 4)
                .padding(.top, 2)
            }
            pillBar
        }
        .background(WColor.bg)
    }

    // MARK: the kernel pill bar

    private var pillBar: some View {
        HStack(spacing: 6) {
            ForEach(Array(game.legalMoves.enumerated()), id: \.offset) { _, move in
                Button {
                    game.play(move)
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
        // Cover is the "good" default in the fork; Pass is secondary.
        switch move { case .pass: return false; default: return true }
    }
}

/// A wooden-free watch pill: brass for the primary action, outline for secondary.
struct PillStyle: ButtonStyle {
    let primary: Bool
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .foregroundStyle(primary ? WColor.bg : WColor.ink)
            .background(
                Capsule().fill(primary ? WColor.brass : Color(white: 0.14))
            )
            .overlay(Capsule().strokeBorder(primary ? .clear : WColor.dim, lineWidth: 1))
            .opacity(configuration.isPressed ? 0.7 : 1)
    }
}
