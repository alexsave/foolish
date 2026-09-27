// FStrings.swift - localization access (§5.5: all text through the table from
// day one). Twenty-five languages, in one in-code table: en, ru, ko, zh, vi, es,
// pt, fr, de, it, ja, pl, uk, tr, id, th, nl, sv, da, no, fi, cs, ro, he, ar.
// The last two are right to left; see AppLanguage.isRTL for what that does and,
// more to the point, what it deliberately does not do to the board.
//
// WHICH ONE A PLAYER GETS IS THE PHONE'S CALL BY DEFAULT: the app reads the
// phone's own ordered language preference. See `FStrings.active` for the
// reasoning and `match` for the resolution.
//
// The phone app's Settings screen still offers the full list to disagree with
// that (owner) - Settings is where a player goes looking for a setting. The
// iMessage board's gear sheet does NOT: a drawer that size has no room to spend
// fifteen rows on a question the phone already answered, and a player who wants
// a different one has the app.
//
// THE TABLE IS NOT IN THIS FILE ANY MORE. It is C - c/i18n/strings_<code>.c,
// one file per language, keyed by c/i18n/keys.h - and tools/datagen writes this
// side and the website's from the same source at build time. The API
// (`FStrings.t`) is unchanged and so are the keys; see `table` below.
//
// That settles Milestone E4 (§16.E4), by a different route than E4 named. E4
// was going to make the WEBSITE's strings.ts the source and merge it into an
// Xcode String Catalog (scripts/gen_ios_strings.mjs). It never landed, and this
// file won instead - which is exactly why the catalog at
// ios/FoolishKit/Localizable.xcstrings went stale and had to be excluded from
// the build (ios/project.yml). The direction is now the other way: C is the
// source, both hosts are generated, and neither one is the other's upstream.
// Keys keep the web's names; iOS-only keys use the `ios.` prefix.

import Foundation

/// The languages the table carries. THE RAW VALUE IS THE MATCH: each case's
/// name is the ISO 639-1 code its speakers' locales begin with, which is what
/// lets `FStrings.match` find a language without a hand-written mapping - see
/// there.
///
/// Adding one is a case here and its 199 keys. Nothing enumerates languages by
/// hand any more: `LocalizationTests` walks `allCases` for completeness, and the
/// resolver walks it against the phone's own preference list, so a new language
/// reaches the people whose phones ask for it the moment it is declared.
///
/// NO REGIONAL VARIANTS, deliberately. One `pt` serves Brazil and Portugal, one
/// `zh` is Simplified and a Traditional reader lands somewhere they can read
/// rather than in English. A split would double the rulebook to fix a handful of
/// words.
///
/// RIGHT-TO-LEFT ARRIVED WITH ARABIC AND HEBREW, and it is the one addition that
/// is not just a column. The earlier note here said shipping them meant auditing
/// the layout rather than adding a case, and that was right: the table mixes
/// absolute `.position`/`.offset` (which SwiftUI does NOT mirror) with
/// `.leading`/`.trailing` padding (which it does), so a mirrored board comes out
/// half-flipped - cards where they were, the deck and the discard swapped around
/// them. See `isRTL` and `MessageTableView`'s pin for what was done instead.
public enum AppLanguage: String, CaseIterable, Sendable {
    case en, ru, ko, zh, vi, es, pt, fr, de, it, ja, pl, uk, tr, id
    case th, nl, sv, da, no, fi, cs, ro, he, ar

    /// Is this language written right to left?
    ///
    /// It does NOT flip the board - see `MessageTableView`, which pins the table
    /// to left-to-right for everyone. It is here for the surfaces that are pure
    /// text and SHOULD mirror (the rulebook, the settings sheet, the lobby), and
    /// so a test can name the two languages that need looking at when the board
    /// is next touched.
    public var isRTL: Bool { self == .he || self == .ar }

    /// Each language names ITSELF - a picker that said "Chinese" in English to
    /// somebody who cannot read English would be pointing at the way out in a
    /// language they do not speak. Endonyms, in the script the language is
    /// written in, which is also why this list is not sorted alphabetically by
    /// anything: it is in `allCases` order, and the phone app's picker shows it
    /// that way.
    public var display: String {
        switch self {
        case .en: return "English"
        case .ru: return "Русский"
        case .ko: return "한국어"
        case .zh: return "中文"
        case .vi: return "Tiếng Việt"
        case .es: return "Español"
        case .pt: return "Português"
        case .fr: return "Français"
        case .de: return "Deutsch"
        case .it: return "Italiano"
        case .ja: return "日本語"
        case .pl: return "Polski"
        case .uk: return "Українська"
        case .tr: return "Türkçe"
        case .id: return "Bahasa Indonesia"
        case .th: return "ไทย"
        case .nl: return "Nederlands"
        case .sv: return "Svenska"
        case .da: return "Dansk"
        case .no: return "Norsk"
        case .fi: return "Suomi"
        case .cs: return "Čeština"
        case .ro: return "Română"
        case .he: return "עברית"
        case .ar: return "العربية"
        }
    }
}

public enum FStrings {
    /// THE PHONE DECIDES UNLESS THE PLAYER SAID OTHERWISE.
    ///
    /// The default is the phone: whatever languages the player set on it, in the
    /// order they put them in, resolved against what the table carries (see
    /// `match`). The owner's reasoning, and it is the right one: a player who
    /// reads Polish has ALREADY told their phone so, once, and asking again is
    /// asking them to do a thing they have done. Done right, nobody ever sees
    /// this happen.
    ///
    /// So the guess does not get to be a guess. It is the OS's own ORDERED
    /// answer, not just its first entry: `Locale.preferredLanguages` is the
    /// whole list from Settings > General > Language & Region, so a phone set to
    /// Catalan then Spanish lands in SPANISH here rather than in English, and a
    /// phone set to a language we do not carry falls through to the next one its
    /// owner actually named. English is the floor, not the second choice.
    ///
    /// AND THE PHONE APP KEEPS ITS LIST (owner). The iMessage board's gear sheet
    /// lost its picker - a drawer that size has no room to spend five rows, let
    /// alone fifteen, on a question the phone already answered - but the phone
    /// app's Settings screen still offers every language, because the one place
    /// a player goes looking for a setting is Settings. Two rules, one sentence:
    /// the phone decides, and a player who disagrees may say so where there is
    /// room to ask.
    public static var active: AppLanguage { override ?? resolved }

    /// The player's explicit choice, or nil for "ask the phone".
    ///
    /// WHERE THE SCREEN IS, THE SETTING IS. It is read from and written to
    /// UserDefaults - but only in the app, never in the iMessage extension, and
    /// `honorsStoredChoice` is that rule. An appex has its own defaults
    /// container, so a value stored there could only have come from the picker
    /// the gear sheet used to have; honouring it now would let a tap from an old
    /// build outrank the phone forever, in the one surface that no longer has a
    /// way to take it back. The extension therefore ignores the key rather than
    /// migrating it, and lands on its own locale like every other device.
    ///
    /// Also the suite's seam: assigning it drives `LocalizationTests` through
    /// every table, and assigning nil puts the phone back in charge.
    public static var override: AppLanguage? {
        get {
            if let forced { return forced }
            guard honorsStoredChoice,
                  let raw = UserDefaults.standard.string(forKey: "ios.language")
            else { return nil }
            return AppLanguage(rawValue: raw)
        }
        set {
            forced = newValue
            if honorsStoredChoice {
                let d = UserDefaults.standard
                if let newValue { d.set(newValue.rawValue, forKey: "ios.language") }
                else { d.removeObject(forKey: "ios.language") }
            }
            cached = nil
        }
    }

    /// The in-memory half of `override`, so a test running inside the extension
    /// target (where nothing is persisted) still drives the language, and so a
    /// set is visible on the very next `t` without a defaults round trip.
    private static var forced: AppLanguage?

    /// Is this process one that has a language screen? False inside the iMessage
    /// extension, whose bundle is an `.appex`. Deliberately a property of the
    /// RUNNING BUNDLE and not a compile flag: FoolishKit is one framework linked
    /// into both, so the answer has to be asked at runtime.
    private static var honorsStoredChoice: Bool {
        Bundle.main.bundleURL.pathExtension != "appex"
    }

    /// The phone's answer, worked out once. `preferredLanguages` is not free and
    /// `t` is called per string per frame; the value cannot change under a live
    /// process anyway, since iOS restarts an app (and its extensions) when the
    /// language changes.
    private static var cached: AppLanguage?

    private static var resolved: AppLanguage {
        if let cached { return cached }
        let lang = Self.match(Locale.preferredLanguages)
        cached = lang
        return lang
    }

    /// The phone's ordered preference list, mapped onto what we carry - the
    /// FIRST entry that matches wins, so the order the player chose is the order
    /// we obey.
    ///
    /// A PREFIX TEST on the language subtag, and it does the work of a mapping
    /// table because `AppLanguage`'s raw values ARE the subtags: `zh-Hans`,
    /// `zh-Hant` and `zh-HK` all begin `zh`, `pt-BR` begins `pt`, `es-419`
    /// begins `es`. Split on `-` first so a tag can never match on a region or a
    /// script by accident - without it `id` would have caught nothing while
    /// something like `xx-ID` caught Indonesian.
    ///
    /// THE SUBTAGS THAT ARE NOT THEIR OWN NAME are the whole reason this is a
    /// function and not a dictionary lookup. Four languages we carry would never
    /// match a phone that asks for them:
    ///
    ///   * `nb` and `nn`. NOBODY'S PHONE SAYS `no`. iOS hands back Bokmal as
    ///     `nb-NO` and Nynorsk as `nn-NO`; `no` is the macrolanguage, and a case
    ///     named for it matches neither. Both land on the one Norwegian table,
    ///     which is written in Bokmal - the form roughly nine in ten Norwegians
    ///     write, and the one a Nynorsk reader reads without friction.
    ///   * `in` and `iw`, Indonesian's and Hebrew's pre-1989 codes. Foundation
    ///     preserves whatever the device stored, so a phone set up years ago can
    ///     still say either, and nobody typed them.
    ///
    /// Internal rather than private so the suite can walk real locale lists
    /// through it; it is pure, and the impurity (asking the phone) is `resolved`.
    static func match(_ preferred: [String]) -> AppLanguage {
        for tag in preferred {
            let subtag = tag.split(separator: "-").first.map(String.init)?.lowercased() ?? ""
            switch subtag {
            case "nb", "nn": return .no     // no phone asks for "no"
            case "in":       return .id     // Indonesian, pre-1989
            case "iw":       return .he     // Hebrew, pre-1989
            default: break
            }
            if let hit = AppLanguage(rawValue: subtag) { return hit }
        }
        return .en
    }

    private static var activeLang: String { active.rawValue }

    /// Look up `key`, interpolating {name}-style placeholders from `args`.
    public static func t(_ key: String, _ args: [String: String] = [:]) -> String {
        let lang = activeLang
        var s = table(lang)?[key] ?? table("en")?[key] ?? key
        // Sorted, because sequential replacement is order-sensitive: a value
        // containing "{otherKey}" would or would not be substituted depending on
        // which key ran first, and Dictionary order is a per-launch accident.
        // No call site relies on that today; sorting makes it impossible to
        // start relying on it by accident.
        for k in args.keys.sorted() {
            guard let v = args[k] else { continue }
            s = s.replacingOccurrences(of: "{\(k)}", with: v)
        }
        return s
    }

    /// A human reason for a rejected move (1.0(4)). The kernel's 21
    /// ENGINE_REJECT_* codes (c/src/game.h, surfaced by fio_last_reject) fold
    /// into a handful of clear, non-code-y sentences. `code` 0 / unknown falls
    /// back to the generic "That move isn't allowed."
    public static func rejectReason(_ code: Int) -> String {
        switch code {
        case 1, 4, 8, 18: return t("ios.rej.turn")     // NOT_PLAYING/NOT_DEFENDER/NOT_FIRST_ATTACKER/NOT_IN_STATUS
        case 2:            return t("ios.rej.pickone")  // EMPTY
        case 3:            return t("ios.rej.defending")// IS_DEFENDER
        case 5, 6:         return t("ios.rej.notyours") // NOT_IN_HAND/DUPLICATES
        case 7, 9:         return t("ios.rej.addrank")  // NOT_SAME_VALUE/VALUE_NOT_ON_TABLE
        case 11, 12, 13, 15: return t("ios.rej.cover")  // NO_UNCOVERED/ATTACK_NOT_ON_TABLE/CANNOT_COVER/COVER_PRESENT
        case 10, 17, 21:   return t("ios.rej.capacity") // DEFENDER_CAPACITY/PASS_CAPACITY/PASS_OVERFLOW
        case 16:           return t("ios.rej.passrank") // PASS_VALUES
        case 20:           return t("ios.rej.mustattack")// FIRST_MUST_ATTACK
        case 19:           return t("ios.rej.alreadygood")// ALREADY_GOOD
        case 14:           return t("ios.rej.notake")   // NO_TABLE_CARDS
        // 22 (PASS_DISABLED, the podkidnoy variant) deliberately has no line of
        // its own: a table without the transfer never offers one, so this is
        // unreachable from the menu, and a message about passing is the one
        // thing such a game must not put on screen. It falls to the generic
        // "that move isn't allowed" with everything else that cannot happen.
        default:           return t("ios.reject")
        }
    }

    // Spoken card names for VoiceOver (round-5 m2): the visible strings were
    // localized while every accessibilityLabel was English, so a ru/ko VoiceOver
    // user got an English board. One builder, shared by every board component,
    // so "queen of spades" / "дама, пики" / "스페이드 퀸" cannot drift apart
    // between the hand, the battles and the deck. Rank indices are the kernel's
    // (9='10' … 13='A', CardRank.label's mapping); numeric ranks stay digits,
    // which VoiceOver reads in its own language already.

    public static func spokenRank(_ value: Int) -> String {
        switch value {
        case 13: return t("ios.rank.ace")
        case 12: return t("ios.rank.king")
        case 11: return t("ios.rank.queen")
        case 10: return t("ios.rank.jack")
        case 9:  return t("ios.rank.ten")
        default: return String(value + 1)
        }
    }

    public static func spokenSuit(_ suit: Suit) -> String {
        switch suit {
        case .spades: return t("ios.suit.spades")
        case .hearts: return t("ios.suit.hearts")
        case .clubs: return t("ios.suit.clubs")
        case .diamonds: return t("ios.suit.diamonds")
        }
    }

    public static func spokenCard(_ value: Int, _ suit: Suit) -> String {
        t("ios.a11y.card", ["rank": spokenRank(value), "suit": spokenSuit(suit)])
    }

    /// The table, by language code. GENERATED, from c/i18n.
    ///
    /// It used to be right here - twenty-five languages and one hundred and
    /// ninety-nine keys written out in Swift, five thousand lines, the largest
    /// hand-written file in the repo by more than double. The trouble was never
    /// its size. It was that the website has a table of its own and the two had
    /// no way to be one thing, so a string changed on the board stayed changed
    /// only on the board.
    ///
    /// The strings are C now (c/i18n/strings_<code>.c, keys in c/i18n/keys.h),
    /// and tools/datagen writes this side and the website's from them at build
    /// time. `t` has not changed and neither have the keys.
    ///
    /// ONE CASE PER LANGUAGE, and it is the one hand-written list left. Swift
    /// cannot look a `let` up by name, so something has to join a code to its
    /// module; making it a switch at least means a language the registry gained
    /// and this did not is a test failure (`LocalizationTests`) rather than a
    /// board silently in English.
    static func table(_ code: String) -> [String: String]? {
        switch code {
        case "en": return FoolishStringsEn
        case "ru": return FoolishStringsRu
        case "ko": return FoolishStringsKo
        case "zh": return FoolishStringsZh
        case "vi": return FoolishStringsVi
        case "es": return FoolishStringsEs
        case "pt": return FoolishStringsPt
        case "fr": return FoolishStringsFr
        case "de": return FoolishStringsDe
        case "it": return FoolishStringsIt
        case "ja": return FoolishStringsJa
        case "pl": return FoolishStringsPl
        case "uk": return FoolishStringsUk
        case "tr": return FoolishStringsTr
        case "id": return FoolishStringsId
        case "th": return FoolishStringsTh
        case "nl": return FoolishStringsNl
        case "sv": return FoolishStringsSv
        case "da": return FoolishStringsDa
        case "no": return FoolishStringsNo
        case "fi": return FoolishStringsFi
        case "cs": return FoolishStringsCs
        case "ro": return FoolishStringsRo
        case "he": return FoolishStringsHe
        case "ar": return FoolishStringsAr
        default: return nil
        }
    }

    /// Every language shared/c/i18n/languages.h declares, with the name it calls
    /// itself and which way it is written. Generated from the same registry
    /// tools/structgen/gen.sh reads to decide what to generate, so this cannot
    /// disagree with what exists.
    public static var languages: [FoolishLanguagesRow] { FoolishLanguages }
}
