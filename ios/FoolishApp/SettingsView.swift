// SettingsView.swift — §6 screen 6 / §16.E3. Language override, haptics toggle,
// account block (with sign-out + DELETE ACCOUNT seams), licenses, links, and a
// DEBUG-only feature-flag list (§16.E2). Account actions are seams until auth
// lands (M-D) and the deletion endpoint exists (§9, §16.E3) — they must not be
// faked (no mailto); they're disabled with an explanatory note until wired.

import SwiftUI
import FoolishKit

struct SettingsView: View {
    @Environment(\.dismiss) private var dismiss
    @AppStorage("ios.haptics") private var hapticsOn = true
    @State private var language = FStrings.override
    @State private var flagRefresh = false

    var body: some View {
        NavigationStack {
            List {
                languageSection
                hapticsSection
                accountSection
                aboutSection
                #if DEBUG
                flagsSection
                #endif
            }
            .scrollContentBackground(.hidden)
            .background(FColor.table.ignoresSafeArea())
            .navigationTitle(FStrings.t("settings"))
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button(FStrings.t("home")) { dismiss() }.tint(FColor.accent)
                }
            }
        }
        .preferredColorScheme(.dark)
    }

    private var languageSection: some View {
        Section("Language") {
            Picker("Language", selection: $language) {
                ForEach(AppLanguage.allCases, id: \.self) { lang in
                    Text(lang.display).tag(lang)
                }
            }
            .onChange(of: language) { FStrings.override = $0 }
        }
    }

    private var hapticsSection: some View {
        Section {
            Toggle("Haptics", isOn: $hapticsOn)
                .onChange(of: hapticsOn) { Haptics.isEnabled = $0 }
                .tint(FColor.accent)
        }
    }

    private var accountSection: some View {
        Section("Account") {
            // Auth lands in M-D; these are the reserved slots (§9 email/verify/
            // reset stubbed from day one). Disabled until wired — never faked.
            Text("Guest").foregroundColor(FColor.textDim)
            Button("Sign out") {}.disabled(true).foregroundColor(FColor.textDim)
            // 5.1.1(v): in-app deletion is mandatory. Calls the deletion edge
            // function once it exists (§16.E3) — blocks submission until then.
            Button(role: .destructive) {} label: { Text("Delete account") }
                .disabled(true)
            Text("Account features arrive with online play.")
                .font(FType.body(12)).foregroundColor(FColor.textDim)
        }
    }

    private var aboutSection: some View {
        Section("About") {
            Link("Privacy Policy", destination: URL(string: "https://foolish.cards/privacy")!)
            Link("Terms of Service", destination: URL(string: "https://foolish.cards/terms")!)
            NavigationLink("Licenses") { LicensesView() }
            HStack {
                Text("Version")
                Spacer()
                Text(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.0")
                    .foregroundColor(FColor.textDim)
            }
        }
    }

    #if DEBUG
    private var flagsSection: some View {
        Section("Feature flags (debug)") {
            ForEach(Flags.all, id: \.name) { flag in
                Toggle(flag.name, isOn: Binding(
                    get: { Flags.overrides[flag.name] ?? flag.value },
                    set: { Flags.overrides[flag.name] = $0; flagRefresh.toggle() }
                )).tint(FColor.accent)
            }
        }
    }
    #endif
}

/// A single static licenses screen (§16.E3).
struct LicensesView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FSpace.l) {
                Text("Open-source licenses")
                    .font(FType.title(18)).foregroundColor(FColor.textPrimary)
                Text("""
                Foolish — Durak. The game engine is compiled from the project's C \
                sources (cnitro). Third-party Swift packages:

                • swift-snapshot-testing (MIT) — test-only, not shipped in the app.

                The full license texts are bundled with the source repository.
                """)
                .font(FType.body(14)).foregroundColor(FColor.textDim)
            }
            .padding(FSpace.xl)
        }
        .background(FColor.table.ignoresSafeArea())
        .navigationTitle("Licenses")
    }
}
