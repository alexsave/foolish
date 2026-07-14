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
    /// Called once the user is signed in and wants to join a game by code.
    let onJoin: (String) -> Void

    /// What to run after an auth sheet completes (quick-match vs join-by-code).
    private enum OnlineIntent: Equatable { case quickMatch, join(String) }

    @EnvironmentObject private var auth: AuthService
    @State private var showAuth = false
    @State private var pendingOnline: OnlineIntent?
    @State private var joinCode = ""
    @State private var roster = EngineC.roster()
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
                onlineSection
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

    private var onlineSection: some View {
        VStack(spacing: FSpace.m) {
            FButton(FStrings.t("play"), kind: .secondary) { begin(.quickMatch) }
            // Join by code: paste/type a game code (or the tail of a shared link).
            HStack(spacing: FSpace.s) {
                TextField(FStrings.t("join_code_prompt"), text: $joinCode)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .font(FType.body(15))
                    .foregroundColor(FColor.textPrimary)
                    .padding(.horizontal, FSpace.m)
                    .frame(height: 44)
                    .background(FColor.surface)
                    .clipShape(RoundedRectangle(cornerRadius: FRadius.button, style: .continuous))
                Button(FStrings.t("join")) {
                    let code = joinCode.trimmingCharacters(in: .whitespacesAndNewlines)
                    guard !code.isEmpty else { return }
                    begin(.join(code))
                }
                .font(FType.title(15))
                .foregroundColor(FColor.textPrimary)
                .padding(.horizontal, FSpace.l)
                .frame(height: 44)
                .background(WoodSurface(seed: 0.7, cornerRadius: FRadius.button))
                .disabled(joinCode.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
        }
        .sheet(isPresented: $showAuth) {
            AuthView(onSignedIn: { resolvePendingOnline() })
        }
    }

    /// Gate an online action on config + auth; run now if signed in, else prompt.
    private func begin(_ intent: OnlineIntent) {
        guard Backend.shared.isConfigured else {
            toast = FStrings.t("ios.online_soon")   // offline build: no backend config
            return
        }
        pendingOnline = intent
        if auth.isSignedIn { resolvePendingOnline() } else { showAuth = true }
    }

    private func resolvePendingOnline() {
        switch pendingOnline {
        case .quickMatch: onQuickMatch()
        case .join(let code): onJoin(code)
        case .none: break
        }
        pendingOnline = nil
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
                    Text(currentBot.name.capitalized)
                        .font(FType.title(24))
                        .foregroundColor(FColor.textPrimary)
                    Text("\(opponentCount) \(FStrings.t("players"))")
                        .font(FType.body(13))
                        .foregroundColor(FColor.textDim)
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
