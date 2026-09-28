// COPIED from foolish/ios/FoolishKit/DesignSystem/FHandFan.swift at c3d99192 - replaced by lift step S11
// The drag-to-reorder: COPIED from foolish/ios/FoolishKit/DesignSystem/FHandFan.swift at 02c97c60 - replaced by lift step S11
//
// foolish's hand row and its one gesture per card: DragGesture(minimumDistance
// 0) in the board space, where a touch that travels under 8pt is a tap
// (foolish's tapThreshold), so selecting, dragging to play and dragging to
// rearrange share one recognizer.
//
// THE REORDER IS FOOLISH'S (ORCHESTRATION O9, IOS_DECISIONS I38): while the
// finger is still in the hand row, the dragged card asks for the slot whose
// centre is nearest its own centre (FHandFan.slotIndex, here the kernel's
// pk_lay_hand_nearest) and goes there live, the others sliding apart;
// `reorderShift` cancels the slot's jump so the card stays pinned to the
// finger (FHandFan.reorder). A release in the row is a rearrange and never a
// play; on the pile it plays (pk_lay_drop, FHandFan.boardPoint's rule).
//
// WHAT IS NOT FOOLISH'S, ON PURPOSE:
//   - the order itself. foolish keeps its arrangement in the view (`order`);
//     here the kernel keeps it in the phone's record (pk_arrange.h) and every
//     card is named by its hand position, the identity the events, the
//     anchors (hand.i) and a PLAY use. `slotOf[pos]` is only where it is drawn.
//   - the geometry. Every slot is pk_lay_hand_slot's (O4, U7, U8): one row,
//     foolish's two, overlap to a 16pt strip, then the rows scroll.
//
// WHILE THE ROWS SCROLL the card gesture is a plain tap, so the horizontal
// pan belongs to the scroll view; drag-to-play there is tap + Play and there
// is no rearranging (IOS_DECISIONS I11).

import CPickemup
import SwiftUI

struct HandRow: View {
    let cards: [Int]
    /// Where each hand position is drawn (the kernel's arrangement).
    var slotOf: [Int] = []
    let layout: PkLayout.Hand
    let selected: Int?
    let dimmed: (Int) -> Bool
    /// A card that is on its way somewhere (the wild waiting on the picker).
    let hidden: Int?
    /// Slots already open for cards still in the air (pk_beats frame).
    var unseen: Set<Int> = []
    let onTap: (Int) -> Void
    /// The drag's point in the board space, while it moves and where it ends.
    let onDragMoved: (Int, CGPoint) -> Void
    let onDragEnded: (Int, CGPoint) -> Void
    /// Is a board point in the hand row (pk_lay_drop)? Only there does a drag rearrange.
    var inRow: (CGPoint) -> Bool = { _ in false }
    /// The slot a card centred at a point of the hand's box asks for.
    var nearest: (CGPoint) -> Int? = { _ in nil }
    /// Move hand position `pos` to arranged slot `slot`; true if it moved.
    var onReorder: (Int, Int) -> Bool = { _, _ in false }

    @State private var dragPos: Int?
    @State private var dragOffset: CGSize = .zero
    @State private var dragMoved = false
    /// The slot the drag started from, captured once (FHandFan's grabSlot).
    @State private var grabSlot: CGRect = .zero
    /// What the rearranges so far moved the dragged card's slot by.
    @State private var reorderShift: CGSize = .zero

    private func slot(_ pos: Int) -> Int { slotOf[safe: pos] ?? pos }

    var body: some View {
        Group {
            if layout.mode == .scroll {
                ScrollView(.horizontal, showsIndicators: true) {
                    cardsLayer.frame(width: layout.contentWidth, height: layout.boxHeight, alignment: .topLeading)
                }
                .mask(LinearGradient(stops: [.init(color: .black, location: 0.88), .init(color: .clear, location: 1)],
                                     startPoint: .leading, endPoint: .trailing))
            } else {
                cardsLayer
            }
        }
        .frame(height: layout.boxHeight)
        // the row makes room on card-spring as a slot opens or closes, or a
        // rearrange moves the others
        .animation(FMotion.card, value: cards.count)
        .animation(FMotion.card, value: slotOf)
        .pkAnchor("hand")
    }

    private var cardsLayer: some View {
        ZStack(alignment: .topLeading) {
            Color.clear
            ForEach(Array(cards.enumerated()), id: \.offset) { pos, card in
                let s = slot(pos)
                if s < layout.slots.count {
                    let r = layout.slots[s]
                    cardView(pos, card, r)
                        .offset(x: r.minX, y: r.minY)
                        .zIndex(dragPos == pos ? 1000 : Double(s))
                        .accessibilitySortPriority(Double(layout.slots.count - s))
                }
            }
        }
    }

    @ViewBuilder private func cardView(_ pos: Int, _ card: Int, _ slot: CGRect) -> some View {
        let face = PkCard(card: card, size: slot.size, selected: selected == pos, dimmed: dimmed(pos),
                          fullFace: layout.mode != .flat)
            .opacity(hidden == pos || unseen.contains(pos) ? 0 : 1)
            .contentShape(Rectangle())
            .pkAnchor("hand.\(pos)")
            .accessibilityLabel(Pk.words(PK_API_W_SPOKEN_CARD, pos))
            .accessibilityAddTraits(.isButton)
        if layout.mode == .scroll {
            face.onTapGesture { Haptics.fire(.pickUp); onTap(pos) }
        } else {
            face
                .offset(dragPos == pos
                        ? CGSize(width: dragOffset.width - reorderShift.width,
                                 height: dragOffset.height - reorderShift.height)
                        : .zero)
                .gesture(
                    DragGesture(minimumDistance: 0, coordinateSpace: .named(boardSpace))
                        .onChanged { g in
                            if dragPos != pos {
                                dragPos = pos
                                dragMoved = false
                                reorderShift = .zero
                                grabSlot = slot
                            }
                            dragOffset = g.translation
                            if hypot(g.translation.width, g.translation.height) >= PkLayout.tapSlop { dragMoved = true }
                            // A TAP MUST NEVER REORDER (FHandFan): only past the slop
                            guard dragMoved else { return }
                            onDragMoved(pos, g.location)
                            if inRow(g.location) { reorder(pos, g.translation) }
                        }
                        .onEnded { g in
                            let moved = dragMoved || hypot(g.translation.width, g.translation.height) >= PkLayout.tapSlop
                            withAnimation(FMotion.card) { dragPos = nil; dragOffset = .zero; reorderShift = .zero }
                            dragMoved = false
                            if moved {
                                onDragEnded(pos, g.location)
                            } else {
                                Haptics.fire(.pickUp)
                                onTap(pos)
                            }
                        }
                )
        }
    }

    /// FHandFan.reorder: the dragged card's own centre (where it started plus
    /// the finger's travel, so where inside the card it was grabbed does not
    /// matter) asks for a slot; a new one moves it there under the card
    /// spring, and the shift pins it to the finger through the slide.
    private func reorder(_ pos: Int, _ travel: CGSize) {
        let centre = CGPoint(x: grabSlot.midX + travel.width, y: grabSlot.midY + travel.height)
        let from = slot(pos)
        guard let to = nearest(centre), to != from, to < layout.slots.count, from < layout.slots.count else { return }
        let d = CGSize(width: layout.slots[to].minX - layout.slots[from].minX,
                       height: layout.slots[to].minY - layout.slots[from].minY)
        withAnimation(FMotion.card) {
            if onReorder(pos, to) {
                reorderShift.width += d.width
                reorderShift.height += d.height
            }
        }
    }
}
