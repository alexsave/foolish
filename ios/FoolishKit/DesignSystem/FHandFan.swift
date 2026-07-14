// FHandFan.swift — the local player's hand, fanned along the bottom and
// reachable one-handed (§5.4). Tap-to-select then play, with expanded hit slop
// (≥44pt). Overlapping cards fan with a slight arc; the selected card lifts.
// Drag-to-play is a post-v1 refinement (§5.4 "drag or tap-to-play"); v1 ships
// tap-to-play, which is the one-handed-reachable default.

import SwiftUI

public struct FHandFan: View {
    public let cards: [Card]
    public let trumpSuit: Suit?
    /// Cards currently locked (in-flight / illegal in this context) — dimmed.
    public let disabled: Set<String>
    @Binding public var selection: Set<String>
    public let onTap: (Card) -> Void
    /// Drag-to-play: reports the dragged card + finger location in the named
    /// coordinate space `dragSpace` (default no-op so tap-only hosts still work).
    public let dragSpace: String
    public let onDragChanged: (Card, CGPoint) -> Void
    public let onDragEnded: (Card, CGPoint) -> Void

    @State private var draggingId: String?

    public init(cards: [Card], trumpSuit: Suit?, disabled: Set<String> = [],
                selection: Binding<Set<String>>, onTap: @escaping (Card) -> Void,
                dragSpace: String = "table",
                onDragChanged: @escaping (Card, CGPoint) -> Void = { _, _ in },
                onDragEnded: @escaping (Card, CGPoint) -> Void = { _, _ in }) {
        self.cards = cards
        self.trumpSuit = trumpSuit
        self.disabled = disabled
        self._selection = selection
        self.onTap = onTap
        self.dragSpace = dragSpace
        self.onDragChanged = onDragChanged
        self.onDragEnded = onDragEnded
    }

    public var body: some View {
        GeometryReader { geo in
            let count = max(cards.count, 1)
            let cardW: CGFloat = 62
            let cardH: CGFloat = 88
            // Overlap so a big post-pickup hand still fits; clamp spread to width.
            let maxSpread = geo.size.width - cardW
            let step = min(cardW * 0.72, maxSpread / CGFloat(max(count - 1, 1)))
            let totalW = step * CGFloat(count - 1)
            let startX = (geo.size.width - totalW) / 2

            ZStack {
                ForEach(Array(cards.enumerated()), id: \.element.identity) { idx, card in
                    let mid = CGFloat(count - 1) / 2
                    let offset = CGFloat(idx) - mid
                    FCard(card: card,
                          selected: selection.contains(card.identity),
                          disabled: disabled.contains(card.identity),
                          trump: trumpSuit != nil && card.suit == trumpSuit,
                          size: CGSize(width: cardW, height: cardH))
                        .rotationEffect(.degrees(Double(offset) * 3), anchor: .bottom)
                        .position(x: startX + step * CGFloat(idx), y: geo.size.height - cardH / 2)
                        // Expanded hit slop for one-handed reach (a11y 44pt floor).
                        .contentShape(Rectangle().inset(by: -8))
                        .matchedGeometryEffect(id: card.identity, in: fanNamespace, isSource: true)
                        // Dim the source while its drag shadow is in flight.
                        .opacity(draggingId == card.identity ? 0.25 : 1)
                        .zIndex(draggingId == card.identity ? 200 : (selection.contains(card.identity) ? 100 : Double(idx)))
                        .onTapGesture {
                            guard !disabled.contains(card.identity) else { Haptics.fire(.reject); return }
                            Haptics.fire(.pickUp)
                            onTap(card)
                        }
                        .gesture(
                            DragGesture(minimumDistance: 12, coordinateSpace: .named(dragSpace))
                                .onChanged { v in
                                    guard !disabled.contains(card.identity) else { return }
                                    if draggingId == nil { draggingId = card.identity; Haptics.fire(.pickUp) }
                                    onDragChanged(card, v.location)
                                }
                                .onEnded { v in
                                    let wasDragging = draggingId == card.identity
                                    draggingId = nil
                                    if wasDragging { onDragEnded(card, v.location) }
                                }
                        )
                }
            }
        }
        .frame(height: 120)
    }

    @Namespace private var fanNamespace
}
