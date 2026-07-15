// GameRoot.swift — Option H root (docs/WATCHOS_LAYOUT.md §4.6). The TableScreen is the
// whole game screen; the Roster sits one page to its right. Two ways there, both owner-
// specified: drag right→left (the page swipe) or tap the seat strip. There are no page
// dots — the face is too tight for them, and the strip is the discoverable door.
//
// This is the one place H relaxes its "zero horizontal gestures" rule (§4.6.1). Opening
// on the table keeps the system back-swipe (left edge → pop to the games list) unshadowed;
// putting the roster on the LEFT would have cost that.
// Game over presents the fool reveal (§10).

import SwiftUI

struct GameRootView: View {
    @ObservedObject var game: WatchGame
    let onExit: () -> Void
    @State private var showOver = false
    @State private var page = ProcessInfo.processInfo.arguments.contains("-roster") ? 1 : 0

    var body: some View {
        TabView(selection: $page) {
            TableScreen(game: game, onOpenRoster: { withAnimation { page = 1 } }).tag(0)
            RosterScreen(game: game).tag(1)
        }
        .tabViewStyle(.page(indexDisplayMode: .never))
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
