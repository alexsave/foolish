// GamesAndOver.swift — the root Games list (§5.3) and the fool-reveal screen
// (§4b). Games rows follow the Weather-forecast grammar (§3.1 cue 5): opponent
// left, right-aligned state glyph + hand count, "HOD!" (your move) in brass.

import SwiftUI

struct GamesListView: View {
    @ObservedObject var game: MockGame

    var body: some View {
        List {
            ForEach(game.games) { g in
                NavigationLink(value: g.id) {
                    HStack(spacing: 6) {
                        Text(g.opponent).font(WFont.label(17)).foregroundStyle(WColor.ink)
                        Spacer()
                        if g.yourTurn {
                            Text("HOD!").font(WFont.token(13)).foregroundStyle(WColor.brass)
                        } else {
                            Text("wait").font(WFont.label(12)).foregroundStyle(WColor.dim)
                        }
                        Text("\(g.handCount)").font(WFont.token(15)).foregroundStyle(WColor.dim)
                            .frame(minWidth: 16, alignment: .trailing)
                    }
                    .padding(.vertical, 2)
                }
                .listRowBackground(
                    RoundedRectangle(cornerRadius: 10)
                        .fill(g.yourTurn ? WColor.brass.opacity(0.14) : Color(white: 0.08))
                )
            }

            NavigationLink(value: "bot") {
                HStack(spacing: 6) {
                    Image(systemName: "plus.circle.fill").foregroundStyle(WColor.dim)
                    Text("Bot game").font(WFont.label(16)).foregroundStyle(WColor.ink)
                }
            }
            .listRowBackground(RoundedRectangle(cornerRadius: 10).fill(Color(white: 0.08)))
        }
        .listStyle(.carousel)
        .navigationTitle("Games")
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
                Text("Rematch").font(WFont.label(15)).frame(maxWidth: .infinity, minHeight: 34)
            }
            .buttonStyle(PillStyle(primary: true))
        }
        .padding(.horizontal, 10)
        .padding(.bottom, 4)
        .background(WColor.bg)
    }
}
