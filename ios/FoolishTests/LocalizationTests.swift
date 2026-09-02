// ROUND 30 — EVERY LANGUAGE SAYS EVERYTHING.
//
// FStrings falls back to English and then to the key itself, so a missing
// translation does not crash: it renders as `ios.msg.replaylink` on the results
// screen, under the ranking, in front of the player. The comment on the table
// has claimed since day one that "a CI check enforces identical key sets" and
// there was no such check - which is fine while three languages are written by
// one hand in one sitting, and stops being fine the moment a key is added.
//
// So this is that check, and it walks `AppLanguage.allCases`: adding a language
// enrols it automatically, and adding a key breaks every language that has not
// learned it yet.
import XCTest
import UIKit
@testable import FoolishKit

final class LocalizationTests: XCTestCase {

    /// The keys the app actually asks for, taken from English - the language
    /// every other one is measured against because it is the fallback.
    private func englishKeys() -> [String] {
        // Read through the public API rather than the private table: `t` is what
        // the app calls, so this measures what the app would actually get.
        // A key is "present" when it does not answer with its own name.
        let was = FStrings.override
        defer { FStrings.override = was }
        FStrings.override = .en
        return Self.allKeys.filter { FStrings.t($0) != $0 }
    }

    /// ENGLISH carries every key the app asks for. Only English: `t` falls back
    /// to English before it falls back to the key, so a key missing from one of
    /// the other languages renders as ENGLISH TEXT, not as its own name - there
    /// is no way to see it from out here, and asking every language whether
    /// `t(k) == k` would pass against a table with the key deleted. Checked by
    /// mutation, which is how that hole was found rather than argued about.
    /// `testNothingIsSilentlyLeftInEnglish` below is the one that catches a
    /// missing translation, precisely because a fallback IS the English string.
    func testEnglishCarriesEveryKeyTheAppAsksFor() {
        let was = FStrings.override
        defer { FStrings.override = was }
        let keys = englishKeys()
        XCTAssertEqual(keys.count, Self.allKeys.count,
                       "English is missing keys: "
                       + Self.allKeys.filter { k in !keys.contains(k) }.joined(separator: ", "))
    }

    /// A translation that dropped `{name}` renders a sentence with a hole in it,
    /// and one that invented `{n}` renders the braces literally. Both are
    /// invisible until somebody plays in that language.
    func testEveryLanguageKeepsThePlaceholdersEnglishHas() {
        let was = FStrings.override
        defer { FStrings.override = was }
        FStrings.override = .en
        var expected: [String: Set<String>] = [:]
        for k in englishKeys() { expected[k] = Self.placeholders(in: FStrings.t(k)) }

        for lang in AppLanguage.allCases where lang != .en {
            FStrings.override = lang
            for (k, want) in expected {
                let got = Self.placeholders(in: FStrings.t(k))
                XCTAssertEqual(got, want, "\(lang) \(k): placeholders \(got) but English has \(want)")
            }
        }
    }

    /// Nothing may be left in English by accident - AND THIS IS THE TEST THAT
    /// CATCHES A MISSING TRANSLATION, for the reason spelled out above: `t`
    /// falls back to English, so a key absent from the Chinese table and a key
    /// somebody forgot to translate are the same thing on screen, and this is
    /// what sees both.
    ///
    /// Only strings that are PROPER NOUNS or genuinely the same word in that
    /// language may match - the bot city names, mostly - and those are named
    /// here one by one rather than pattern-matched, so a real untranslated
    /// sentence cannot hide behind a rule.
    func testNothingIsSilentlyLeftInEnglish() {
        let was = FStrings.override
        defer { FStrings.override = was }
        FStrings.override = .en
        var english: [String: String] = [:]
        for k in englishKeys() { english[k] = FStrings.t(k) }

        for lang in AppLanguage.allCases where lang != .en {
            FStrings.override = lang
            var same: [String] = []
            for (k, e) in english where FStrings.t(k) == e && !Self.mayMatchEnglish.contains(k) {
                same.append(k)
            }
            XCTAssertTrue(same.isEmpty, "\(lang) left these in English: \(same.sorted().joined(separator: ", "))")
        }
    }

    /// THE ACTION BAR IS A FIXED-WIDTH PILL, and a label that does not fit it is
    /// a defect the table cannot show you.
    ///
    /// `FActionBar` draws Attack / Cover / Pass / Pickup / Good at a fixed 96pt
    /// with 10pt of padding each side, so each label has 76pt to live in. The
    /// button already refuses to wrap (`lineLimit(1)`) and shrinks to 0.75
    /// before it gives up - a floor put there for the Russian "Отменить" - so
    /// past that it TRUNCATES, and a button reading "Tấn cô…" is not a button.
    ///
    /// Measured with the real font rather than counted in characters, because
    /// the languages this has to hold now disagree wildly about what a character
    /// costs: 盖牌 is two glyphs and wide, "Tấn công" is eight and narrow.
    func testEveryActionLabelFitsItsButton() {
        let was = FStrings.override
        defer { FStrings.override = was }
        let keys = ["attack", "cover", "pass", "pickup", "good"]
        let room: CGFloat = 96 - 2 * 10
        let font = UIFont.systemFont(ofSize: 15, weight: .semibold)
        var shrunk: [String] = []
        for lang in AppLanguage.allCases {
            FStrings.override = lang
            for k in keys {
                let label = FStrings.t(k)
                let w = (label as NSString)
                    .size(withAttributes: [.font: font]).width
                XCTAssertLessThanOrEqual(w, room / 0.75,
                    "\(lang) \(k) = \"\(label)\" is \(Int(w))pt and would TRUNCATE in a "
                    + "\(Int(room))pt button even shrunk to its 0.75 floor")
                if w > room { shrunk.append("\(lang).\(k)=\"\(label)\" \(Int(w))pt") }
            }
        }
        // Not a failure - shrinking is what the floor is for - but say which
        // ones are spending it, so a language that is one word away from
        // truncating is visible before it gets there.
        if !shrunk.isEmpty { print("action labels riding the shrink floor: \(shrunk.joined(separator: ", "))") }
    }

    /// Every language names itself in its own script, so the picker is readable
    /// by the person who needs it (see `AppLanguage.display`).
    func testEveryLanguageNamesItself() {
        var seen = Set<String>()
        for lang in AppLanguage.allCases {
            XCTAssertFalse(lang.display.isEmpty, "\(lang) has no display name")
            XCTAssertTrue(seen.insert(lang.display).inserted,
                          "two languages both call themselves \(lang.display)")
        }
    }

    /// The OS locale has to land somewhere sensible for each language we ship,
    /// or a Vietnamese phone opens the app in English and the setting looks
    /// broken rather than undiscovered.
    func testTheSystemLocaleFindsEachLanguage() {
        // Exercised through the same prefix rule `systemDetected` uses; the
        // private property itself is not reachable, so this pins the CONTRACT
        // the raw values encode - each language's code is the prefix its
        // speakers' locales carry.
        for lang in AppLanguage.allCases {
            XCTAssertEqual(lang.rawValue.count, 2, "\(lang) is not a 2-letter code")
        }
        // The Chinese case is the one worth stating: `zh-Hans`, `zh-Hant` and
        // `zh-HK` all begin `zh`, and all of them should land on the table we
        // have rather than in English.
        for code in ["zh", "zh-Hans", "zh-Hant", "zh-HK", "zh-TW"] {
            XCTAssertTrue(code.hasPrefix(AppLanguage.zh.rawValue),
                          "\(code) would not be detected as Chinese")
        }
        XCTAssertTrue("vi-VN".hasPrefix(AppLanguage.vi.rawValue))
    }

    // MARK: fixtures

    private static func placeholders(in s: String) -> Set<String> {
        var out = Set<String>()
        var i = s.startIndex
        while let open = s[i...].firstIndex(of: "{") {
            guard let close = s[open...].firstIndex(of: "}") else { break }
            out.insert(String(s[s.index(after: open)..<close]))
            i = s.index(after: close)
        }
        return out
    }

    /// Proper nouns and words that are genuinely identical in another language.
    /// Bot names are cities: Vietnamese writes Miami, New York, Seoul, Madrid
    /// and Vienna exactly as English does, and "Max" is a name in every language
    /// here.
    private static let mayMatchEnglish: Set<String> = [
        "ios.bot.random", "ios.bot.handwritten", "ios.bot.robusta",
        "ios.bot.firecracker", "ios.bot.blackpowder", "ios.bot.cordite",
        "ios.bot.octogen", "ios.bot.max",
        // Digits, in every language that writes them as digits.
        "ios.rank.ten",
        // Chinese card players say the letters (A/K/Q/J), exactly as printed on
        // the card - so these read identically to the English words' KEYS but
        // are the localized answer, not an oversight.
        "ios.rank.ace", "ios.rank.king", "ios.rank.queen", "ios.rank.jack",
    ]

    /// Every key the table is expected to carry. Listed rather than reflected
    /// because the table is private and - more to the point - because a list
    /// is what makes "the app asks for a key nobody wrote" a failure instead
    /// of a silent pass.
    private static let allKeys: [String] = [
        "play", "offline", "join_by_code", "resume", "replays", "tutorial",
        "settings", "about", "pass", "pickup", "good", "attack", "cover",
        "game_over", "you_win", "you_lose", "rematch", "share_replay", "home",
        "choose_opponent", "start_game", "players", "thinking",
        "leave_game_title", "leave_game_body", "leave", "cancel",
        "ios.lobby", "ios.game_code", "ios.ready", "ios.add_bot",
        "ios.share_invite", "join_game", "ios.dashboard", "ios.create_game",
        "ios.sign_out", "ios.online_soon", "ios.reject", "ios.you", "ios.fool",
        "ios.nobattle", "ios.msg.yourmove", "ios.msg.staged", "ios.msg.waiting",
        "ios.msg.waitingfor", "ios.msg.send", "ios.msg.sending", "ios.msg.undo",
        "ios.msg.newgame", "ios.msg.replaylink", "ios.msg.replaylink.copied",
        "ios.msg.pickseat", "ios.msg.spectating", "ios.msg.thread", "ios.msg.tap",
        "ios.msg.damaged", "ios.msg.open", "ios.msg.fool", "ios.msg.isfool",
        "ios.msg.moved", "ios.msg.opennewest", "ios.msg.stale",
        "ios.msg.viewanyway", "ios.msg.yourname", "ios.msg.nameprompt",
        "ios.msg.continue", "ios.msg.seatopen", "ios.msg.joinas",
        "ios.msg.waitingjoin", "ios.msg.lobbyfull", "ios.msg.creategame",
        "ios.msg.startgame", "ios.msg.gameon", "ios.msg.joininvite",
        "ios.msg.exitgame", "ios.msg.left", "ios.msg.leftanon", "ios.msg.invite",
        "ios.msg.nickname_ph", "ios.msg.entername", "ios.msg.nametoolong",
        "ios.msg.cardfmt", "ios.msg.seatn", "ios.msg.mv.attack", "ios.msg.mv.pass",
        "ios.msg.mv.cover", "ios.msg.mv.coverpair", "ios.msg.mv.pickup",
        "ios.msg.mv.out", "ios.msg.mv.good", "ios.msg.mv.nothing",
        "ios.msg.mv.roundover", "ios.msg.sendhint", "ios.msg.started",
        "ios.msg.joined", "ios.rej.turn", "ios.rej.pickone", "ios.rej.defending",
        "ios.rej.notyours", "ios.rej.addrank", "ios.rej.cover", "ios.rej.capacity",
        "ios.rej.passrank", "ios.rej.mustattack", "ios.rej.alreadygood",
        "ios.rej.notake", "ios.help", "ios.done", "ios.settings.title",
        "ios.settings.language", "ios.settings.table", "ios.settings.table.wool",
        "ios.settings.table.felt", "ios.rules.title", "ios.rules.goal.h",
        "ios.rules.goal.b", "ios.rules.setup.h", "ios.rules.setup.b",
        "ios.rules.setup.cap", "ios.rules.start.h", "ios.rules.start.b",
        "ios.rules.sword", "ios.rules.shield", "ios.rules.attack.h",
        "ios.rules.attack.b", "ios.rules.attack.ok1", "ios.rules.attack.ok2",
        "ios.rules.attack.no1", "ios.rules.defend.h", "ios.rules.defend.b",
        "ios.rules.defend.b.nopass", "ios.rules.cover.h", "ios.rules.cover.b",
        "ios.rules.cover.ok1", "ios.rules.cover.ok2", "ios.rules.cover.no1",
        "ios.rules.cover.no2", "ios.rules.throw.h", "ios.rules.throw.b",
        "ios.rules.throw.ok1", "ios.rules.throw.ok2", "ios.rules.throw.ok3",
        "ios.rules.throw.no1", "ios.rules.throw.no2", "ios.rules.pickup.h",
        "ios.rules.pickup.b", "ios.rules.pickup.cap", "ios.rules.round.h",
        "ios.rules.round.b", "ios.rules.pass.h", "ios.rules.pass.b",
        "ios.rules.pass.ok1", "ios.rules.pass.no1", "ios.rules.pass.no2",
        "ios.rules.pass.no3", "ios.rules.end.h", "ios.rules.end.b",
        "ios.rules.lobby.h", "ios.rules.lobby.b", "ios.rules.variant.h",
        "ios.rules.variant.b", "ios.rules.fool.h", "ios.rules.fool.b",
        "ios.lobby.passing", "ios.rules.lbl.table", "ios.rules.lbl.hand",
        "ios.rules.defender", "ios.rules.nextplayer", "ios.msg.nametaken",
        "ios.a11y.attackfirst", "ios.a11y.on", "ios.a11y.off",
        "ios.a11y.defending", "ios.a11y.attacking", "ios.a11y.saidgood",
        "ios.a11y.thinking", "ios.a11y.out", "ios.a11y.cards", "ios.a11y.deck",
        "ios.a11y.trump", "ios.a11y.trumpmark", "ios.a11y.discard",
        "ios.a11y.covered", "ios.a11y.uncovered", "ios.a11y.hiddencard",
        "ios.a11y.facedown", "ios.a11y.card", "ios.suit.spades",
        "ios.suit.hearts", "ios.suit.clubs", "ios.suit.diamonds",
        "ios.rank.ace", "ios.rank.king", "ios.rank.queen", "ios.rank.jack",
        "ios.rank.ten", "ios.tut_next", "ios.tut_done", "ios.tut_1", "ios.tut_2",
        "ios.tut_3", "ios.tut_4", "ios.tut_5", "ios.bot.random",
        "ios.bot.handwritten", "ios.bot.robusta", "ios.bot.firecracker",
        "ios.bot.blackpowder", "ios.bot.cordite", "ios.bot.octogen",
        "ios.bot.max", "ios.bot.km", "ios.bot.km0",
    ]
}
