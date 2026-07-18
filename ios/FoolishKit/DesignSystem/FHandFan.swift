// FHandFan.swift — the local player's hand, matching the WEB client's self-hand
// (ActionButtons.tsx CardDiv): a FLAT ROW, not a fan. Cards share the width,
// clamped 20–50pt wide at a fixed 70pt tall, with a small gap and NO overlap — so
// every card is directly tappable (the old overlapping fan let only the top-most
// rightmost card take a tap). Tapping toggles multi-selection; the board turns a
// selection into a move (drag / buttons / tap-a-battle), never this view.

import SwiftUI

public struct FHandFan: View {
    public let cards: [Card]
    public let trumpSuit: Suit?
    /// Cards currently locked (in-flight / illegal in this context) — dimmed.
    public let disabled: Set<String>
    @Binding public var selection: Set<String>
    /// Tap a card — the board toggles it in the selection.
    public let onTap: (Card) -> Void

    public init(cards: [Card], trumpSuit: Suit?, disabled: Set<String> = [],
                selection: Binding<Set<String>>, onTap: @escaping (Card) -> Void) {
        self.cards = cards
        self.trumpSuit = trumpSuit
        self.disabled = disabled
        self._selection = selection
        self.onTap = onTap
    }

    private let cardH: CGFloat = 70          // web: fixed 70pt tall
    private let gap: CGFloat = 4

    public var body: some View {
        GeometryReader { geo in
            let count = max(cards.count, 1)
            // Flex-share the width, clamped 20–50pt (web: flex 1, minWidth 20,
            // maxWidth 50). Many cards squish toward 20pt and hit FCard's thin
            // layout; a few sit at 50pt, centered.
            let avail = geo.size.width - gap * CGFloat(count + 1)
            let cardW = min(50, max(20, avail / CGFloat(count)))
            HStack(spacing: gap) {
                ForEach(cards, id: \.identity) { card in
                    FCard(card: card,
                          selected: selection.contains(card.identity),
                          disabled: disabled.contains(card.identity),
                          trump: trumpSuit != nil && card.suit == trumpSuit,
                          size: CGSize(width: cardW, height: cardH))
                        .contentShape(Rectangle())
                        .onTapGesture {
                            guard !disabled.contains(card.identity) else { Haptics.fire(.reject); return }
                            Haptics.fire(.pickUp)
                            onTap(card)
                        }
                }
            }
            .frame(width: geo.size.width, height: geo.size.height, alignment: .center)
        }
        .frame(height: cardH + 8)
    }
}
