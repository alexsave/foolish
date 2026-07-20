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
    /// Cards currently in overlay flight (a draw landing) — rendered invisible so
    /// only the flying ghost shows.
    public let hidden: Set<String>

    public init(cards: [Card], trumpSuit: Suit?, disabled: Set<String> = [],
                selection: Binding<Set<String>>, onTap: @escaping (Card) -> Void,
                onDragChanged: @escaping (Card, CGPoint) -> Void = { _, _ in },
                onDragEnded: @escaping (Card, CGPoint) -> Void = { _, _ in },
                namespace: Namespace.ID? = nil, hidden: Set<String> = []) {
        self.cards = cards
        self.trumpSuit = trumpSuit
        self.disabled = disabled
        self._selection = selection
        self.onTap = onTap
        self.onDragChanged = onDragChanged
        self.onDragEnded = onDragEnded
        self.namespace = namespace
        self.hidden = hidden
    }

    @State private var dragId: String?
    @State private var dragOffset: CGSize = .zero
    /// note 5 (hand reordering): the local player's cosmetic display order —
    /// card identities, reconciled against `cards` every body evaluation via
    /// `displayCards`. This is PURELY a UI convenience (the kernel hand order
    /// never changes): it lives only in this view's @State, so it resets
    /// whenever FHandFan is recreated (a board reload swaps in a fresh view),
    /// and is never persisted or sent anywhere.
    @State private var order: [String] = []
    /// This view's own frame in `boardSpace`, mirrored from the same
    /// `HandFrameKey` preference it publishes below — read locally so a
    /// reorder can tell "drag point still inside the hand" from "dragged out
    /// to play" without needing the board to feed it back down.
    @State private var handFrameSelf: CGRect = .zero
    /// Cumulative x-compensation from in-flight reorders, subtracted from
    /// `dragOffset` so the DRAGGED card's own `.offset` doesn't jump when its
    /// slot in `order` moves out from under it (only the OTHER cards should
    /// visibly slide — the dragged one keeps tracking the finger).
    @State private var reorderShift: CGFloat = 0

    private let cardH: CGFloat = 72          // CONSTANT height — a skinny (many-card) hand stays this tall
    private let maxCardW: CGFloat = 52       // never wider than ~proper aspect, so cards never go "superwide"
    private let gap: CGFloat = 4
    private var rowH: CGFloat { cardH + 8 }

    /// `cards` reordered by the local `order` state: identities still present
    /// keep their relative order from `order`; any identity in `cards` not yet
    /// in `order` (a fresh hand, or a card just dealt in) is appended in
    /// kernel order. Pure — never mutates `order` itself, so it's safe to read
    /// from `body` on every evaluation.
    private var displayCards: [Card] {
        guard !order.isEmpty else { return cards }
        let byId = Dictionary(uniqueKeysWithValues: cards.map { ($0.identity, $0) })
        var seen = Set<String>()
        var result: [Card] = []
        result.reserveCapacity(cards.count)
        for id in order {
            if let c = byId[id], seen.insert(id).inserted { result.append(c) }
        }
        for c in cards where !seen.contains(c.identity) {
            result.append(c); seen.insert(c.identity)
        }
        return result
    }

    /// In-hand reorder (note 5): turn the drag's x (within this view's own
    /// width) into a slot index and, if it differs from the dragged card's
    /// current slot, splice `order` there under a spring — the other cards
    /// slide apart, mirroring the web's live reorder feel. `cardW` is `body`'s
    /// per-evaluation slot width (depends on hand width/count), passed in
    /// since it isn't a stored constant.
    private func reorder(_ card: Card, x: CGFloat, cardW: CGFloat) {
        let current = displayCards.map(\.identity)
        guard let from = current.firstIndex(of: card.identity) else { return }
        let slot = cardW + gap
        guard slot > 0 else { return }
        let raw = Int(((x - gap) / slot).rounded())
        let to = min(max(raw, 0), max(current.count - 1, 0))
        guard to != from else { return }
        var next = current
        next.remove(at: from)
        next.insert(card.identity, at: min(to, next.count))
        // Both mutations animate together (same spring, same transaction) so
        // the compensation tracks the layout's own transition instead of
        // stepping instantly while the slide is still mid-spring.
        withAnimation(FMotion.card) {
            order = next
            reorderShift += CGFloat(to - from) * slot
        }
    }

    public var body: some View {
        GeometryReader { geo in
            let count = max(cards.count, 1)
            let avail = geo.size.width - gap * CGFloat(count + 1)
            // Width shrinks to fit a big hand (skinny cards), grows only up to
            // maxCardW for a small hand. Height is CONSTANT either way - a skinny
            // card is narrow but full-height, never squished, never superwide.
            let cardW = min(maxCardW, max(22, avail / CGFloat(count)))
            HStack(spacing: gap) {
                ForEach(displayCards, id: \.identity) { card in
                    FCard(card: card,
                          selected: selection.contains(card.identity),
                          disabled: disabled.contains(card.identity),
                          trump: trumpSuit != nil && card.suit == trumpSuit,
                          size: CGSize(width: cardW, height: cardH))
                        .opacity(hidden.contains(card.identity) ? 0 : 1)
                        .modifier(FlightID(id: card.identity, namespace: namespace))
                        .background(GeometryReader { g in
                            Color.clear.preference(key: HandCardFramesKey.self,
                                                   value: [card.identity: g.frame(in: .named(boardSpace))])
                        })
                        .contentShape(Rectangle())
                        .offset(dragId == card.identity
                                ? CGSize(width: dragOffset.width - reorderShift, height: dragOffset.height)
                                : .zero)
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
                                    if dragId != card.identity { dragId = card.identity; reorderShift = 0 }
                                    dragOffset = g.translation
                                    onDragChanged(card, g.location)
                                    // note 5: live-reorder only while the point is still
                                    // inside the hand's own frame — once it leaves, this
                                    // is a play-drag and today's behavior is untouched.
                                    if handFrameSelf.contains(g.location) {
                                        reorder(card, x: g.location.x - handFrameSelf.minX, cardW: cardW)
                                    }
                                }
                                .onEnded { g in
                                    let c = card
                                    let wasDragging = dragId == c.identity
                                    dragId = nil; dragOffset = .zero; reorderShift = 0
                                    if wasDragging { onDragEnded(c, g.location) }
                                }
                        )
                }
            }
            .frame(width: geo.size.width, height: geo.size.height, alignment: .center)
        }
        .frame(height: rowH)
        // Publish the hand's frame so a drop back inside it cancels the play.
        .background(GeometryReader { g in
            Color.clear.preference(key: HandFrameKey.self, value: g.frame(in: .named(boardSpace)))
        })
        // Mirror that same frame locally (note 5) — this doesn't stop it from
        // also bubbling up to the board's own `onPreferenceChange`.
        .onPreferenceChange(HandFrameKey.self) { handFrameSelf = $0 }
    }
}
