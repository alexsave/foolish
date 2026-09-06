// RosterScreen.swift — the roster page (docs/WATCHOS_G_SPEC.md §3). The player list +
// game state: one row per seat (name · count · shield if defending), you in gold; a
// footer with deck/discard/flip and who's out. Under Option H this is a page PUSHED by
// tapping the table's seat strip — the system back chevron returns (§4.6).

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

            // No "out: …" line — an eliminated player is SHOWN by their row going dark
            // gray (owner review), not announced in a footer.
            Text(flipLine).font(WFont.label(11)).foregroundStyle(WColor.dim)
                .lineLimit(1).minimumScaleFactor(0.8)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .padding(.horizontal, 6)
        .background(WColor.bg)
    }

    /// Escaped players read as dark gray — no strikethrough, no label, no footer entry.
    /// They keep their place at the bottom of the list and simply recede.
    private func row(_ r: RosterRow) -> some View {
        HStack(spacing: 5) {
            Text(r.name)
                .font(.system(size: 12.5, weight: r.isSelf ? HTuning.stripSelfWeight : .regular, design: .rounded))
                .foregroundStyle(color(r))
                .lineLimit(1)
            if r.isDefender {
                ShieldShape().stroke(WColor.defender, lineWidth: 1)
                    .frame(width: 9, height: 11)
            }
            Spacer(minLength: 2)
            Text("\(r.count)")
                .font(.system(size: 12.5, weight: r.isSelf ? HTuning.stripSelfWeight : .regular, design: .rounded))
                .foregroundStyle(color(r))
        }
        .frame(height: 16.5)
    }

    /// Same colour language as the strip (§4.6.1): out ▸ defender ▸ good ▸ opening ▸ plain.
    /// You are bold, not gold — colour is reserved for state.
    private func color(_ r: RosterRow) -> Color {
        if r.isOut { return WColor.out }
        if r.isDefender { return WColor.defender }
        if r.isGood { return WColor.green }
        if r.isOpening { return WColor.attacker }
        return WColor.plain
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
}
