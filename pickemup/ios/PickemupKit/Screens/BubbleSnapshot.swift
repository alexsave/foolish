// COPIED from foolish/ios/FoolishKit/Boards/BubbleSnapshot.swift at c3d99192 - replaced by lift step S16
//
// The transcript bubble's picture (U17): foolish's 300 x 195, baked at
// insert and identical on every phone. It draws the PUBLIC table - the
// spectator's view, so the sender's own hand is a fan of backs like
// everybody's and no hand shows a count - and, once the game is over, every
// hand face up. A lobby bubble is the numbered roster and who deals.
//
// Rendered synchronously, from the resident, at the moment of staging: the
// resident is one slot, so nothing reads it across an await.

import CPickemup
import SwiftUI
import UIKit

public enum BubbleSnapshot {
    public static let size = CGSize(width: 300, height: 195)

    @MainActor
    public static func render(scheme: ColorScheme) -> UIImage? {
        let content = ZStack {
            FeltBackground()
            if (Pk.table()?.phase ?? PK_PHASE_WAITING) == PK_PHASE_WAITING {
                LobbyPicture()
            } else {
                TablePicture()
            }
        }
        .frame(width: size.width, height: size.height)
        .environment(\.colorScheme, scheme)
        .dynamicTypeSize(.large)
        let renderer = ImageRenderer(content: content)
        renderer.scale = UIScreen.main.scale
        renderer.isOpaque = true
        return renderer.uiImage
    }
}

private struct LobbyPicture: View {
    var body: some View {
        let n = Pk.table()?.seat.count ?? 0
        VStack(spacing: 6) {
            Text(Pk.string("LOBBY_TITLE")).font(.system(size: 15, weight: .heavy)).onFeltText()
            VStack(alignment: .leading, spacing: 2) {
                ForEach(0..<n, id: \.self) { s in
                    // the public roster: no "(You)", the picture is everybody's
                    Text(Pk.words(PK_API_W_PUBLIC_ROW, s))
                        .font(.system(size: 12.5, weight: .heavy)).onFeltText().lineLimit(1)
                }
            }
            Text(Pk.words(PK_API_W_LOBBY_DEALER)).font(.system(size: 11, weight: .semibold)).onFeltText(FColor.textDim)
        }
        .padding(.top, 12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }
}

private struct TablePicture: View {
    var body: some View {
        let v = Pk.view(Pk.viewerSpectator)
        let t = Pk.table()
        let n = t?.seat.count ?? 0
        let board = BubbleSnapshot.size
        let over = (v?.over ?? 0) != 0
        ZStack(alignment: .topLeading) {
            Color.clear
            if let v, let top = v.top == PK_CARD_NONE ? nil : v.top {
                PkCard(card: top, size: CGSize(width: 44, height: 62), fullFace: true,
                       chosen: CardFace(top)?.isWild == true ? v.liveSuit : nil)
                    .rotationEffect(.degrees(-3))
                    .background(Circle().fill(RadialGradient(colors: [SuitInk.halo(v.liveSuit), .clear], center: .center,
                                                             startRadius: 0, endRadius: 48)).frame(width: 96, height: 96))
                    .position(x: board.width / 2, y: board.height / 2)
                Text(Pk.words(PK_API_W_DECK_LEFT))
                    .font(.system(size: 11, weight: .bold)).foregroundColor(.white)
                    .shadow(color: .black, radius: 1, y: 1)
                    .position(x: board.width / 2 - 52, y: board.height / 2)
            }
            if (v?.showDir ?? 0) != 0 {
                Text(Pk.words(PK_API_W_DIR).uppercased())
                    .font(.system(size: 8.5, weight: .bold)).tracking(1.2).foregroundColor(Color(hex: 0x8FADA6))
                    .position(x: board.width - 44, y: 12)
            }
            ForEach(0..<n, id: \.self) { seat in
                SeatBadge(seat: seat, name: Pk.words(PK_API_W_SEAT, seat),
                          revealed: over ? v?.reveal[safe: seat]?.card : nil,
                          isTurn: false,
                          stamp: TableModel.Stamp(kernel: Pk.stamp(seat)), calling: false, onTapFan: {})
                    .scaleEffect(0.78)
                    .position(PkLayout.seat(seat, me: -1, count: n, board: board, collapse: 0.5))
            }
        }
        .frame(width: board.width, height: board.height)
    }
}
