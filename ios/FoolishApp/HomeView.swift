// HomeView.swift — §6 screen 1. Big PLAY (online — not wired until M-D), OFFLINE
// (the working path: a bot picker cycling the §7.2 roster + opponent count),
// join-by-code and resume are placed as seams, footer: Replays · Tutorial ·
// Settings. Cold-starts with zero network (§6 flow notes). One primary action
// per screen (§5.5): OFFLINE's Start.

import SwiftUI
import FoolishKit

struct HomeView: View {
    let onStartOffline: (OfflineConfig) -> Void
    /// Called once the user is signed in and wants to quick-match online.
    let onQuickMatch: () -> Void

    @EnvironmentObject private var auth: AuthService
    @State private var showAuth = false
    // The offline difficulty ladder: the 7 cities (Miami → Moscow), resolved to
    // their strategy ids from the kernel roster (BotNames.ladder, §17.10).
    @State private var roster: [(id: Int, name: String)] = {
        let full = EngineC.roster()
        return BotNames.ladder.compactMap { key in full.first(where: { $0.name == key }) }
    }()
    @State private var opponentIndex = 0
    @State private var opponentCount = 1
    @State private var toast: String?
    @State private var showGallery = false
    @State private var showReplays = false
    @State private var showSettings = false
    @State private var showTutorial = false

    private var currentBot: (id: Int, name: String) {
        roster.isEmpty ? (0, "cordite") : roster[opponentIndex % roster.count]
    }

    var body: some View {
        ZStack {
            WoolBackground()
            VStack(spacing: FSpace.xl) {
                header
                onlineButton
                offlineCard
                Spacer()
                footer
            }
            .padding(FSpace.xl)
        }
        .fToast($toast)
        #if DEBUG
        .fullScreenCover(isPresented: $showGallery) {
            ZStack(alignment: .topTrailing) {
                GalleryView()
                Button("Close") { showGallery = false }
                    .padding(FSpace.l).foregroundColor(FColor.accent)
            }
        }
        #endif
    }

    private var header: some View {
        VStack(spacing: FSpace.xs) {
            Text("FOOLISH")
                .font(FType.numeral(52))
                .foregroundColor(FColor.textPrimary)
            Text("ДУРАК")
                .font(FType.title(16))
                .foregroundColor(FColor.accent)
                .tracking(6)
        }
        .padding(.top, FSpace.xxl)
        .accessibilityElement(children: .combine)
        #if DEBUG
        // Long-press the wordmark to open the component gallery (§16.A6).
        .onLongPressGesture { showGallery = true }
        #endif
    }

    private var onlineButton: some View {
        FButton(FStrings.t("play"), kind: .secondary) { handlePlay() }
            .sheet(isPresented: $showAuth) {
                AuthView(onSignedIn: { onQuickMatch() })
            }
    }

    private func handlePlay() {
        guard Backend.shared.isConfigured else {
            toast = FStrings.t("ios.online_soon")   // offline build: no backend config
            return
        }
        if auth.isSignedIn { onQuickMatch() } else { showAuth = true }
    }

    private var offlineCard: some View {
        VStack(spacing: FSpace.l) {
            Text(FStrings.t("choose_opponent"))
                .font(FType.body(14))
                .foregroundColor(FColor.textDim)

            // Left/right cycle over the roster (web precedent: commit 7f22749).
            HStack {
                cycleButton("chevron.left") {
                    opponentIndex = (opponentIndex + roster.count - 1) % max(roster.count, 1)
                }
                Spacer()
                VStack(spacing: FSpace.xs) {
                    Text(BotNames.display(strategy: currentBot.name))
                        .font(FType.title(24))
                        .foregroundColor(FColor.textPrimary)
                        .lineLimit(1)
                        .minimumScaleFactor(0.6)
                    // The strength ladder as a place: "1,420 km from Moscow".
                    if let flavor = BotNames.flavorLine(strategy: currentBot.name) {
                        Text(flavor)
                            .font(FType.body(12))
                            .foregroundColor(FColor.win)
                            .lineLimit(1)
                            .minimumScaleFactor(0.6)
                    }
                }
                Spacer()
                cycleButton("chevron.right") {
                    opponentIndex = (opponentIndex + 1) % max(roster.count, 1)
                }
            }

            Stepper(value: $opponentCount, in: 1...3) {
                Text("\(FStrings.t("players")): \(opponentCount + 1)")
                    .font(FType.body(14)).foregroundColor(FColor.textPrimary)
            }
            .tint(FColor.accent)

            FButton(FStrings.t("start_game"), kind: .primary) {
                onStartOffline(OfflineConfig(
                    opponentStrategyId: currentBot.id,
                    opponentName: currentBot.name,
                    opponents: opponentCount
                ))
            }
        }
        .padding(FSpace.l)
        .background(FColor.surface)
        .clipShape(RoundedRectangle(cornerRadius: FRadius.sheet))
    }

    private func cycleButton(_ system: String, _ action: @escaping () -> Void) -> some View {
        Button(action: { Haptics.fire(.pickUp); action() }) {
            Image(systemName: system)
                .font(.system(size: 20, weight: .semibold))
                .foregroundColor(FColor.textPrimary)
                .frame(width: 44, height: 44)
        }
    }

    private var footer: some View {
        HStack(spacing: FSpace.xl) {
            footerLink("replays", "rectangle.stack") { showReplays = true }
            footerLink("tutorial", "graduationcap") { showTutorial = true }
            footerLink("settings", "gearshape") { showSettings = true }
        }
        .padding(.bottom, FSpace.m)
        .sheet(isPresented: $showReplays) { ReplaysView() }
        .sheet(isPresented: $showSettings) { SettingsView() }
        .fullScreenCover(isPresented: $showTutorial) { TutorialView() }
    }

    private func footerLink(_ key: String, _ system: String, _ action: @escaping () -> Void) -> some View {
        // Replays (§16.C), Tutorial (§16.B6) and Settings (§16.E3) open their screens.
        Button(action: action) {
            VStack(spacing: FSpace.xs) {
                Image(systemName: system).font(.system(size: 18))
                Text(FStrings.t(key)).font(FType.body(12))
            }
            .foregroundColor(FColor.textDim)
        }
    }
}
