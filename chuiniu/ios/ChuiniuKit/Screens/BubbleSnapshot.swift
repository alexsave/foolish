// COPIED from pickemup/ios/PickemupKit/Screens/BubbleSnapshot.swift at 03eb3362 - a later lift into shared/ replaces it
//
// The transcript bubble's picture: foolish's 300 x 195, baked at insert and
// identical on every phone. It draws the PUBLIC table: the cups and their
// counts and the bid, never anybody's dice while a round is bid; after a call,
// the tally. A lobby bubble is the roster.
//
// Rendered synchronously from the model read at the moment of staging.

import SwiftUI
import UIKit

public enum BubbleSnapshot {
    public static let size = CGSize(width: 300, height: 195)

    @MainActor
    public static func render(table: TableModel, title: String, scheme: ColorScheme) -> UIImage? {
        let content = ZStack {
            FeltBackground()
            if table.phase == .lobby {
                LobbyPicture(table: table, title: title)
            } else {
                TablePicture(table: table)
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
    let table: TableModel
    let title: String

    var body: some View {
        VStack(spacing: 6) {
            Text(title).font(.system(size: 15, weight: .heavy)).onFeltText()
            VStack(alignment: .leading, spacing: 2) {
                ForEach(table.seats) { seat in
                    Text(seat.name).font(.system(size: 12.5, weight: .heavy)).onFeltText().lineLimit(1)
                }
            }
        }
        .padding(.top, 12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }
}

private struct TablePicture: View {
    let table: TableModel

    var body: some View {
        VStack(spacing: 10) {
            HStack(spacing: 14) {
                ForEach(table.seats) { seat in
                    VStack(spacing: 2) {
                        ZStack(alignment: .bottomTrailing) {
                            Cup(width: 26)
                            if seat.alive { CountBadge(n: seat.dice).scaleEffect(0.8).offset(x: 7, y: 3) }
                        }
                        .opacity(seat.alive ? 1 : 0.45)
                        Text(seat.name).font(.system(size: 10, weight: .semibold)).onFeltText().lineLimit(1)
                            .frame(maxWidth: 52)
                    }
                }
            }
            // the move this bubble carries: my staged raise first (the
            // committed table does not have it yet), else the table's news
            if let b = table.stagedBid {
                BidLine(text: table.stagedBidText, face: b.face)
            } else if let r = table.reveal {
                Text(r.tally).font(.system(size: 20, weight: .heavy)).onFeltText().lineLimit(1)
                    .minimumScaleFactor(0.6)
            } else if !table.bidText.isEmpty {
                BidLine(text: table.bidText, face: table.bid?.face)
            }
        }
        .padding(.horizontal, 12)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct BidLine: View {
    let text: String
    let face: Int?

    var body: some View {
        HStack(spacing: 8) {
            Text(text).font(.system(size: 28, weight: .heavy)).onFeltText().lineLimit(1)
                .minimumScaleFactor(0.6)
            if let face { Die(face: face, size: 28) }
        }
    }
}
