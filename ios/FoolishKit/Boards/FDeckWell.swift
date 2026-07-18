// FDeckWell.swift — the stock, matching the web's DeckAndFlipped
// (src/components/GameDisplay/DeckAndFlipped.tsx): a leaning stack of landscape
// card backs with the remaining count centred ON the pile, and the flipped
// trump hanging below it (upright, tucked under the stack). When the stock and
// the flipped card are gone, the bare trump-suit glyph takes their place.
//
// Web-layout parity per IOS_APP_DESIGN §17.10.

import SwiftUI

public struct FDeckWell: View {
    public let deckCount: Int
    public let flipped: Card?
    public let hasFlipped: Bool
    public let trumpSuit: Suit?
    public var backSeed: UInt64

    public init(deckCount: Int, flipped: Card?, hasFlipped: Bool, trumpSuit: Suit?, backSeed: UInt64 = 42) {
        self.deckCount = deckCount
        self.flipped = flipped
        self.hasFlipped = hasFlipped
        self.trumpSuit = trumpSuit
        self.backSeed = backSeed
    }

    // The badge counts the flipped card too (web badgeTotal = deck + flipped).
    private var badgeTotal: Int { deckCount + ((hasFlipped && flipped != nil) ? 1 : 0) }
    private var stackLayers: Int { min(max(deckCount, 0), 6) }

    public var body: some View {
        ZStack(alignment: .center) {
            // The flipped trump hangs below the stack, upright, behind it.
            if hasFlipped, let flipped {
                FCard(card: flipped, trump: true, size: CGSize(width: 46, height: 66))
                    .offset(y: 34)
                    .zIndex(0)
            }

            if deckCount > 0 {
                deckStack.zIndex(1)
            } else if !hasFlipped, let trumpSuit {
                // Stock and flip both gone — show the trump suit glyph.
                Text(trumpSuit.glyph)
                    .font(.system(size: 44))
                    .foregroundColor(FColor.suitColor(trumpSuit))
            }
        }
        .frame(width: 92, height: 108)
        // Publish the deck's rect so draw flights (deck→hand) have a source.
        .background(GeometryReader { g in
            Color.clear.preference(key: DeckFrameKey.self, value: g.frame(in: .named(boardSpace)))
        })
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(deckCount) cards left in the deck" +
            (trumpSuit != nil ? ", trump \(["spades","hearts","clubs","diamonds"][trumpSuit!.rawValue])" : ""))
    }

    private var deckStack: some View {
        ZStack {
            ForEach(0..<stackLayers, id: \.self) { i in
                FCard(card: nil, backSeed: backSeed, size: CGSize(width: 46, height: 66))
                    .rotationEffect(.degrees(90))
                    .offset(x: CGFloat(-i), y: CGFloat(-i * 2))
            }
            Text("\(badgeTotal)")
                .font(.system(size: 17, weight: .bold))
                .foregroundColor(.white)
                .shadow(color: .black.opacity(0.8), radius: 1, x: 1, y: 1)
                .offset(y: -CGFloat(stackLayers))
        }
        .frame(width: 78, height: 60)
    }
}
