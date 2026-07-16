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

    var body: some View {
        ZStack {
            switch coordinator.screen {
            case .home:
                HomeView(
                    onStartOffline: { config in
                        lastConfig = config
                        coordinator.startOffline(config)
                    },
                    // Signed-in Play lands on the Dashboard (web flow: login →
                    // dashboard → create/join → lobby).
                    onQuickMatch: { coordinator.screen = .dashboard }
                )
            case .dashboard:
                DashboardView(
                    onCreate: { if let uid = auth.userId { coordinator.createOnline(userId: uid) } },
                    onJoin: { code in if let uid = auth.userId { coordinator.joinOnline(code: code, userId: uid) } },
                    onClose: { coordinator.goHome() }
                )
            case .table:
                if let game = coordinator.offlineGame {
                    tableStack(game)
                }
            case .onlineTable:
                if let game = coordinator.onlineGame {
                    onlineTableStack(game)
                }
            }
        }
        .background(FColor.table.ignoresSafeArea())
        .alert("Couldn’t start online game",
               isPresented: Binding(get: { coordinator.onlineError != nil },
                                    set: { if !$0 { coordinator.onlineError = nil } })) {
            Button("OK", role: .cancel) { coordinator.onlineError = nil }
        } message: {
            Text(coordinator.onlineError ?? "")
        }
        .onOpenURL { coordinator.handle(url: $0) }
        .sheet(item: $coordinator.pendingReplay) { pending in
            NavigationStack { ReplayPlayerView(replay: pending.replay) }
                .preferredColorScheme(.dark)
        }
        #if DEBUG
        // Screenshot/UI-verification hook: FOOLISH_DEBUG_TABLE=<opponents> drops
        // straight into a live offline table so `simctl launch` can capture the
        // board without driving the menu. Not compiled into release builds.
        .task {
            let env = ProcessInfo.processInfo.environment
            let autoplay = env["FOOLISH_DEBUG_AUTOPLAY"] != nil

            if let raw = env["FOOLISH_DEBUG_TABLE"], coordinator.screen == .home {
                // Offline: drop into a table (optionally auto-playing to the win screen).
                let opponents = max(1, min(7, Int(raw) ?? 3))
                let stratName = env["FOOLISH_DEBUG_BOT"] ?? "random"
                let roster = EngineC.roster()
                let pick = roster.first(where: { $0.name == stratName }) ?? roster.first ?? (0, "random")
                let config = OfflineConfig(opponentStrategyId: pick.id, opponentName: pick.name, opponents: opponents)
                lastConfig = config
                coordinator.startOffline(config)
                if autoplay {
                    while !Task.isCancelled {
                        try? await Task.sleep(nanoseconds: 500_000_000)
                        guard let g = coordinator.offlineGame, g.foolSeat == nil else { break }
                        let legal = g.humanLegal
                        let shed = legal.filter { $0.type == .attack || $0.type == .cover }
                        if let mv = (shed.randomElement() ?? legal.randomElement()) { g.play(mv) }
                    }
                }
            } else if let mode = env["FOOLISH_DEBUG_ONLINE"], let user = env["FOOLISH_DEBUG_USER"] {
                // Online two-sim demo: auto sign-in, then host (create+start) or
                // guest (join). See AppCoordinator.debugHost/debugGuest.
                let pass = env["FOOLISH_DEBUG_PASS"] ?? "foolish-demo-pass"
                do { try await auth.signUp(username: user, password: pass) }
                catch { try? await auth.signIn(username: user, password: pass) }
                guard let uid = auth.userId else { return }
                if mode == "host" {
                    let wait = Double(env["FOOLISH_DEBUG_START_AFTER"] ?? "18") ?? 18
                    coordinator.debugHost(userId: uid, startAfter: wait, autoplay: autoplay)
                } else if mode == "guest", let gid = env["FOOLISH_DEBUG_GAME"] {
                    coordinator.debugGuest(userId: uid, gameId: gid, autoplay: autoplay)
                } else if mode == "dashboard" {
                    coordinator.screen = .dashboard
                }
            }
        }
        #endif
    }

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

    // Online table: same board (§16.D5 — one TableView), no offline rematch
    // config (rematch online is a fresh quick-match, wired when the create seam
    // lands). Leave returns home and tears down the feed.
    // The online session moves through lobby → table → win → (continue) lobby,
    // all driven by the authoritative feed (§16.D5). We pick the screen from the
    // game's status; Continue resets the same game back to its lobby.
    @ViewBuilder
    private func onlineTableStack(_ game: OnlineGame) -> some View {
        ZStack(alignment: .topLeading) {
            if let fool = game.foolSeat {
                TableView(game: game, onLeave: { confirmLeave = true })
                WinView(game: game, foolSeat: fool, humanSeat: game.humanSeat,
                        onRematch: { game.continueGame() },                 // → back to lobby
                        onHome: { game.leave(); coordinator.goHome() })
                    .transition(.opacity)
            } else if game.isWaiting {
                LobbyView(game: game, onLeave: { game.leave(); coordinator.goHome() })
                Button(action: { game.leave(); coordinator.goHome() }) {
                    Image(systemName: "xmark")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(FColor.textDim).padding(FSpace.m)
                }
                .accessibilityLabel(FStrings.t("leave"))
            } else {
                TableView(game: game, onLeave: { confirmLeave = true })
                Button(action: { confirmLeave = true }) {
                    Image(systemName: "xmark")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(FColor.textDim).padding(FSpace.m)
                }
                .accessibilityLabel(FStrings.t("leave"))
            }
        }
        .animation(FMotion.chrome, value: game.foolSeat)
        .confirmationDialog(FStrings.t("leave_game_title"), isPresented: $confirmLeave, titleVisibility: .visible) {
            Button(FStrings.t("leave"), role: .destructive) { game.leave(); coordinator.goHome() }
            Button(FStrings.t("cancel"), role: .cancel) {}
        } message: {
            Text(FStrings.t("leave_game_body"))
        }
    }
}
