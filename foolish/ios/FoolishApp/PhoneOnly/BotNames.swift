// BotNames.swift - this app's bot names (docs/IOS_BOT_NAMING.md). The brains
// keep their explosive strategy keys (C keys, the offline roster); what a person
// sees is a world city, so the App-Store age-rating questionnaire stays boring
// and the strength ladder reads as a journey home (Miami -> ... -> Moscow).
//
// Online and in replays the city is already in the STORED name ('%Vienna 1'),
// so displayNickname only drops the bot prefix. Offline seats are named from a
// strategy key, and THOSE CITIES ARE NOT HERE: they are `bot.<strategy_key>` in
// c/i18n, so the map is the product's, not this platform's.
//
// Every surface (table, picker, win line, lobby) calls these and never
// re-derives a name. Strategy keys are treated as opaque KEYS and localized
// through FStrings; a STORED nickname is shown as stored, minus the bot prefix
// (displayNickname). Nothing here may leak back into a stored or server-bound
// field (§2 "never reverse-map").

// MOVED OUT OF FoolishKit (bundle size). FoolishKit is a DYNAMIC framework that
// ships inside FoolishMessagesApp, and every `public` symbol in a dylib is an
// export - which makes it a dead-strip ROOT the linker may never remove. So any
// public API in FoolishKit that only the PHONE app calls is bytes on every
// iMessage install for code that install can never run. This file had no caller
// outside ios/FoolishApp, so it lives with its caller now.

import Foundation
import FoolishKit

public enum BotNames {

    /// Roster strategy key (`EngineC.roster()` name, e.g. "octogen") → localized
    /// display name. Unknown keys degrade to `.capitalized`.
    public static func display(strategy key: String) -> String {
        let lookup = "bot.\(key)"
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
        if d == 0 { return FStrings.t("bot.km0") }
        let fmt = NumberFormatter()
        fmt.numberStyle = .decimal   // localized grouping separator
        let n = fmt.string(from: NSNumber(value: d)) ?? String(d)
        return FStrings.t("bot.km", ["km": n])
    }

    /// The reserved first character of every bot's STORED name (bots.nickname,
    /// a replay blob's seat name): '%Vienna 1'. Humans may not use it anywhere in
    /// theirs (server/impls/supabase/seed.sql enforces it), which is what makes
    /// "starts with it" a sound bot test. The web's twin is BOT_USERNAME_PREFIX
    /// in src/common/botName.ts; the kernel has no rule for it.
    static let botPrefix = "%"

    /// A stored name belongs to a bot iff it carries the prefix.
    public static func isBotName(_ name: String) -> Bool { name.hasPrefix(botPrefix) }

    /// A stored seat name as a person reads it: a bot's loses the prefix (its
    /// seat carries the bot icon instead), anyone else's is untouched. The web's
    /// botDisplayName, rule for rule.
    ///
    /// NO MAPPING, by design (owner, 2026-09-22): the roster is named after
    /// cities where it is STORED, so '%Vienna 1' shows as "Vienna 1", and an old
    /// replay that says '%Octogen 1' shows "Octogen 1", because that is the bot
    /// that played it. The cost is that a stored name is not localized. The
    /// `bot.*` strings stay for the offline picker, which names a seat from its
    /// strategy key (`display(strategy:)`), not from a stored row.
    public static func displayNickname(_ raw: String) -> String {
        isBotName(raw) ? String(raw.dropFirst(botPrefix.count)) : raw
    }
}
