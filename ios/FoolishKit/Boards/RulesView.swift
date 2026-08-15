// RulesView.swift — the scrollable "How to play" page (1.0(4)), opened from the
// board's Help (?) square. A decently long walk through Durak with small visual
// examples that REUSE the real FCard, so the cards a player sees on the board are
// the same cards they learn the rules from (never a second, drifting card art).
//
// Text is localized (ios.rules.*). The examples are static illustrations, not a
// live game — FCard renders them straight, no kernel involved.

import SwiftUI

public struct RulesView: View {
    private let onClose: () -> Void
    public init(onClose: @escaping () -> Void = {}) { self.onClose = onClose }

    // A trump for the illustrations (hearts here — only for the page's examples).
    private static let trump: Suit = .hearts

    public var body: some View {
        ZStack {
            WoolBackground().ignoresSafeArea()
            VStack(spacing: 0) {
                header
                ScrollView {
                    VStack(alignment: .leading, spacing: FSpace.xl) {
                        section("ios.rules.goal.h", "ios.rules.goal.b")

                        section("ios.rules.deck.h", "ios.rules.deck.b") {
                            cardRow([Card(s: 1, v: 1), Card(s: 1, v: 5), Card(s: 1, v: 13)], trumpSuit: Self.trump)
                        }

                        section("ios.rules.attack.h", "ios.rules.attack.b") {
                            cardRow([Card(s: 0, v: 6)])
                        }

                        section("ios.rules.defend.h", "ios.rules.defend.b") {
                            battleRow(attack: Card(s: 0, v: 6), defense: Card(s: 0, v: 10))
                            battleRow(attack: Card(s: 0, v: 6), defense: Card(s: 1, v: 1), note: "ios.rules.defend.trump")
                        }

                        section("ios.rules.throw.h", "ios.rules.throw.b") {
                            cardRow([Card(s: 0, v: 6), Card(s: 2, v: 6), Card(s: 3, v: 6)])
                        }

                        section("ios.rules.takegood.h", "ios.rules.takegood.b")

                        section("ios.rules.pass.h", "ios.rules.pass.b") {
                            cardRow([Card(s: 0, v: 6), Card(s: 3, v: 6)])
                        }

                        section("ios.rules.win.h", "ios.rules.win.b")
                    }
                    .padding(FSpace.xl)
                    .padding(.bottom, FSpace.xxl)
                }
            }
        }
    }

    private var header: some View {
        HStack {
            Text(FStrings.t("ios.rules.title"))
                .font(FType.title(22)).onWoolText()
            Spacer()
            Button(action: onClose) {
                Text(FStrings.t("ios.done")).font(FType.title(16)).onWoolText()
            }
        }
        .padding(.horizontal, FSpace.xl)
        .padding(.vertical, FSpace.l)
    }

    // MARK: sections

    @ViewBuilder
    private func section(_ headKey: String, _ bodyKey: String,
                         @ViewBuilder visual: () -> some View = { EmptyView() }) -> some View {
        VStack(alignment: .leading, spacing: FSpace.s) {
            Text(FStrings.t(headKey)).font(FType.title(18)).onWoolText()
            Text(FStrings.t(bodyKey)).font(FType.body(15)).onWoolText()
                .fixedSize(horizontal: false, vertical: true)
            visual()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    // MARK: card illustrations (reusing FCard)

    private static let exSize = CGSize(width: 46, height: 64)

    private func cardRow(_ cards: [Card], trumpSuit: Suit? = nil) -> some View {
        HStack(spacing: FSpace.s) {
            ForEach(Array(cards.enumerated()), id: \.offset) { _, c in
                FCard(card: c, trump: trumpSuit != nil && c.suit == trumpSuit, size: Self.exSize)
            }
        }
        .padding(.top, FSpace.xs)
    }

    /// An attack card with a defence card laid over it, the way a covered battle
    /// reads on the board (defence offset down-right).
    private func battleRow(attack: Card, defense: Card, note: String? = nil) -> some View {
        HStack(spacing: FSpace.l) {
            ZStack {
                FCard(card: attack, size: Self.exSize)
                FCard(card: defense, trump: defense.suit == Self.trump, size: Self.exSize)
                    .offset(x: 12, y: 14)
                    .rotationEffect(.degrees(6))
            }
            .frame(width: Self.exSize.width + 12, height: Self.exSize.height + 14, alignment: .topLeading)
            if let note {
                Text(FStrings.t(note)).font(FType.body(14)).onWoolText()
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.top, FSpace.xs)
    }
}
