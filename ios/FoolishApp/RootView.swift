// RootView.swift — switches between Home and the live Table, and floats the Win
// screen over a finished game. Owns the "leave a live game" confirmation (the
// only destructive confirm in the app, §5.5).

import SwiftUI
import FoolishKit

struct RootView: View {
    @StateObject private var coordinator = AppCoordinator()
    @State private var lastConfig: OfflineConfig?
    @State private var confirmLeave = false

    var body: some View {
        ZStack {
            switch coordinator.screen {
            case .home:
                HomeView(onStartOffline: { config in
                    lastConfig = config
                    coordinator.startOffline(config)
                })
            case .table:
                if let game = coordinator.offlineGame {
                    tableStack(game)
                }
            }
        }
        .background(FColor.table.ignoresSafeArea())
        .onOpenURL { coordinator.handle(url: $0) }
        .sheet(item: $coordinator.pendingReplay) { pending in
            NavigationStack { ReplayPlayerView(replay: pending.replay) }
                .preferredColorScheme(.dark)
        }
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
}
