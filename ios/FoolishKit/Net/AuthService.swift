// AuthService.swift — username+password auth over supabase-swift, mirroring the
// web AuthContext exactly (§9, §16.D2). The username→email derivation and the
// reserved-prefix rule are the pure, tested `Auth` port; this wraps the Supabase
// calls around them. supabase-swift persists the session in the Keychain by
// default, so guests stay signed in across launches.
//
// NOTE (Mac compile pass): supabase-swift auth API (signIn(email:password:),
// signUp(email:password:data:), signOut(), session/currentUser) is stable in
// 2.x; confirm the exact member names against the resolved version.

import Foundation
import Combine   // ObservableObject / @Published
import Supabase

@MainActor
public final class AuthService: ObservableObject {
    @Published public private(set) var userId: UUID?
    @Published public private(set) var username: String?

    public init() {}

    public var isSignedIn: Bool { userId != nil }
    private var client: SupabaseClient? { Backend.shared.client }

    /// Restore any persisted session on launch.
    public func restore() async {
        guard let client else { return }
        if let session = try? await client.auth.session {
            apply(user: session.user)
        }
    }

    public func signIn(username name: String, password: String) async throws {
        guard let client else { throw AuthError.notConfigured }
        let email = Auth.nameToEmail(name)
        let session = try await client.auth.signIn(email: email, password: password)
        apply(user: session.user)
    }

    public func signUp(username name: String, password: String) async throws {
        guard let client else { throw AuthError.notConfigured }
        // Local guard mirroring the web + the DB trigger: humans may not use the
        // bot-reserved prefix anywhere in the name.
        if Auth.usernameUsesReservedPrefix(name) { throw AuthError.reservedName }
        let email = Auth.nameToEmail(name)
        let response = try await client.auth.signUp(
            email: email,
            password: password,
            data: ["username": .string(Auth.signUpUsername(name))]
        )
        apply(user: response.user)
    }

    public func signOut() async {
        try? await client?.auth.signOut()
        userId = nil
        username = nil
    }

    private func apply(user: User) {
        userId = user.id
        username = user.userMetadata["username"]?.stringValue ?? username
    }

    public enum AuthError: Error, LocalizedError {
        case notConfigured, reservedName
        public var errorDescription: String? {
            switch self {
            case .notConfigured: return "Online play isn’t configured in this build."
            case .reservedName: return "That username isn’t allowed."
            }
        }
    }
}
