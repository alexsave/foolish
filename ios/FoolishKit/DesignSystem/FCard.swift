// FCard.swift — a single card: face or back, selectable, disabled, trump-badged
// (§5.4). Face is a bone rectangle with condensed-free type (cards use plain SF
// pips, not the numeral face) plus corner rank/suit; back is the procedural fern
// (FernCardBack). VoiceOver reads "seven of spades" etc. (§5.4 a11y floor).

import SwiftUI

public struct FCard: View {
    public let card: Card?          // nil ⇒ face-down (use `backSeed`)
    public var selected: Bool
    public var disabled: Bool       // dimmed + locked (the C1 in-flight affordance)
    public var trump: Bool          // draw the trump badge
    public var backSeed: UInt64
    public var size: CGSize

    public init(card: Card?, selected: Bool = false, disabled: Bool = false,
                trump: Bool = false, backSeed: UInt64 = 1,
                size: CGSize = CGSize(width: 62, height: 88)) {
        self.card = card
        self.selected = selected
        self.disabled = disabled
        self.trump = trump
        self.backSeed = backSeed
        self.size = size
    }

    public var body: some View {
        Group {
            if let card, !card.isHidden {
                face(card)
            } else {
                back
            }
        }
        .frame(width: size.width, height: size.height)
        .overlay(selectionRing)
        .opacity(disabled ? 0.55 : 1)
        .offset(y: selected ? -14 : 0)          // lift on select — the ONE spring animates this
        .accessibilityElement()
        .accessibilityLabel(a11yLabel)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }

    // MARK: face

    private func face(_ card: Card) -> some View {
        let suit = card.suit ?? .spades
        let color = FColor.suitColor(suit)
        return RoundedRectangle(cornerRadius: FRadius.card)
            .fill(FColor.card)
            .overlay(alignment: .topLeading) { corner(card, suit: suit, color: color) }
            .overlay(alignment: .bottomTrailing) {
                corner(card, suit: suit, color: color).rotationEffect(.degrees(180))
            }
            .overlay {
                Text(suit.glyph)
                    .font(.system(size: size.width * 0.42))
                    .foregroundColor(color)
            }
    }

    private func corner(_ card: Card, suit: Suit, color: Color) -> some View {
        VStack(spacing: 0) {
            Text(CardRank.label(card.v))
                .font(.system(size: size.width * 0.24, weight: .semibold))
            Text(suit.glyph)
                .font(.system(size: size.width * 0.18))
        }
        .foregroundColor(color)
        .padding(size.width * 0.08)
    }

    // MARK: back

    private var back: some View {
        // The shared procedural fern back (gold/red/amber on black), matching the
        // web card backs. One fern per install, drawn by TextureStore.
        FernBack(cornerRadius: FRadius.card)
    }

    private var selectionRing: some View {
        RoundedRectangle(cornerRadius: FRadius.card)
            .strokeBorder(selected ? FColor.win : Color.clear, lineWidth: 2.5)
    }

    private var a11yLabel: String {
        guard let card, !card.isHidden, let suit = card.suit else { return "face down card" }
        let rank: String
        switch card.v {
        case 13: rank = "ace"; case 12: rank = "king"; case 11: rank = "queen"
        case 10: rank = "ten"; default: rank = "\(card.v)"
        }
        let suitName = ["spades", "hearts", "clubs", "diamonds"][suit.rawValue]
        return "\(rank) of \(suitName)" + (trump ? ", trump" : "")
    }
}
