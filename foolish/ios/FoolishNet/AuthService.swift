// AuthService.swift - username and password accounts (§9, §16.D2).
//
// The server derives the account from the upper-cased username (the address
// both clients used to work out for themselves), refuses a name with the
// bot-reserved '%', and rotates the tokens; the kernel's client keeps the
// session. This file upper-cases the name, keeps the session blob in the
// Keychain so a player stays signed in across launches, and publishes who is
// signed in.

import Foundation
import Combine   // ObservableObject / @Published
import FoolishKit

@MainActor
public final class AuthService: ObservableObject {
    @Published public private(set) var userId: UUID?
    @Published public private(set) var username: String?

    public init() {}

    public var isSignedIn: Bool { userId != nil }

    /// Restore the session kept from a previous launch.
    public func restore() async {
        if let blob = Session.load(), NetClient.load(blob) { apply() }
    }

    public func signIn(username name: String, password: String) async throws {
        try await sign { NetClient.signIn(name.uppercased(), password) }
    }

    public func signUp(username name: String, password: String) async throws {
        try await sign { NetClient.signUp(name.uppercased(), password) }
    }

    private func sign(_ build: () -> NcRequestSnap?) async throws {
        let (status, body) = try await Backend.shared.call(build)
        try Backend.shared.keep(NetClient.readSession(status, body))
        apply()
    }

    /// Sign out everywhere; locally whatever the server answered.
    public func signOut() async {
        _ = try? await Backend.shared.call { NetClient.signOut() }
        signedOut()
    }

    /// Forget the session on this device only (the account is gone already).
    func signedOut() {
        Stream.shared.stop()
        NetClient.reset()
        Session.save(nil)
        userId = nil
        username = nil
    }

    private func apply() {
        guard let s = NetClient.session, s.signedIn != 0 else { return }
        userId = UUID(uuidString: s.userId)
        username = s.username
    }
}
