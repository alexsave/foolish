// TableScreen.swift - UI.html's Table, Collapsed, Draw, Throw, Wild picker,
// Staged turn, Call-out and the end reveal, as one board.
//
// foolish's board (MessageTableView) minus the flipped trump, the discard
// pile and the battle grid: the pile in the centre, the deck immediately to
// its LEFT, the direction word in the freed top-right, the status line in
// the top-left corner, the seat ring, the hand on the drawer's bottom edge
// and the pill row above it. Every position is the kernel's layout
// (PkLayout, pk_lay.c); every word the kernel's table; every touch one call
// on the model.
//
// STATIC ON PURPOSE. A new state snaps in. The flights, stamps, riffles and
// turn-bar fades of the motion grid are the next layer's: every element here
// reports its frame under the grid's anchor name (Anchors.swift), so that
// layer can fly between them without changing this file's structure.
//
// THE DECK'S DRAG STAYS INSIDE THE EXTENSION (U24): foolish's
// DragGesture(minimumDistance: 0) in the board space, attached with
// highPriorityGesture, so it claims the touch on touch-down, before any pan
// of the host's can begin; nothing in a drag asks Messages for a presentation
// change. Whether a downward drag starting mid-board can still collapse the
// drawer is a device question (IOS_DECISIONS.md I5, open).

import CPickemup
import SwiftUI

public struct TableScreen: View {
    @ObservedObject var model: TableModel
    let onRules: () -> Void

    @State private var pileHot = false
    @State private var deckDrag: CGSize?
    @State private var bandHot = false

    public init(model: TableModel, onRules: @escaping () -> Void) {
        self.model = model
        self.onRules = onRules
    }

    public var body: some View {
        GeometryReader { outer in
            let inset = PkLayout.boardInset
            let board = CGSize(width: max(0, outer.size.width - inset.leading - inset.trailing),
                               height: max(0, outer.size.height - inset.top - inset.bottom))
            let collapse = PkLayout.collapse(viewHeight: outer.size.height)
            boardView(board, collapse: collapse, rows: PkLayout.maxRows(viewHeight: outer.size.height))
                .frame(width: board.width, height: board.height, alignment: .topLeading)
                .coordinateSpace(name: boardSpace)
                .offset(x: inset.leading, y: inset.top)
        }
        .background(FeltBackground())
    }

    @ViewBuilder
    private func boardView(_ board: CGSize, collapse: CGFloat, rows: Int) -> some View {
        let v = model.view
        let hand = model.hand
        let handW = max(0, board.width - PkLayout.handPadding * 2)
        let layout = PkLayout.hand(count: hand.count, width: handW, maxRows: rows)
        let handTop = board.height - layout.boxHeight
        let pc = PkLayout.pileCentre(board: board, collapse: collapse)
        let deckAt = PkLayout.deckOrigin(board: board, collapse: collapse)
        let pileRect = CGRect(x: pc.x - PkLayout.pileSize.width / 2 - 8, y: pc.y - PkLayout.pileSize.height / 2 - 8,
                              width: PkLayout.pileSize.width + 16, height: PkLayout.pileSize.height + 16)
        let band = CGRect(x: PkLayout.handPadding, y: handTop - 64, width: handW, height: layout.boxHeight + 88)
        let me = model.me ?? -1
        let n = model.seatCount
        let pills = model.pills

        ZStack(alignment: .topLeading) {
            Color.clear

            // the seat ring: everybody but me (a spectator sees them all)
            ForEach(0..<n, id: \.self) { seat in
                if seat != me {
                    SeatBadge(seat: seat, name: seat < model.names.count ? model.names[seat] : "",
                              revealed: model.isOver ? v?.reveal[safe: seat]?.card : nil,
                              isTurn: !model.isOver && v?.turn == seat,
                              stamp: model.stamp(seat), calling: model.calling(seat),
                              onTapFan: { model.tapFan(seat) })
                        .position(PkLayout.seat(seat, me: me, count: n, board: board, collapse: collapse))
                }
            }

            StatusCorner(headline: model.headline, subline: model.subline, strip: model.strip,
                         onUnsay: { model.unsay() })
                .offset(x: 2, y: 0)

            if !model.direction.isEmpty, let v {
                DirectionBox(word: model.direction, clockwise: v.dir == PK_DIR_CW)
                    .offset(x: board.width - 78, y: -3)
            }

            PileView(top: v.map { $0.top == PK_CARD_NONE ? nil : $0.top } ?? nil,
                     stackCount: v?.stackN ?? 0, liveSuit: v?.liveSuit ?? 0,
                     chosen: topIsWild(v) ? v?.liveSuit : nil,
                     pending: model.pickerFor.flatMap { $0 < hand.count ? hand[$0] : nil },
                     hot: pileHot)
                .position(pc)
                .overlay(alignment: .topLeading) {
                    Color.clear.frame(width: PkLayout.pileSize.width, height: PkLayout.pileSize.height)
                        .offset(x: pc.x - PkLayout.pileSize.width / 2, y: pc.y - PkLayout.pileSize.height / 2)
                        .pkAnchor("stack")
                        .allowsHitTesting(false)
                }

            DeckStack(count: v?.deckN ?? 0, label: model.deckLeft, buried: model.buried, lifted: deckDrag != nil)
                .offset(x: deckAt.x, y: deckAt.y)
                .highPriorityGesture(deckGesture(band: band, origin: deckAt))

            if let drag = deckDrag {
                deckGhost(at: CGPoint(x: deckAt.x + drag.width, y: deckAt.y + drag.height))
            }

            if bandHot {
                RoundedRectangle(cornerRadius: 7)
                    .fill(FColor.win.opacity(0.12))
                    .overlay(RoundedRectangle(cornerRadius: 7)
                        .strokeBorder(FColor.win, style: StrokeStyle(lineWidth: 2.5, dash: [6, 4])))
                    .frame(width: band.width, height: band.height)
                    .offset(x: band.minX, y: band.minY)
                    .allowsHitTesting(false)
            }

            LeftChrome(maySay: model.maySay, onSay: { model.sayIt() }, onRules: onRules)
                .frame(width: board.width)
                .offset(y: handTop - 4 - 40)

            PillRow(trailing: pills.trailing, leading: pills.leading,
                    onDraw: { model.draw() }, onPlay: { model.playSelected() },
                    onPass: { model.pass() }, onUndo: { model.undo() })
                .frame(width: board.width)
                .offset(y: handTop - 4 - 40)

            HandRow(cards: hand, layout: layout, selected: model.selected, dimmed: { model.dimmed($0) },
                    hidden: model.pickerFor,
                    onTap: { model.tap($0) },
                    onDragMoved: { _, p in pileHot = pileRect.contains(p) },
                    onDragEnded: { pos, p in
                        pileHot = false
                        if pileRect.contains(p) { model.play(pos) }
                    })
                .frame(width: handW)
                .offset(x: PkLayout.handPadding, y: handTop)

            if let toast = model.toast {
                Toast(text: toast)
                    .position(x: board.width / 2, y: handTop - 64)
            }

            if model.pickerFor != nil {
                SuitPicker(centre: pc, onPick: { model.choose($0) }, onCancel: { model.cancelPicker() })
                    .frame(width: board.width, height: board.height)
            }

            if model.isOver {
                ResultsPlank(model: model)
                    .position(x: board.width / 2, y: pc.y)
            }
        }
    }

    private func topIsWild(_ v: PkViewSnap?) -> Bool {
        guard let v, let f = CardFace(v.top) else { return false }
        return f.isWild
    }

    /// Tap the deck (a touch that ends within 8pt), or drag a back off it and
    /// let go in the hand band (the hand grown 64 up and 24 down): both are
    /// one DRAW. Anywhere else the back springs home and nothing happened.
    private func deckGesture(band: CGRect, origin: CGPoint) -> some Gesture {
        DragGesture(minimumDistance: 0, coordinateSpace: .named(boardSpace))
            .onChanged { g in
                guard (model.view?.canDraw ?? 0) != 0 else { return }
                if hypot(g.translation.width, g.translation.height) >= HandRow.tapThreshold {
                    deckDrag = g.translation
                    bandHot = band.contains(g.location)
                }
            }
            .onEnded { g in
                let moved = hypot(g.translation.width, g.translation.height) >= HandRow.tapThreshold
                let inBand = band.contains(g.location)
                withAnimation(FMotion.card) { deckDrag = nil }
                bandHot = false
                if !moved || inBand { model.draw() }
            }
    }

    /// The back riding the finger, with foolish's verb 52 above it.
    private func deckGhost(at p: CGPoint) -> some View {
        ZStack(alignment: .topLeading) {
            PkCard(card: nil, size: PkLayout.deckSize)
                .rotationEffect(.degrees(-4))
                .scaleEffect(1.08)
            Text(Pk.string("BTN_DRAW"))
                .font(.system(size: 13, weight: .semibold))
                .onFeltText(FColor.card)
                .fixedSize()
                .offset(x: 6, y: -52 + PkLayout.deckSize.height / 2 - 8)
        }
        .offset(x: p.x, y: p.y)
        .allowsHitTesting(false)
        .zIndex(2000)
    }
}

/// The end reveal's surface: who won, the order, and Again. Every hand is
/// already face up on the ring (the reveal rows).
struct ResultsPlank: View {
    @ObservedObject var model: TableModel

    var body: some View {
        let ranks = Pk.ranks()
        VStack(spacing: 8) {
            Text(model.headline)
                .font(.system(size: 17, weight: .heavy))
                .onWoodText()
            VStack(alignment: .leading, spacing: 3) {
                ForEach(0..<ranks.count, id: \.self) { i in
                    Text(Pk.words(PK_API_W_RANK_ROW, i))
                        .font(.system(size: 14, weight: .heavy))
                        .onWoodText(dimmed: i > 0)
                        .lineLimit(1)
                }
            }
            WoodButton(title: Pk.string("BTN_AGAIN"), width: 120, action: { model.again() })
                .overlay(Rectangle().strokeBorder(Color.black.opacity(0.5), lineWidth: 1))
        }
        .padding(14)
        .frame(width: 210)
        .background(WoodFill())
        .overlay(Rectangle().strokeBorder(Color.black.opacity(0.35), lineWidth: 1))
        .shadow(color: .black.opacity(0.5), radius: 10, y: 6)
        .pkAnchor("results")
    }
}

extension Array {
    subscript(safe i: Int) -> Element? { i >= 0 && i < count ? self[i] : nil }
}
