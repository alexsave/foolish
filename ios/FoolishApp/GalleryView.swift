// GalleryView.swift — the DEBUG component gallery (§16.A6). Lists every
// DesignSystem / Boards component so §5 has a living reviewer-of-record. Reach
// it from Home only in DEBUG builds. Keep it current forever: a new component
// with no gallery entry is an incomplete component.

#if DEBUG
import SwiftUI
import FoolishKit

struct GalleryView: View {
    @State private var selection: Set<String> = ["0-7"]

    private let sampleHand: [Card] = [
        Card(s: 0, v: 7), Card(s: 1, v: 13), Card(s: 2, v: 6),
        Card(s: 3, v: 11), Card(s: 1, v: 9),
    ]
    private let sampleBattles: [BattleView] = [
        BattleView(attack: Card(s: 0, v: 7), defense: Card(s: 0, v: 9)),
        BattleView(attack: Card(s: 2, v: 11), defense: nil),
    ]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FSpace.xl) {
                section("FButton") {
                    FButton("Primary") {}
                    FButton("Secondary", kind: .secondary) {}
                }
                section("FCard") {
                    HStack(spacing: FSpace.m) {
                        FCard(card: Card(s: 1, v: 13))
                        FCard(card: Card(s: 0, v: 7), selected: true)
                        FCard(card: Card(s: 3, v: 10), trump: true)
                        FCard(card: Card(s: 2, v: 8), disabled: true)
                        FCard(card: nil, backSeed: 42)
                    }
                }
                section("FHandFan") {
                    FHandFan(cards: sampleHand, trumpSuit: .hearts,
                             selection: $selection, onTap: { _ in })
                        .frame(height: 120)
                }
                section("FBattleGrid") {
                    FBattleGrid(battles: sampleBattles, trumpSuit: .spades, coverable: [1])
                }
                section("FDeckWell") {
                    FDeckWell(deckCount: 18, flipped: Card(s: 1, v: 10),
                              hasFlipped: true, trumpSuit: .hearts)
                }
                section("FSeatBadge") {
                    HStack(spacing: FSpace.l) {
                        FSeatBadge(name: "Cordite", handCount: 6, isDefender: true)
                        FSeatBadge(name: "Espresso", handCount: 4, isAttacker: true, thinking: true)
                        FSeatBadge(name: "Random", handCount: 0, saidGood: true, isOut: true)
                    }
                }
                section("FActionBar") {
                    FActionBar(canPickup: true, canDone: true, canTransfer: true,
                               onPickup: {}, onDone: {}, onTransfer: {})
                }
                section("FToast") {
                    FToast("Nine of spades covered")
                    FToast("That move isn’t allowed.", accent: true)
                }
                section("Type ramp") {
                    Text("Compressed 48").font(FType.numeral(48)).foregroundColor(FColor.textPrimary)
                    Text("Title 22").font(FType.title(22)).foregroundColor(FColor.textPrimary)
                    Text("Body 15").font(FType.body(15)).foregroundColor(FColor.textDim)
                }
                section("Palette") {
                    HStack {
                        ForEach(["table", "surface", "card", "accent", "win"], id: \.self) { swatch($0) }
                    }
                }
            }
            .padding(FSpace.l)
        }
        .background(FColor.table.ignoresSafeArea())
        .preferredColorScheme(.dark)
    }

    private func section<C: View>(_ title: String, @ViewBuilder _ content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: FSpace.m) {
            Text(title).font(FType.title(15)).foregroundColor(FColor.accent)
            content()
        }
    }

    private func swatch(_ name: String) -> some View {
        let color: Color = {
            switch name {
            case "table": return FColor.table
            case "surface": return FColor.surface
            case "card": return FColor.card
            case "accent": return FColor.accent
            case "win": return FColor.win
            default: return .gray
            }
        }()
        return VStack(spacing: 2) {
            RoundedRectangle(cornerRadius: 6).fill(color).frame(width: 44, height: 44)
            Text(name).font(FType.body(10)).foregroundColor(FColor.textDim)
        }
    }
}

#Preview { GalleryView() }
#endif
