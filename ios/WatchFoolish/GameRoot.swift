// GameRoot.swift — Option G root (docs/WATCHOS_G_SPEC.md §1). A horizontal TabView(.page)
// with two pages: the TableScreen (play) and the RosterScreen (state). The system back
// chevron (from the NavigationStack push) owns the top-left; horizontal swipe moves
// between the two pages. Game over presents the fool reveal (§10).

import SwiftUI

struct GameRootView: View {
    @ObservedObject var game: WatchGame
    let onExit: () -> Void
    @State private var showOver = false
    @State private var page = ProcessInfo.processInfo.arguments.contains("-roster") ? 1 : 0

    var body: some View {
        TabView(selection: $page) {
            TableScreen(game: game).tag(0)
            RosterScreen(game: game).tag(1)
        }
        .tabViewStyle(.page(indexDisplayMode: .automatic))
        .background(WColor.bg.ignoresSafeArea())
        .onChange(of: game.foolName) { showOver = $0 != nil }
        .onAppear { if game.foolName != nil { showOver = true } }
        .fullScreenCover(isPresented: $showOver) {
            GameOverScreen(foolName: game.foolName ?? "Bot",
                           escapeOrder: game.outNames) {
                showOver = false
                onExit()
            }
        }
    }
}
