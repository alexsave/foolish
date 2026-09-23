// UtttDev.swift - DEBUG-only identity override, so one device can be two people.
//
// NEVER COMPILED INTO A SHIPPING BUILD: the whole file is inside `#if DEBUG`,
// and a TestFlight or App Store build does not define it.
//
// WHY IT EXISTS. A two-player game in a transcript cannot be played on one
// simulator. Messages gives a conversation exactly one local participant, a
// seat tag is a hash of that participant, and an app bubble cannot be
// forwarded to the other stub thread - so the creator can send an invitation
// and then nothing at all happens, forever. Every screen past the lobby is
// unreachable, which is most of the app.
//
// This is the same shape as the host app's `dev.seat`: a FILE in the App
// Group, read fresh every time, that overrides who this device is. Write "a"
// and the device is one player; write "b" and it is the other; delete it and
// the real participant comes back. The rig flips it between taps and plays
// both sides of one game in one thread.
//
// A FILE, not a UserDefaults key, for the reason the host app records: a
// `defaults write` from outside the sandbox lands in the wrong domain, and
// cfprefsd caches App Group preferences until the device reboots. A file is
// read by the process that wants it, when it wants it.
//
// READ EVERY TIME, never cached. The whole point is that it changes between
// two drawer openings that are one second apart.

#if DEBUG
import Foundation

public enum UtttDev {
    private static let appGroup = "group.cards.uttt.msg"
    private static let seatFile = "dev.seat"
    private static let gameFile = "dev.game"
    private static let liveFile = "dev.live"
    private static let pickerFile = "dev.picker"

    /// The word the rig wrote, or nil in every ordinary run - including an
    /// ordinary DEBUG one, because the file is absent until somebody writes it.
    public static var seat: String? {
        guard let dir = FileManager.default
                .containerURL(forSecurityApplicationGroupIdentifier: appGroup),
              let raw = try? String(contentsOf: dir.appendingPathComponent(seatFile),
                                    encoding: .utf8)
        else { return nil }
        let s = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return s.isEmpty ? nil : s
    }

    /// The identity bytes a device has when `dev.seat` holds `word`. The
    /// kernel hashes them into a seat tag exactly as it hashes a real
    /// participant, so a seeded game can name both seats without two devices.
    public static func identity(_ word: String) -> Data { Data("dev:\(word)".utf8) }

    /// Ask who this device is every time a bubble is opened. Off unless the
    /// rig writes the file, and absent from a shipping build entirely.
    public static var picker: Bool {
        guard let u = url(pickerFile) else { return false }
        return FileManager.default.fileExists(atPath: u.path)
    }

    /// Set the seat from inside the app, which is what the on-screen picker
    /// does. The rig writes the same file from outside.
    public static func setSeat(_ word: String?) {
        guard let u = url(seatFile) else { return }
        if let word, !word.isEmpty {
            try? word.write(to: u, atomically: true, encoding: .utf8)
        } else {
            try? FileManager.default.removeItem(at: u)
        }
    }

    /// How many moves into a game to open, or nil for the ordinary flow.
    ///
    /// AND IT SKIPS THE FLOW ENTIRELY. With this set the extension does not
    /// show the lobby, does not start, does not join: it seats both players
    /// from `dev.seat`, plays this many moves with the kernel's own bot, and
    /// opens the board. The host app's `dev.fatboard` does the same thing for
    /// the same reason - some states are tedious to reach by hand and a
    /// filmed before-and-after is only comparable if the same one comes up
    /// every run. The bot is deterministic given the seed, so it does.
    public static var game: Int? {
        guard let dir = FileManager.default
                .containerURL(forSecurityApplicationGroupIdentifier: appGroup),
              let raw = try? String(contentsOf: dir.appendingPathComponent(gameFile),
                                    encoding: .utf8)
        else { return nil }
        let t = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return t.contains(",") ? -1 : Int(t)
    }

    /// `rig.sh devgame 47,20,26,...` - an exact game as block*9+index moves,
    /// for the end states (a win, a draw) the fixed opening never reaches.
    public static var moves: [Int]? {
        guard let u = url(gameFile),
              let raw = try? String(contentsOf: u, encoding: .utf8),
              raw.contains(",")
        else { return nil }
        return raw.split(separator: ",").compactMap {
            Int($0.trimmingCharacters(in: .whitespacesAndNewlines))
        }
    }

    /// THE SEEDED GAME AS IT NOW STANDS, written back after every move.
    ///
    /// Without this the seeded opening rebuilds itself every time the drawer
    /// opens, so a move made under `dev.seat a` is gone the moment the seat
    /// flips to `b` and the game can never get past one ply. With it, flipping
    /// the seat and re-opening the drawer is the other player sitting down at
    /// the same board - which is the only way one simulator plays a two-handed
    /// game at all. The host app writes `dev.claimed` and `dev.staged` back
    /// the same way.
    public static var live: String? {
        get {
            guard let u = url(liveFile),
                  let raw = try? String(contentsOf: u, encoding: .utf8)
            else { return nil }
            let s = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            return s.isEmpty ? nil : s
        }
        set {
            guard let u = url(liveFile) else { return }
            if let newValue { try? newValue.write(to: u, atomically: true, encoding: .utf8) }
            else { try? FileManager.default.removeItem(at: u) }
        }
    }

    private static func url(_ name: String) -> URL? {
        FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup)?
            .appendingPathComponent(name)
    }

    /// The seed every seeded game uses. A constant, so two runs are the same
    /// board and a screenshot from one can be diffed against the other.
    public static let seed: Int32 = 77
}
#endif
