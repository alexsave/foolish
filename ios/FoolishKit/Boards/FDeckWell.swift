// FDeckWell.swift — the stock (§5.4): remaining deck count as a condensed
// numeral, with the flipped trump laid under a card back. When the stock is
// empty the well shows just the trump suit mark.

import SwiftUI

public struct FDeckWell: View {
    public let deckCount: Int
    public let flipped: Card?
    public let hasFlipped: Bool
    public let trumpSuit: Suit?
    public var backSeed: UInt64

    public init(deckCount: Int, flipped: Card?, hasFlipped: Bool, trumpSuit: Suit?, backSeed: UInt64 = 7) {
        self.deckCount = deckCount
        self.flipped = flipped
        self.hasFlipped = hasFlipped
        self.trumpSuit = trumpSuit
        self.backSeed = backSeed
    }

    public var body: some View {
        VStack(spacing: FSpace.s) {
            ZStack {
                // The flipped trump lies across the bottom of the deck.
                if hasFlipped, let flipped {
                    FCard(card: flipped, trump: true, size: CGSize(width: 46, height: 66))
                        .rotationEffect(.degrees(90))
                        .offset(x: 18)
                }
                if deckCount > 0 {
                    FCard(card: nil, backSeed: backSeed, size: CGSize(width: 46, height: 66))
                }
            }
            .frame(width: 84, height: 70)

            HStack(spacing: FSpace.xs) {
                Text("\(deckCount)")
                    .font(FType.numeral(26))
                    .foregroundColor(FColor.textPrimary)
                if let trumpSuit {
                    Text(trumpSuit.glyph)
                        .font(.system(size: 16))
                        .foregroundColor(FColor.suitColor(trumpSuit))
                }
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(deckCount) cards left in the deck" +
            (trumpSuit != nil ? ", trump \(["spades","hearts","clubs","diamonds"][trumpSuit!.rawValue])" : ""))
    }
}
