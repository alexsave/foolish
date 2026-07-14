// GamesAndOver.swift — the root New-game list (§5.3) and the fool-reveal screen
// (§4b). The C kernel is a single global game, so the root is a menu of table
// sizes; picking one deals a REAL offline game vs the C bots.

import SwiftUI

struct GamesListView: View {
    /// player count (you + bots) for the game to deal.
    let onNew: (Int) -> Void

    private let options: [(label: String, players: Int, sub: String)] = [
        ("Heads-up",   2, "you + 1 bot"),
        ("Four-hand",  4, "you + 3 bots"),
        ("Full table", 8, "you + 7 bots"),
    ]

    var body: some View {
        List {
            ForEach(options, id: \.players) { o in
                Button { onNew(o.players) } label: {
                    HStack(spacing: 8) {
                        Text("\(o.players)").font(WFont.token(18)).foregroundStyle(WColor.brass)
                            .frame(minWidth: 18)
                        VStack(alignment: .leading, spacing: 0) {
                            Text(o.label).font(WFont.label(16)).foregroundStyle(WColor.ink)
                            Text(o.sub).font(WFont.label(11)).foregroundStyle(WColor.dim)
                        }
                        Spacer()
                        SwordIcon(size: 13, color: WColor.dim).frame(width: 20, height: 20)
                    }
                    .padding(.vertical, 2)
                }
                .buttonStyle(.plain)
                .listRowBackground(RoundedRectangle(cornerRadius: 10).fill(Color(white: 0.08)))
            }
        }
        .listStyle(.carousel)
        .navigationTitle("New game")
        .background(WColor.bg)
    }
}

/// Game over — the fool reveal (§4b, §3.1 cue 1: the `Д` is the hero).
struct GameOverScreen: View {
    let foolName: String
    let onRematch: () -> Void

    var body: some View {
        VStack(spacing: 10) {
            Spacer()
            VStack(spacing: 0) {
                Text("\(foolName.uppercased()) is").foregroundStyle(WColor.ink)
                Text("the FOOL").foregroundStyle(WColor.ink)
            }
            .font(WFont.label(17))
            .multilineTextAlignment(.center)
            .lineLimit(1)
            .minimumScaleFactor(0.7)
            Text("Д")
                .font(.system(size: 56, weight: .heavy, design: .rounded))
                .foregroundStyle(WColor.brass)
            Spacer()
            Button(action: onRematch) {
                Text("Rematch")
                    .font(WFont.label(15)).foregroundStyle(WColor.bg)
                    .frame(maxWidth: .infinity, minHeight: 34)
                    .background(Capsule().fill(WColor.brass))
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, 10)
        .padding(.bottom, 4)
        .background(WColor.bg)
    }
}
