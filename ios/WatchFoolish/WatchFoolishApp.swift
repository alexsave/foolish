// WatchFoolishApp.swift — @main. New-game list ▸ GameRoot in ONE NavigationStack, so
// the pushed game screen gets the SYSTEM back chevron inline with the clock (top-left,
// never overridden, §1). Every game is a REAL offline game driven by FoolishKit's
// LocalGame over the C kernel; the kernel is a single global, so exactly one is live.

import SwiftUI

/// The one drill-down past the root list: the live game (Table/Roster pager).
enum Route: Hashable { case game }

/// Owns the live game + nav path so a New-game tap can set the game BEFORE the push.
@MainActor final class Nav: ObservableObject {
    @Published var path: [Route] = []
    @Published var game: WatchGame?

    func start(_ players: Int) {
        game = WatchGame(players: players, botStrategy: WatchGame.defaultStrategy)
        path = [.game]
    }
    func exit() { path = [] }
}

@main
struct WatchFoolishApp: App {
    @StateObject private var nav = Nav()

    var body: some Scene {
        WindowGroup {
            Group {
                if ProcessInfo.processInfo.arguments.contains("-over") {
                    GameOverScreen(foolName: "Boris", escapeOrder: ["Kat", "Mira", "you", "Nils"]) {}
                } else {
                    NavigationStack(path: $nav.path) {
                        GamesListView(onNew: nav.start)
                            .navigationDestination(for: Route.self) { _ in
                                if let g = nav.game {
                                    GameRootView(game: g, onExit: nav.exit)
                                        .navigationTitle("")
                                        .navigationBarTitleDisplayMode(.inline)
                                }
                            }
                    }
                }
            }
            .tint(WColor.gold)               // tints the system back chevron gold
            .preferredColorScheme(.dark)
            .onAppear(perform: applyLaunchScreen)
        }
    }

    /// Launch flags for inspection: `-stress`/`-table8` deal the 8-seat game;
    /// `-table`/`-bot` deal heads-up.
    private func applyLaunchScreen() {
        let args = ProcessInfo.processInfo.arguments
        if args.contains("-stress") || args.contains("-table8") { nav.start(8) }
        else if args.contains("-table4") { nav.start(4) }
        else if args.contains("-table") || args.contains("-bot") { nav.start(2) }
    }
}
