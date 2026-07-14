// WatchFoolishApp.swift — @main + hierarchical navigation
// (docs/WATCHOS_APP_PLAN.md §5.3): Games ▸ Table ▸ Action, return via the system
// Back chevron / left-edge swipe. Three levels, the HIG maximum. This is the
// design-warm-up shell driving MockGame; W1/W2 swap in FoolishKit's engine.

import SwiftUI

/// Drill-down routes. The only navigation is this hierarchy + system Back.
enum Route: Hashable { case table(String), action }

@main
struct WatchFoolishApp: App {
    @StateObject private var game = MockGame()
    @State private var path: [Route] = []
    @State private var showOver = false

    var body: some Scene {
        WindowGroup {
            NavigationStack(path: $path) {
                GamesListView(game: game)
                    .navigationDestination(for: Route.self) { route in
                        switch route {
                        case .table:
                            TableScreen(game: game, onPlay: { path.append(.action) })
                                .navigationTitle("")
                        case .action:
                            ActionScreen(game: game)
                                .navigationTitle("")
                        }
                    }
            }
            .tint(WColor.brass)
            .preferredColorScheme(.dark)
            .fullScreenCover(isPresented: $showOver) {
                GameOverScreen(foolName: game.foolName ?? "Boris", onRematch: { showOver = false })
            }
            .onAppear(perform: applyLaunchScreen)
        }
    }

    /// `-table` / `-action` / `-over` open directly on that screen so each can be
    /// inspected without hand-tapping through the flow. Default: Games.
    private func applyLaunchScreen() {
        let args = ProcessInfo.processInfo.arguments
        if args.contains("-table") { path = [.table("g1")] }
        else if args.contains("-action") { path = [.table("g1"), .action] }
        else if args.contains("-over") { game.foolName = "Boris"; showOver = true }
    }
}
