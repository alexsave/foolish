// RosterScreen.swift — Option G page 2 (docs/WATCHOS_G_SPEC.md §3). The player list +
// game state: one row per seat (name · count · shield if defending), you in gold; a
// footer with deck/discard/flip and who's out. Swipe left from the table to reach it.

import SwiftUI

struct RosterScreen: View {
    @ObservedObject var game: WatchGame

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Indent clears the system back chevron that sits top-left (§1).
            Text(header).font(WFont.label(12)).foregroundStyle(WColor.dim)
                .padding(.leading, 34)
                .padding(.bottom, 4)

            ForEach(game.rosterRows) { r in row(r) }

            Spacer(minLength: 4)

            VStack(alignment: .leading, spacing: 1) {
                Text(flipLine).font(WFont.label(11)).foregroundStyle(WColor.dim)
                Text(outLine).font(WFont.label(11)).foregroundStyle(WColor.dim)
            }
            .lineLimit(1).minimumScaleFactor(0.8)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .padding(.horizontal, 6)
        .background(WColor.bg)
    }

    private func row(_ r: RosterRow) -> some View {
        HStack(spacing: 5) {
            Text(r.name)
                .font(WFont.label(12.5))
                .strikethrough(r.isOut, color: WColor.faint)
                .foregroundStyle(r.isOut ? WColor.faint : (r.isSelf ? WColor.gold : WColor.suitBlack))
                .lineLimit(1)
            if r.isDefender {
                ShieldShape().stroke(WColor.gold, lineWidth: 1)
                    .frame(width: 9, height: 11)
            }
            Spacer(minLength: 2)
            Text("\(r.count)")
                .font(WFont.heavy(12.5))
                .foregroundStyle(r.isOut ? WColor.faint : (r.isSelf ? WColor.gold : WColor.seat))
        }
        .frame(height: 16.5)
        .opacity(r.isOut ? 0.6 : 1)
    }

    private var header: String {
        if let s = game.trumpSuit { return "durak · \(game.seatStrip.count)p · \(s.glyph)" }
        return "durak · \(game.seatStrip.count)p"
    }

    private var flipLine: String {
        let base = "deck \(game.deckCount) · disc \(game.discardCount)"
        if let f = game.flipped {
            return base + " · flip \(CardRank.label(f.v))\(f.suit?.glyph ?? "") under deck"
        } else if let s = game.trumpSuit {
            return base + " · trump \(s.glyph)"
        }
        return base
    }
    private var outLine: String {
        let names = game.outNames
        return "out: " + (names.isEmpty ? "—" : names.joined(separator: ", "))
    }
}
