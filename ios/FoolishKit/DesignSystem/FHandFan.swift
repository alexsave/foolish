// FHandFan.swift — the local player's hand, matching the WEB self-hand: a FLAT
// ROW (not a fan). Cards share the width, clamped 20-50pt wide at a fixed 70pt
// tall, no overlap, so every card is directly tappable. Tapping toggles
// multi-selection; DRAGGING a card lifts it out to play (the board resolves the
// drop against the table/battles). The board turns selection+gesture into a move,
// never this view.

import SwiftUI

public struct FHandFan: View {
    public let cards: [Card]
    public let trumpSuit: Suit?
    /// Cards currently locked (in-flight / illegal in this context) — dimmed.
    public let disabled: Set<String>
    @Binding public var selection: Set<String>
    /// Tap a card — the board toggles it in the selection.
    public let onTap: (Card) -> Void
    /// Drag updates: the dragged card + the current point in `boardSpace` (for the
    /// live drop-target highlight). Ended: resolve + play the drop.
    public let onDragChanged: (Card, CGPoint) -> Void
    public let onDragEnded: (Card, CGPoint) -> Void
    /// Shared card-flight namespace: a card keeps its identity when it moves from
    /// the hand to the table, so matchedGeometry animates the flight.
    public let namespace: Namespace.ID?

    public init(cards: [Card], trumpSuit: Suit?, disabled: Set<String> = [],
                selection: Binding<Set<String>>, onTap: @escaping (Card) -> Void,
                onDragChanged: @escaping (Card, CGPoint) -> Void = { _, _ in },
                onDragEnded: @escaping (Card, CGPoint) -> Void = { _, _ in },
                namespace: Namespace.ID? = nil) {
        self.cards = cards
        self.trumpSuit = trumpSuit
        self.disabled = disabled
        self._selection = selection
        self.onTap = onTap
        self.onDragChanged = onDragChanged
        self.onDragEnded = onDragEnded
        self.namespace = namespace
    }

    @State private var dragId: String?
    @State private var dragOffset: CGSize = .zero

    private let cardH: CGFloat = 70          // web: fixed 70pt tall
    private let gap: CGFloat = 4

    public var body: some View {
        GeometryReader { geo in
            let count = max(cards.count, 1)
            let avail = geo.size.width - gap * CGFloat(count + 1)
            let cardW = min(50, max(20, avail / CGFloat(count)))
            HStack(spacing: gap) {
                ForEach(cards, id: \.identity) { card in
                    FCard(card: card,
                          selected: selection.contains(card.identity),
                          disabled: disabled.contains(card.identity),
                          trump: trumpSuit != nil && card.suit == trumpSuit,
                          size: CGSize(width: cardW, height: cardH))
                        .modifier(FlightID(id: card.identity, namespace: namespace))
                        .contentShape(Rectangle())
                        .offset(dragId == card.identity ? dragOffset : .zero)
                        .zIndex(dragId == card.identity ? 1000 : 0)
                        .onTapGesture {
                            guard !disabled.contains(card.identity) else { Haptics.fire(.reject); return }
                            Haptics.fire(.pickUp)
                            onTap(card)
                        }
                        .gesture(
                            DragGesture(minimumDistance: 12, coordinateSpace: .named(boardSpace))
                                .onChanged { g in
                                    guard !disabled.contains(card.identity) else { return }
                                    dragId = card.identity
                                    dragOffset = g.translation
                                    onDragChanged(card, g.location)
                                }
                                .onEnded { g in
                                    let c = card
                                    let wasDragging = dragId == c.identity
                                    dragId = nil; dragOffset = .zero
                                    if wasDragging { onDragEnded(c, g.location) }
                                }
                        )
                }
            }
            .frame(width: geo.size.width, height: geo.size.height, alignment: .center)
        }
        .frame(height: cardH + 8)
        // Publish the hand's frame so a drop back inside it cancels the play.
        .background(GeometryReader { g in
            Color.clear.preference(key: HandFrameKey.self, value: g.frame(in: .named(boardSpace)))
        })
    }
}
