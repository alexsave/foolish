// DashboardView.swift — the online hub (web src/components/Dashboard.tsx): the
// signed-in landing page. Create a game (→ lobby), join one by code (→ lobby),
// or sign out. The web also lists your active games + leaderboard/history; those
// are follow-ups (§17.7) — create/join is the core loop the owner called out.

import SwiftUI
import FoolishKit

struct DashboardView: View {
    @EnvironmentObject private var auth: AuthService
    let onCreate: () -> Void
    let onJoin: (String) -> Void
    let onClose: () -> Void

    @State private var code = ""
    @State private var showSettings = false

    var body: some View {
        ZStack {
            WoolBackground()
            VStack(spacing: FSpace.l) {
                topBar
                header
                FButton(FStrings.t("ios.create_game"), kind: .primary) { onCreate() }
                joinRow
                Spacer()
            }
            .padding(FSpace.xl)
        }
        .sheet(isPresented: $showSettings) { SettingsView() }
    }

    /// Back (left) and Settings (right) — Settings holds Sign out (owner: it
    /// belongs there, not on the dashboard).
    private var topBar: some View {
        HStack {
            Button(action: onClose) {
                Image(systemName: "chevron.left")
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundColor(FColor.textPrimary)
            }
            .accessibilityLabel(FStrings.t("home"))
            Spacer()
            Button(action: { showSettings = true }) {
                Image(systemName: "gearshape")
                    .font(.system(size: 18))
                    .foregroundColor(FColor.textPrimary)
            }
            .accessibilityLabel(FStrings.t("settings"))
        }
    }

    private var header: some View {
        VStack(spacing: FSpace.xs) {
            Text(FStrings.t("ios.dashboard"))
                .font(FType.numeral(34)).foregroundColor(FColor.textPrimary)
            if let name = auth.username {
                Text(name).font(FType.body(15)).foregroundColor(FColor.textDim)
            }
        }
        .padding(.top, FSpace.xxl)
    }

    private var joinRow: some View {
        HStack(spacing: FSpace.s) {
            TextField(FStrings.t("ios.game_code"), text: $code)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .padding(FSpace.m)
                .background(FColor.surface)
                .clipShape(RoundedRectangle(cornerRadius: FRadius.card))
                .foregroundColor(FColor.textPrimary)
            FButton(FStrings.t("join_game"), kind: .wood, enabled: !code.trimmingCharacters(in: .whitespaces).isEmpty) {
                onJoin(code.trimmingCharacters(in: .whitespacesAndNewlines))
            }
            .frame(width: 110)
        }
    }
}
