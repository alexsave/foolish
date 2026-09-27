// TableScreen.swift - the table: the seat row, the dice tray in the middle of
// the felt, the status line and the Roll pill under it, and my own card below
// (T10, T53).
//
// pickemup's board, re-laid for dice: its felt, its board inset (8/8/14/4),
// its 96 x 40 wood pill 12pt from the trailing edge, its felt text. The seat
// ring becomes one row across the top (T53), and the pile's place in the
// middle holds the tray.
//
// THE TWO WAYS A TURN MOVES ON (T3): the Roll pill stages a KEEP of the
// unmarked dice; a tap on an open row of my card stages a SCORE, which ends
// the turn. Both are one call on the model, and the drawer's staging is the
// controller's.
//
// KERNEL: pickemup reads every one of these numbers from pk_lay.c; when
// tb_lay.c exists, TbLayout's literals become calls, as PkLayout's are.

import SwiftUI

/// T53: the table's layout numbers, all in the board's coordinates.
public enum TbLayout {
    /// pickemup's board inset inside the extension's view.
    public static let inset = (leading: CGFloat(8), trailing: CGFloat(8), top: CGFloat(14), bottom: CGFloat(4))
    /// The seat row's height (SeatBadge.height) and the gap under it.
    public static let seatRowGap: CGFloat = 8
    /// The gap from the tray to the status and pill row, and from that row to the card.
    public static let trayGap: CGFloat = 10
    public static let pillWidth: CGFloat = 96
    public static let pillHeight: CGFloat = 40
    public static let pillTrailing: CGFloat = 12

    public static var trayTop: CGFloat { SeatBadge.height + seatRowGap }
    public static var pillTop: CGFloat { trayTop + DiceFace.side + trayGap }
    public static var cardTop: CGFloat { pillTop + pillHeight + trayGap }
}

public struct TableScreen: View {
    @ObservedObject var model: TallyTable
    @ObservedObject private var tumble: TumblePlayer
    @State private var looking: CardModel?

    public init(model: TallyTable) {
        self.model = model
        self.tumble = model.tumble
    }

    public var body: some View {
        GeometryReader { outer in
            let i = TbLayout.inset
            let board = CGSize(width: max(0, outer.size.width - i.leading - i.trailing),
                               height: max(0, outer.size.height - i.top - i.bottom))
            boardView(board)
                .frame(width: board.width, height: board.height, alignment: .topLeading)
                .coordinateSpace(name: boardSpace)
                .offset(x: i.leading, y: i.top)
        }
        .background(FeltBackground())
        .onPreferenceChange(TbAnchorKey.self) { TbAnchors.latest = $0 }
        .sheet(item: $looking) { card in
            OtherCardSheet(model: model, card: card) { looking = nil }
        }
    }

    @ViewBuilder
    private func boardView(_ board: CGSize) -> some View {
        let tray = model.tray
        let others = model.others
        VStack(spacing: 0) {
            SeatRow(model: model, others: others, width: board.width) { looking = $0 }
                .frame(height: SeatBadge.height)
                .padding(.bottom, TbLayout.seatRowGap)

            DiceTray(tray: tray, motion: tumble, animating: tumble.animating) { model.toggle($0) }
                .frame(maxWidth: .infinity)
                .tbAnchor("tray")

            HStack(alignment: .center, spacing: 8) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(model.view.headline)
                        .font(.system(size: 14, weight: .heavy))
                        .onFeltText()
                        .lineLimit(1)
                        .minimumScaleFactor(0.75)
                    if !model.view.rollLine.isEmpty {
                        Text(model.view.rollLine)
                            .font(.system(size: 12, weight: .semibold))
                            .onFeltText(FColor.textDim)
                    }
                }
                .tbAnchor("status")
                Spacer(minLength: 0)
                if tray.phase == .rolling {
                    WoodButton(title: model.kernel.string(.roll), width: TbLayout.pillWidth,
                               height: TbLayout.pillHeight, enabled: tray.canRoll) { model.roll() }
                        .tbAnchor("pill.roll")
                }
            }
            .frame(height: TbLayout.pillHeight)
            .padding(.leading, 4)
            .padding(.trailing, TbLayout.pillTrailing)
            .padding(.top, TbLayout.trayGap)

            if let card = model.myCard {
                ScrollView(.vertical, showsIndicators: false) {
                    Scorecard(card: card,
                              rows: ScoreRow.rows(card: card, preview: model.preview, draft: model.view.draft,
                                                  canScore: tray.canScore, name: model.kernel.categoryName),
                              words: model.kernel.string,
                              onPick: { model.score($0) })
                        .padding(.horizontal, 2)
                        .padding(.bottom, 8)
                }
                .padding(.top, TbLayout.trayGap)
            }
            Spacer(minLength: 0)
        }
    }
}

/// Everybody but me, in one row across the top (T53).
struct SeatRow: View {
    @ObservedObject var model: TallyTable
    let others: [CardModel]
    let width: CGFloat
    let onTap: (CardModel) -> Void

    var body: some View {
        let n = max(others.count, 1)
        let w = min(SeatBadge.nameMax, width / CGFloat(n))
        HStack(spacing: 0) {
            Spacer(minLength: 0)
            ForEach(others) { card in
                SeatBadge(seat: card.seat, name: card.name, total: card.total,
                          isTurn: model.view.tray.turn == card.seat, width: w) { onTap(card) }
            }
            Spacer(minLength: 0)
        }
    }
}

/// Another seat's card, read-only, on the felt.
struct OtherCardSheet: View {
    @ObservedObject var model: TallyTable
    let card: CardModel
    let onClose: () -> Void

    var body: some View {
        // the newest copy of this seat's card, not the one tapped
        let live = model.view.cards.first { $0.seat == card.seat } ?? card
        VStack(spacing: 12) {
            Text(live.name)
                .font(.system(size: 17, weight: .heavy))
                .onFeltText()
            Scorecard(card: live,
                      rows: ScoreRow.rows(card: live, preview: [], draft: .none, canScore: false,
                                          name: model.kernel.categoryName),
                      words: model.kernel.string, onPick: nil)
            WoodButton(title: model.kernel.string(.close), width: TbLayout.pillWidth,
                       height: TbLayout.pillHeight, action: onClose)
            Spacer(minLength: 0)
        }
        .padding(16)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(FeltBackground())
        .presentationDetents([.medium])
    }
}
