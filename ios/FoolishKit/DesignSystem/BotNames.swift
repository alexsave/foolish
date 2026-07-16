// BotNames.swift — the iOS-only "road to Moscow" display map (docs/IOS_BOT_NAMING.md).
// The bot roster is named after explosives everywhere it is STORED (C keys, DB
// nicknames, replay blobs, the wire) and renamed to Russian cities only at
// RENDER time here, so the App-Store age-rating questionnaire stays boring and
// the strength ladder reads as a journey home (Miami → … → Moscow).
//
// Three entry points; every surface (table, picker, win line, future watch /
// iMessage) calls these and never re-derives a name. Strategy keys are treated
// as opaque KEYS and localized through FStrings — nothing here may leak back
// into a stored or server-bound field (§2 "never reverse-map").

import Foundation

public enum BotNames {

    /// Roster strategy key (`EngineC.roster()` name, e.g. "octogen") → localized
    /// display name. Unknown keys degrade to `.capitalized`.
    public static func display(strategy key: String) -> String {
        let lookup = "ios.bot.\(key)"
        let s = FStrings.t(lookup)
        return s == lookup ? key.capitalized : s
    }

    /// The offline difficulty ladder: 7 tiers, weakest → strongest, each a world
    /// city on the road to Moscow (owner decision 2026-07-16). The picker shows
    /// exactly these; the 3 intermediate strategies (simple_heuristic, espresso,
    /// gunpowder) are not surfaced. Values are strategy keys (EngineC.roster()).
    public static let ladder: [String] = [
        "random",       // Miami
        "handwritten",  // New York
        "robusta",      // Seoul
        "firecracker",  // Madrid
        "blackpowder",  // Vienna
        "cordite",      // St. Petersburg
        "octogen",      // Moscow
    ]

    /// Air distance to the Kremlin per rung. Drives the picker flavor line;
    /// strictly monotonic so it doubles as the strength order.
    private static let km: [String: Int] = [
        "random": 9_600, "handwritten": 7_500, "robusta": 6_600,
        "firecracker": 3_440, "blackpowder": 1_660, "cordite": 635, "octogen": 0,
    ]

    /// Localized picker caption — "1,420 km from Moscow", or "The Kremlin
    /// itself" for Moscow. `nil` for keys with no rung (humans, unknowns).
    public static func flavorLine(strategy key: String) -> String? {
        guard let d = km[key] else { return nil }
        if d == 0 { return FStrings.t("ios.bot.km0") }
        let fmt = NumberFormatter()
        fmt.numberStyle = .decimal   // localized grouping separator
        let n = fmt.string(from: NSNumber(value: d)) ?? String(d)
        return FStrings.t("ios.bot.km", ["km": n])
    }

    /// Website / replay nicknames: `"% <Base> [Max] [<n>]"` → localized,
    /// stripping the storage-only `%` and keeping any Max tier + instance number
    /// ("% Octogen Max 2" → ru "Москва Макс 2"). Humans and the `% 0x…` easter
    /// egg pass through unchanged. Never feed the result back to the server (§2).
    public static func displayNickname(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        guard trimmed.hasPrefix("%") else { return raw }   // human name — untouched

        var body = String(trimmed.dropFirst()).trimmingCharacters(in: .whitespaces)
        // The hex easter egg is culture-neutral and beloved — leave it verbatim.
        if body.lowercased().hasPrefix("0x") { return raw }

        // Peel a trailing instance number, then a trailing "Max" tier.
        var suffix = ""
        var parts = body.split(separator: " ").map(String.init)
        if let last = parts.last, Int(last) != nil {
            suffix = " " + last
            parts.removeLast()
        }
        var isMax = false
        if let last = parts.last, last.caseInsensitiveCompare("Max") == .orderedSame {
            isMax = true
            parts.removeLast()
        }
        body = parts.joined(separator: " ")

        guard let key = baseToKey[body.lowercased()] else { return raw }  // unknown base
        var out = display(strategy: key)
        if isMax { out += " " + FStrings.t("ios.bot.max") }
        return out + suffix
    }

    /// Website base nickname (lowercased) → strategy key. Keeps the dropped
    /// families (Espresso/Robusta/Gunpowder) for historical replay blobs (§2).
    private static let baseToKey: [String: String] = [
        "random": "random",
        "simple heuristic": "simple_heuristic",
        "handwritten": "handwritten",
        "espresso": "espresso",
        "robusta": "robusta",
        "firecracker": "firecracker",
        "gunpowder": "gunpowder",
        "blackpowder": "blackpowder",
        "cordite": "cordite",
        "octogen": "octogen",
    ]
}
