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
// THE MOTION IS THE KERNEL'S TIMELINE (BeatPlayer, pk_beats.h). While a plan
// plays, the board draws the plan's frame instead of the settled view (the
// count, the pile, whose turn, my hand with the cards still in the air
// unseen), every anchored element takes its beat's transform (Anchors.swift,
// PkFX), and the flight layer draws the cards in the air between the anchors'
// frames. With no plan it is the settled view, and a new state snaps in.
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
    /// Every anchor's frame in the board space, for the flights.
    @State private var anchors: [String: CGRect] = [:]
    @ObservedObject private var player: BeatPlayer

    public init(model: TableModel, onRules: @escaping () -> Void) {
        self.model = model
        self.onRules = onRules
        self.player = model.player
    }

    public var body: some View {
        GeometryReader { outer in
            let inset = PkLayout.boardInset
            let board = CGSize(width: max(0, outer.size.width - inset.leading - inset.trailing),
                               height: max(0, outer.size.height - inset.top - inset.bottom))
            let collapse = PkLayout.collapse(viewHeight: outer.size.height)
            TimelineView(.animation(paused: !player.animating)) { _ in
                let ms = player.ms()
                let shown = model.shown(player.frame(ms))
                ZStack(alignment: .topLeading) {
                    boardView(board, collapse: collapse, rows: PkLayout.maxRows(viewHeight: outer.size.height),
                              shown: shown, ms: ms)
                        .opacity(shown.holds(PK_HOLD_BOARD) ? 0 : 1)
                        .pkAnchor("board")
                    FlightLayer(ghosts: player.ghosts(ms, anchors: anchors))
                }
                .environment(\.pkFX, player.effects(ms, anchors: anchors))
                .frame(width: board.width, height: board.height, alignment: .topLeading)
                .coordinateSpace(name: boardSpace)
                .onPreferenceChange(PkAnchorKey.self) { anchors = $0 }
            }
            .offset(x: inset.leading, y: inset.top)
        }
        .background(FeltBackground())
    }

    @ViewBuilder
    private func boardView(_ board: CGSize, collapse: CGFloat, rows: Int, shown: TableModel.Shown,
                           ms: Int) -> some View {
        let v = model.view
        let hand = shown.hand
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
                    // the reveal rows once the plan's reveal has begun (or with
                    // no plan, a finished table); backs before
                    let revealing = model.isOver && (!shown.playing || shown.revealShown != nil)
                    SeatBadge(seat: seat, name: seat < model.names.count ? model.names[seat] : "",
                              revealed: revealing ? v?.reveal[safe: seat]?.card : nil,
                              isTurn: shown.turn == seat,
                              stamp: shown.stampHeld(seat) ? nil : model.stamp(seat), calling: model.calling(seat),
                              fanEmpty: shown.fanEmpty(seat),
                              revealShown: shown.playing ? shown.revealShown?[safe: seat] : nil,
                              onTapFan: { model.tapFan(seat) })
                        .position(PkLayout.seat(seat, me: me, count: n, board: board, collapse: collapse))
                }
            }

            StatusCorner(headline: model.headline, subline: model.subline, strip: model.strip,
                         onUnsay: { model.unsay() })
                .offset(x: 2, y: 0)

            if !model.direction.isEmpty, !shown.holds(PK_HOLD_DIR) {
                DirectionBox(word: Pk.words(PK_API_W_DIR_OF, shown.dir), clockwise: shown.dir == PK_DIR_CW)
                    .offset(x: board.width - 78, y: -3)
            }

            PileView(top: shown.top,
                     stackCount: shown.stackN, liveSuit: shown.suit,
                     chosen: topIsWild(shown.top) ? shown.suit : nil,
                     pending: shown.holds(PK_HOLD_PENDING) ? nil
                         : model.pickerFor.flatMap { $0 < hand.count ? hand[$0] : nil },
                     hot: pileHot)
                .position(pc)
                .overlay(alignment: .topLeading) {
                    Color.clear.frame(width: PkLayout.pileSize.width, height: PkLayout.pileSize.height)
                        .offset(x: pc.x - PkLayout.pileSize.width / 2, y: pc.y - PkLayout.pileSize.height / 2)
                        .pkAnchor("stack")
                        .allowsHitTesting(false)
                }

            DeckStack(count: shown.deckN, label: Pk.words(PK_API_W_DECK_N, shown.deckN), buried: model.buried,
                      lifted: deckDrag != nil, buriedHold: shown.buriedHold)
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
                    hidden: model.pickerFor, unseen: shown.unseen,
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

            // up while the picker is asked, and while its tiles fall back in
            if model.pickerFor != nil || player.pending(PK_BK_COLLAPSE, at: ms) {
                SuitPicker(centre: pc, onPick: { model.choose($0) }, onCancel: { model.cancelPicker() })
                    .frame(width: board.width, height: board.height)
                    .allowsHitTesting(model.pickerFor != nil)
            }

            if model.isOver, !shown.holds(PK_HOLD_RESULTS) {
                ResultsPlank(model: model)
                    .position(x: board.width / 2, y: pc.y)
            }
        }
    }

    private func topIsWild(_ top: Int?) -> Bool {
        guard let top, let f = CardFace(top) else { return false }
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

/// The cards in the air (BeatPlayer.ghosts): each one a face or a back tweened
/// between two anchors' frames, on the kernel's curve. foolish's FlyingCardsLayer
/// in shape; every position and transform is a sample of the timeline.
struct FlightLayer: View {
    let ghosts: [BeatPlayer.Ghost]

    var body: some View {
        ZStack(alignment: .topLeading) {
            ForEach(ghosts) { g in
                PkCard(card: g.card, size: g.size, fullFace: true)
                    .brightness(g.dimmed ? -0.2 : 0)
                    .overlay {
                        if g.retract {       // UI.html `.retract`: foolish's red retraction ghost
                            RoundedRectangle(cornerRadius: 5)
                                .fill(Color(red: 1, green: 150 / 255, blue: 150 / 255).opacity(0.35))
                                .overlay(RoundedRectangle(cornerRadius: 5).strokeBorder(Color(hex: 0xDC2626), lineWidth: 2))
                        }
                    }
                    .scaleEffect(x: g.scale * g.scaleX, y: g.scale)
                    .rotationEffect(.degrees(g.rot))
                    .position(g.center)
            }
        }
        .allowsHitTesting(false)
        .zIndex(1500)
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
