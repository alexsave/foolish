// Auth.swift — the username→email derivation and reserved-name rule (§9,
// §16.D2). A byte-for-byte port of the web's AuthContext.nameToEmail and
// botName.usernameUsesReservedPrefix so the iOS app and the site derive the
// SAME Supabase identity for a given username. The actual sign-in/up calls
// (supabase-swift) land in Milestone D; this pure layer is testable now.
//
// The app must not invent its own scheme (§9) — this mirrors the site exactly.
// When the real-email auth rebuild ships (Oracle doc §4), verify/reset flows are
// added here; leave those seams.

import Foundation
import CryptoKit
import FoolishKit

public enum Auth {
    /// The account email domain (web `WEBSITE_DOMAIN`).
    public static let websiteDomain = "foolish.cards"

    /// Bot-reserved username prefix. Humans may not use it ANYWHERE in the name
    /// (the web uses `.includes`, and a DB trigger enforces it server-side).
    public static let botPrefix = "%"

    /// nameToEmail: SHA-256 of the UPPERCASED UTF-8 name → first 16 hex chars →
    /// `<hex>@foolish.cards` (AuthContext.tsx:12-33).
    public static func nameToEmail(_ name: String) -> String {
        let digest = SHA256.hash(data: Data(name.uppercased().utf8))
        let hex = digest.map { String(format: "%02x", $0) }.joined()
        return "\(hex.prefix(16))@\(websiteDomain)"
    }

    /// True if `name` uses the bot-reserved prefix anywhere — must be rejected
    /// locally before sign-up (botName.ts `usernameUsesReservedPrefix`).
    public static func usernameUsesReservedPrefix(_ name: String) -> Bool {
        !name.isEmpty && name.contains(botPrefix)
    }

    /// The `user_metadata.username` value the web signUp carries (uppercased).
    public static func signUpUsername(_ name: String) -> String { name.uppercased() }
}
