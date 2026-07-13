// HomeView.swift — §6 screen 1. Big PLAY (online — not wired until M-D), OFFLINE
// (the working path: a bot picker cycling the §7.2 roster + opponent count),
// join-by-code and resume are placed as seams, footer: Replays · Tutorial ·
// Settings. Cold-starts with zero network (§6 flow notes). One primary action
// per screen (§5.5): OFFLINE's Start.

import SwiftUI
import FoolishKit

struct HomeView: View {
    let onStartOffline: (OfflineConfig) -> Void

    @State private var roster = EngineC.roster()
    @State private var opponentIndex = 0
    @State private var opponentCount = 1
    @State private var toast: String?
    @State private var showGallery = false

    private var currentBot: (id: Int, name: String) {
        roster.isEmpty ? (0, "cordite") : roster[opponentIndex % roster.count]
    }

    var body: some View {
        ZStack {
            FColor.table.ignoresSafeArea()
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
        FButton(FStrings.t("play"), kind: .secondary) {
            toast = FStrings.t("ios.online_soon")   // wired in Milestone D
        }
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
            footerLink("replays", "rectangle.stack")
            footerLink("tutorial", "graduationcap")
            footerLink("settings", "gearshape")
        }
        .padding(.bottom, FSpace.m)
    }

    private func footerLink(_ key: String, _ system: String) -> some View {
        // Destinations land in M-B (tutorial) / M-C (replays) / M-E (settings);
        // placed here now as the Home footer per §6.
        Button(action: { toast = FStrings.t("ios.online_soon") }) {
            VStack(spacing: FSpace.xs) {
                Image(systemName: system).font(.system(size: 18))
                Text(FStrings.t(key)).font(FType.body(12))
            }
            .foregroundColor(FColor.textDim)
        }
    }
}
