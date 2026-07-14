// WatchFoolishApp.swift — @main + hierarchical navigation
// (docs/WATCHOS_APP_PLAN.md §5.3): New-game ▸ Table ▸ Action in ONE NavigationStack,
// so each pushed screen gets the SYSTEM back button — a thin brass ‹ inline with
// the clock (not a nav-bar strip below it), the native watchOS header. Return via
// that chevron / left-edge swipe. Every game is a REAL offline game driven by
// FoolishKit's LocalGame over the C kernel (W1/W2); the kernel is a single global,
// so exactly one game is live at a time.

import SwiftUI

/// The drill-down past the root list. Table is a push, Action a push on top.
enum Route: Hashable { case table, action }

/// Owns the live game + nav path so a New-game tap can set the game BEFORE the
/// push (no nil-race when the destination for `.table` is first evaluated).
@MainActor final class Nav: ObservableObject {
    @Published var path: [Route] = []
    @Published var game: WatchGame?

    func start(_ players: Int) {
        game = WatchGame(players: players, botStrategy: WatchGame.defaultStrategy)
        path = [.table]
    }
}

@main
struct WatchFoolishApp: App {
    @StateObject private var nav = Nav()

    var body: some Scene {
        WindowGroup {
            NavigationStack(path: $nav.path) {
                GamesListView(onNew: nav.start)
                    .navigationDestination(for: Route.self) { route in
                        switch route {
                        case .table:
                            if let g = nav.game {
                                TableContainer(game: g,
                                               onPlay: { nav.path.append(.action) },
                                               onRematch: { nav.path = [.table] })
                            }
                        case .action:
                            if let g = nav.game {
                                ActionScreen(game: g).navigationTitle("")
                            }
                        }
                    }
            }
            .tint(WColor.brass)          // tints the system back chevron brass
            .preferredColorScheme(.dark)
            .onAppear(perform: applyLaunchScreen)
        }
    }

    /// `-table`/`-bot` open a heads-up game, `-stress` the 8-seat ring, and the
    /// `-action*` variants drill straight to the play screen for inspection.
    private func applyLaunchScreen() {
        let args = ProcessInfo.processInfo.arguments
        if args.contains("-stress") || args.contains("-action8") { nav.start(8) }
        else if args.contains("-table") || args.contains("-bot") || args.contains("-botplay")
                    || args.contains("-defend") || args.contains("-attack") || args.contains("-action") { nav.start(2) }
        else { return }
        if args.contains("-action") || args.contains("-action8") || args.contains("-botplay") { nav.path.append(.action) }
    }
}

/// Wraps the table and owns the fool-reveal cover. It observes the game (the App
/// root can't see the wrapped object's own publishes), so `foolName` becoming
/// non-nil presents. The system supplies the back button — no custom chrome here.
private struct TableContainer: View {
    @ObservedObject var game: WatchGame
    let onPlay: () -> Void
    let onRematch: () -> Void
    @State private var showOver = false

    var body: some View {
        TableScreen(game: game, onPlay: onPlay)
            .navigationTitle(game.turnText)      // the title line beside the clock
            .onChange(of: game.foolName) { showOver = $0 != nil }
            .fullScreenCover(isPresented: $showOver) {
                GameOverScreen(foolName: game.foolName ?? "Bot") {
                    game.rematch()
                    showOver = false
                    onRematch()
                }
            }
    }
}
