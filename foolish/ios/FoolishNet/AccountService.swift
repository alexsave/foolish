// AccountService.swift - account deletion (§9, §16.E3), the in-app path Apple
// Guideline 5.1.1(v) requires. The server redacts the player from every game
// and replay and deletes the account; then this device forgets the session.
// A refusal keeps the player signed in (never a silent partial delete).

import Foundation
import FoolishKit

@MainActor
public final class AccountService {
    private let auth: AuthService
    public init(auth: AuthService) { self.auth = auth }

    public func deleteAccount() async throws {
        _ = try await Backend.shared.ask(NC_DO_DELETE_ACCOUNT)
        auth.signedOut()
    }
}
