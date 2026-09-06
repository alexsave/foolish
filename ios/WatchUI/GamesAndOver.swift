// GamesAndOver.swift — the root New-game list (§5.3) and the fool-reveal screen
// (§4b). The C kernel is a single global game, so the root is a menu of table
// sizes; picking one deals a REAL offline game vs the C bots.

import SwiftUI

struct GamesListView: View {
    /// player count (you + bots) for the game to deal.
    let onNew: (Int) -> Void

    private let options: [(label: String, players: Int, bots: Int)] = [
        ("Heads-up",   2, 1),
        ("Four-hand",  4, 3),
        ("Full table", 8, 7),
    ]

    var body: some View {
        List {
            ForEach(options, id: \.players) { o in
                Button { onNew(o.players) } label: {
                    HStack(spacing: 8) {
                        Text("\(o.players)").font(WFont.token(18)).foregroundStyle(WColor.plain)
                            .frame(minWidth: 18)
                        VStack(alignment: .leading, spacing: 0) {
                            Text(o.label).font(WFont.label(16)).foregroundStyle(WColor.suitBlack)
                            Text("you + \(o.bots) \(botLabel)").font(WFont.label(11))
                                .foregroundStyle(WColor.dim)
                                .lineLimit(1).minimumScaleFactor(0.7)
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

    /// The opponent every row deals against — named, because "7 bots" and "7 octogen bots"
    /// are very different games. Read straight off the roster: building a WatchGame here
    /// would boot a second kernel game (the C kernel is a single global).
    private var botLabel: String {
        EngineC.roster().first { $0.id == WatchGame.defaultStrategy }?.name ?? "bots"
    }
}

/// Game over (§10): a small ДУРАК label, the loser large in red, the escape order on
/// one line, and a CLOSE button back to the New-game list.
struct GameOverScreen: View {
    let foolName: String
    let escapeOrder: [String]
    let onClose: () -> Void

    var body: some View {
        VStack(spacing: 6) {
            Spacer()
            Text("ДУРАК")
                .font(WFont.label(13)).tracking(2)
                .foregroundStyle(WColor.dim)
            Text(foolName)
                .font(.system(size: 34, weight: .heavy, design: .rounded))
                .foregroundStyle(WColor.red)
                .lineLimit(1).minimumScaleFactor(0.5)
            if !escapeOrder.isEmpty {
                Text(escapeOrder.joined(separator: " › "))
                    .font(WFont.label(11)).foregroundStyle(WColor.dim)
                    .lineLimit(1).minimumScaleFactor(0.7)
            }
            Spacer()
            Button(action: onClose) {
                Text("Close")
                    .font(WFont.label(15)).foregroundStyle(WColor.bg)
                    .frame(maxWidth: .infinity, minHeight: 34)
                    .background(Capsule().fill(WColor.plain))
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, 10)
        .padding(.bottom, 4)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(WColor.bg.ignoresSafeArea())
    }
}
