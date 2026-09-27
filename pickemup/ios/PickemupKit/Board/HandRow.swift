// COPIED from foolish/ios/FoolishKit/DesignSystem/FHandFan.swift at c3d99192 - replaced by lift step S11
//
// foolish's hand row and its one gesture per card: DragGesture(minimumDistance
// 0) in the board space, where a touch that travels under 8pt is a tap
// (foolish's tapThreshold), so selecting and dragging share one recognizer.
//
// WHAT IS NOT COPIED, ON PURPOSE:
//   - the reorder. The hand is in acquisition order and the kernel owns it
//     (D24): new cards go on the right and nobody rearranges.
//   - the geometry. Every slot is pk_lay_hand_slot's (O4, U7, U8): one row,
//     foolish's two, overlap to a 16pt strip, then the rows scroll.
//
// WHILE THE ROWS SCROLL the card gesture is a plain tap, so the horizontal
// pan belongs to the scroll view; drag-to-play there is tap + Play (see
// IOS_DECISIONS.md I7).

import CPickemup
import SwiftUI

struct HandRow: View {
    let cards: [Int]
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

    @State private var dragPos: Int?
    @State private var dragOffset: CGSize = .zero
    @State private var dragMoved = false

    static let tapThreshold: CGFloat = 8

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
        // the row makes room on card-spring as a slot opens or closes
        .animation(FMotion.card, value: cards.count)
        .pkAnchor("hand")
    }

    private var cardsLayer: some View {
        ZStack(alignment: .topLeading) {
            Color.clear
            ForEach(Array(cards.enumerated()), id: \.offset) { pos, card in
                if pos < layout.slots.count {
                    let slot = layout.slots[pos]
                    cardView(pos, card, slot)
                        .offset(x: slot.minX, y: slot.minY)
                        .zIndex(dragPos == pos ? 1000 : Double(pos))
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
                .offset(dragPos == pos ? dragOffset : .zero)
                .gesture(
                    DragGesture(minimumDistance: 0, coordinateSpace: .named(boardSpace))
                        .onChanged { g in
                            if dragPos != pos { dragPos = pos; dragMoved = false }
                            dragOffset = g.translation
                            if hypot(g.translation.width, g.translation.height) >= Self.tapThreshold { dragMoved = true }
                            if dragMoved { onDragMoved(pos, g.location) }
                        }
                        .onEnded { g in
                            let moved = dragMoved || hypot(g.translation.width, g.translation.height) >= Self.tapThreshold
                            withAnimation(FMotion.card) { dragPos = nil; dragOffset = .zero }
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
}
