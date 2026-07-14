// RootView.swift — switches between Home and the live Table, and floats the Win
// screen over a finished game. Owns the "leave a live game" confirmation (the
// only destructive confirm in the app, §5.5).

import SwiftUI
import FoolishKit

struct RootView: View {
    @StateObject private var coordinator = AppCoordinator()
    @EnvironmentObject private var auth: AuthService
    @State private var lastConfig: OfflineConfig?
    @State private var confirmLeave = false
    @State private var showJoinAuth = false

    /// Consume a pending universal-link join: join now if signed in, else prompt.
    private func tryPendingJoin() {
        guard let code = coordinator.pendingJoinCode, Backend.shared.isConfigured else { return }
        if let uid = auth.userId {
            coordinator.pendingJoinCode = nil
            coordinator.joinOnline(gameId: code, userId: uid)
        } else {
            showJoinAuth = true
        }
    }

    var body: some View {
        ZStack {
            switch coordinator.screen {
            case .home:
                HomeView(
                    onStartOffline: { config in
                        lastConfig = config
                        coordinator.startOffline(config)
                    },
                    onQuickMatch: {
                        if let uid = auth.userId { coordinator.startOnline(userId: uid) }
                    },
                    onJoin: { code in
                        if let uid = auth.userId { coordinator.joinOnline(gameId: code, userId: uid) }
                    }
                )
            case .table:
                if let game = coordinator.offlineGame {
                    tableStack(game)
                }
            case .onlineTable:
                if let game = coordinator.onlineGame {
                    OnlineStack(game: game, onHome: { coordinator.goHome() })
                }
            }
        }
        .background(WoolBackground())
        .alert("Couldn’t start online game",
               isPresented: Binding(get: { coordinator.onlineError != nil },
                                    set: { if !$0 { coordinator.onlineError = nil } })) {
            Button("OK", role: .cancel) { coordinator.onlineError = nil }
        } message: {
            Text(coordinator.onlineError ?? "")
        }
        .onOpenURL { coordinator.handle(url: $0) }
        // A universal link that decoded as a live game (not a replay): join it
        // once a user is available, prompting auth first if needed.
        .onChange(of: coordinator.pendingJoinCode) { _ in tryPendingJoin() }
        .onChange(of: auth.userId) { _ in tryPendingJoin() }
        .sheet(isPresented: $showJoinAuth) {
            AuthView(onSignedIn: { tryPendingJoin() })
        }
        .onAppear {
            coordinator.maybeAutostartFromLaunchArgs()
            #if DEBUG
            maybeAutostartOnline()
            #endif
        }
        .sheet(item: $coordinator.pendingReplay) { pending in
            NavigationStack { ReplayPlayerView(replay: pending.replay) }
                .preferredColorScheme(.dark)
        }
    }

    #if DEBUG
    /// QA hook: `-onlineAutostart` signs in a throwaway user and quick-matches on
    /// launch, dropping straight into the online lobby so the online flow can be
    /// driven/screenshotted without hand-tapping auth. Needs a configured backend
    /// (a local `supabase start`). No effect in normal launches.
    private func maybeAutostartOnline() {
        guard ProcessInfo.processInfo.arguments.contains("-onlineAutostart"),
              Backend.shared.isConfigured, coordinator.screen == .home,
              coordinator.onlineGame == nil else { return }
        let args = ProcessInfo.processInfo.arguments
        let autoplay = args.contains("-onlineAutoplay")
        Task {
            if !auth.isSignedIn {
                let name = "SIM" + String(UUID().uuidString.prefix(6).uppercased())
                try? await auth.signUp(username: name, password: "password123")
            }
            guard let uid = auth.userId else { return }
            coordinator.startOnline(userId: uid)
            // `-onlineLobbyBots N` adds N bots and STAYS in the lobby (to inspect
            // the picker / reorder / remove UI). `-onlineAutoplay` goes further.
            let lobbyBots = ProcessInfo.processInfo.arguments.firstIndex(of: "-onlineLobbyBots")
                .flatMap { i in i + 1 < args.count ? Int(args[i + 1]) : nil } ?? 0
            guard autoplay || lobbyBots > 0 else { return }
            for _ in 0..<60 where coordinator.onlineGame == nil { try? await Task.sleep(nanoseconds: 100_000_000) }
            guard let g = coordinator.onlineGame else { return }
            try? await Task.sleep(nanoseconds: 600_000_000)
            let wanted = autoplay ? 1 : lobbyBots
            for target in 1...max(wanted, 1) {
                g.addBot()
                for _ in 0..<60 where g.roster.count < target + 1 { try? await Task.sleep(nanoseconds: 100_000_000) }
            }
            // `-onlineReorder` exercises the drag→rearrange path programmatically.
            if args.contains("-onlineReorder"), g.roster.count >= 2 {
                try? await Task.sleep(nanoseconds: 400_000_000)
                g.rearrange(newOrder: g.roster.map(\.playerId).reversed())
            }
            guard autoplay else { return }
            g.startGame()
            // Once dealt and it's our turn, fire one real move through the packed
            // action path to prove the outgoing move round-trip end to end.
            for _ in 0..<80 where g.isWaiting || g.humanLegal.isEmpty { try? await Task.sleep(nanoseconds: 100_000_000) }
            if let move = g.humanLegal.first { g.play(move) }
        }
    }
    #endif

    @ViewBuilder
    private func tableStack(_ game: LocalGame) -> some View {
        ZStack(alignment: .topLeading) {
            TableView(game: game, onLeave: { confirmLeave = true })

            // Leave affordance (chrome kept minimal; only shown mid-game).
            if game.foolSeat == nil {
                Button(action: { confirmLeave = true }) {
                    Image(systemName: "xmark")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(FColor.textDim)
                        .padding(FSpace.m)
                }
                .accessibilityLabel(FStrings.t("leave"))
            }

            // Win screen over the settled board (§6 screen 4).
            if let fool = game.foolSeat {
                WinView(
                    game: game,
                    foolSeat: fool,
                    humanSeat: game.humanSeat,
                    onRematch: { if let c = lastConfig { coordinator.rematch(c) } },
                    onHome: { coordinator.goHome() }
                )
                .transition(.opacity)
            }
        }
        .animation(FMotion.chrome, value: game.foolSeat)
        .confirmationDialog(FStrings.t("leave_game_title"), isPresented: $confirmLeave, titleVisibility: .visible) {
            Button(FStrings.t("leave"), role: .destructive) { coordinator.goHome() }
            Button(FStrings.t("cancel"), role: .cancel) {}
        } message: {
            Text(FStrings.t("leave_game_body"))
        }
    }

}

// Online stack: WAITING → the lobby (add bots / start); dealt → the board; over
// → the win screen. A dedicated `@ObservedObject` view (not a ViewBuilder func on
// RootView) so it re-renders on the game's own @Published transitions — the
// lobby→table flip is driven by `view` arriving on the feed, which RootView
// itself doesn't observe. Leave returns home and tears down the feed (§16.D5).
private struct OnlineStack: View {
    @ObservedObject var game: OnlineGame
    let onHome: () -> Void
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var confirmLeave = false

    var body: some View {
        ZStack(alignment: .topLeading) {
            if game.isWaiting {
                LobbyView(game: game, onLeave: { confirmLeave = true })
            } else {
                TableView(game: game, onLeave: { confirmLeave = true })
            }
            if game.foolSeat == nil {
                Button(action: { confirmLeave = true }) {
                    Image(systemName: "xmark")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(FColor.textDim)
                        .padding(FSpace.m)
                }
                .accessibilityLabel(FStrings.t("leave"))
            }
            if let fool = game.foolSeat {
                WinView(game: game, foolSeat: fool, humanSeat: game.humanSeat,
                        onRematch: onHome, onHome: onHome)
                    .transition(.opacity)
            }
        }
        .animation(FMotion.chrome, value: game.foolSeat)
        .confirmationDialog(FStrings.t("leave_game_title"), isPresented: $confirmLeave, titleVisibility: .visible) {
            Button(FStrings.t("leave"), role: .destructive) {
                if game.isWaiting { game.leaveLobby() }   // best-effort tidy of an abandoned lobby
                onHome()
            }
            Button(FStrings.t("cancel"), role: .cancel) {}
        } message: {
            Text(FStrings.t("leave_game_body"))
        }
    }
}
