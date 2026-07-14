// AuthView.swift — username+password sign-in / sign-up (§9, §16.D2). Guest-first:
// the app never walls play behind this; it appears only when the user chooses an
// online action that needs an account. Uses AuthService (the tested nameToEmail
// derivation + reserved-prefix rule under the hood).

import SwiftUI
import FoolishKit

struct AuthView: View {
    @EnvironmentObject private var auth: AuthService
    @Environment(\.dismiss) private var dismiss
    let onSignedIn: () -> Void

    @State private var name = ""
    @State private var password = ""
    @State private var mode: Mode = .signIn
    @State private var working = false
    @State private var error: String?

    enum Mode { case signIn, signUp }

    var body: some View {
        NavigationStack {
            VStack(spacing: FSpace.l) {
                Picker("", selection: $mode) {
                    Text("Sign in").tag(Mode.signIn)
                    Text("Create account").tag(Mode.signUp)
                }
                .pickerStyle(.segmented)

                TextField("Username", text: $name)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .textFieldStyle(.roundedBorder)
                SecureField("Password", text: $password)
                    .textFieldStyle(.roundedBorder)

                FButton(mode == .signIn ? "Sign in" : "Create account", enabled: canSubmit) {
                    Task { await submit() }
                }
                if let error {
                    Text(error).font(FType.body(13)).foregroundColor(FColor.accent)
                }
                Spacer()
            }
            .padding(FSpace.xl)
            .background(FColor.table.ignoresSafeArea())
            .navigationTitle("Play online")
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button(FStrings.t("cancel")) { dismiss() }.tint(FColor.accent)
                }
            }
        }
        .preferredColorScheme(.dark)
    }

    private var canSubmit: Bool { !name.isEmpty && password.count >= 6 && !working }

    private func submit() async {
        working = true
        error = nil
        do {
            if mode == .signIn { try await auth.signIn(username: name, password: password) }
            else { try await auth.signUp(username: name, password: password) }
            working = false
            onSignedIn()
            dismiss()
        } catch {
            self.error = error.localizedDescription
            working = false
        }
    }
}
