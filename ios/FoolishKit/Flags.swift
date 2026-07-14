// Flags.swift — compile-time feature flags with a DEBUG override overlay (§10.2).
// v1 ships every billing/Oracle surface OFF: the code compiles (so the seams
// exist) but is invisible. Flipping these later is a leaf-node change — no
// architectural rework (§10). Anti-goal (§10.5): no price strings, product ids,
// StoreKit imports, or "coming soon" teasers anywhere while these are off.

import Foundation

public enum Flags {
    /// Show the Infinite Oracle UI (Replay screen button, Settings row). OFF in v1.
    public static var oracleUI: Bool { overrides["oracleUI"] ?? false }
    /// Show any paywall. OFF in v1.
    public static var paywallUI: Bool { overrides["paywallUI"] ?? false }
    /// The US web-upsell link-out lever (Oracle doc §5). OFF in v1, decided later.
    public static var webUpsellLink: Bool { overrides["webUpsellLink"] ?? false }

    /// All flags, for the DEBUG settings overlay (§16.E2).
    public static var all: [(name: String, value: Bool)] {
        [("oracleUI", oracleUI), ("paywallUI", paywallUI), ("webUpsellLink", webUpsellLink)]
    }

    // DEBUG-only in-memory overrides, toggled from the Settings flag list. In
    // release builds `overrides` is always empty, so the compile-time defaults
    // above are the only values (nothing user-flippable ships).
    #if DEBUG
    public static var overrides: [String: Bool] = [:]
    #else
    public static let overrides: [String: Bool] = [:]
    #endif
}
