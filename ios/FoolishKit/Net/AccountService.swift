// AccountService.swift — account deletion (§9, §16.E3). Calls the `delete-account`
// edge function (this repo's server/impls/supabase/functions/delete-account), which scrubs
// shared-history PII and deletes the auth user, then signs out locally. This is
// the in-app path Apple Guideline 5.1.1(v) requires; the standalone web page
// (src/app/delete-account) is the Play-required app-independent path.
//
// NOTE (Mac compile pass): confirm the functions.invoke signature against the
// resolved supabase-swift version (2.x: functions.invoke(_:options:)).

import Foundation
import Supabase

@MainActor
public final class AccountService {
    private let auth: AuthService
    public init(auth: AuthService) { self.auth = auth }

    /// Delete the signed-in account. Throws if not configured / not signed in /
    /// the server rejects; the caller shows the error and keeps the user signed
    /// in on failure (never a silent partial delete).
    public func deleteAccount() async throws {
        guard let client = Backend.shared.client else { throw ServiceError.notConfigured }
        guard auth.isSignedIn else { throw ServiceError.notSignedIn }
        // POST with the caller's bearer token attached by the client.
        _ = try await client.functions.invoke(
            "delete-account",
            options: FunctionInvokeOptions(method: .post)
        )
        await auth.signOut()
    }

    public enum ServiceError: Error, LocalizedError {
        case notConfigured, notSignedIn
        public var errorDescription: String? {
            switch self {
            case .notConfigured: return "Online play isn’t configured in this build."
            case .notSignedIn: return "You’re not signed in."
            }
        }
    }
}
