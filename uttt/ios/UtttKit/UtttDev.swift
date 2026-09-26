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
    /// Every dev file lives here (shared/swift/MessagesKit/DevFlags.swift
    /// does the finding, reading and writing; the keys below are uttt's).
    private static let dev = DevFlags(group: "group.cards.uttt.msg")
    private static let seatFile = "dev.seat"
    private static let gameFile = "dev.game"
    private static let liveFile = "dev.live"
    private static let pickerFile = "dev.picker"
    private static let rulerFile = "dev.ruler"

    /// `rig.sh ruler on`: paint the motion ruler (UtttRuler) over the sheet.
    /// Read every time, like every other dev file.
    public static var ruler: Bool { dev.exists(rulerFile) }

    /// `dev.empty`: the extension shows nothing at all - no hosting
    /// controller, no kernel call. The memory FLOOR: what Messages, UIKit and
    /// SwiftUI cost an extension before any of ours (TESTFLIGHT_PLAN.md 12).
    public static var empty: Bool { dev.exists("dev.empty") }

    /// `dev.dropinsert`: every insert is swallowed without an answer, exactly
    /// as ChatKit drops one that arrives before the drawer counts as presenting
    /// (shared/c/msg_stage/INSERT_GATING.md) - the only way to film the retries and the send
    /// door on a simulator, where the gate always passes.
    public static var dropInsert: Bool { dev.exists("dev.dropinsert") }

    /// `rig.sh arrive [MOVE]`: the other player's reply, arriving now.
    ///
    /// One simulator has one participant, so nothing ever ARRIVES at an open
    /// drawer - and channel E (a move that lands while the board is up) is
    /// unfilmable without this. The open extension polls for the file (see
    /// `devWatchForArrivals` in MessagesViewController), deletes it, and has
    /// the other dev seat play into the game on screen: MOVE if the file names
    /// one (block*9+cell), otherwise the middle of the kernel's legal list, so
    /// two runs are the same move. The bytes are the shipping kernel's own
    /// `uti_msg_play` as that seat, handed to the same lines `didReceive` runs.
    /// Returns the file's trimmed contents once, then nil until it is written
    /// again.
    public static func takeArrival() -> String? { dev.take("dev.arrive") }

    /// `dev.caption`: the collapsed line of the NEXT staged message, once.
    ///
    /// Store frames only. On the simulator Messages draws each superseded
    /// message of a session with a neighbour's summary (rig README, the iOS 26
    /// and 27 notes), so a transcript of real moves shows wrong lines. The
    /// shoot writes the text each line has to read; the stage that takes it
    /// sets it as `summaryText` only - the bubble's own caption stays the
    /// kernel's. Returns the file's trimmed contents once, then nil.
    public static func takeCaption() -> String? { dev.take("dev.caption") }

    /// `dev.restage`: the next opened bubble puts its own board, unchanged,
    /// back into the input field in the same session - a message that is not
    /// a move. Store frames only: on the simulator the last collapsed line of
    /// a session repeats the line above it, so the shoot sends one extra copy
    /// of a board to carry that repeat, and the owner scrolls it out of frame.
    /// True once, then false until the file is written again.
    public static func takeRestage() -> Bool { dev.take("dev.restage") != nil }

    /// `dev.invite`: the next invitation opens with the seeded game's seed.
    ///
    /// Store frames only. An invitation's seed is the moment it is composed
    /// (utm_seed_at), so an ordinary one draws a different napkin from the
    /// store game's; the empty-board frame opens a real invitation through
    /// the + menu with this set, and it is the same game as every other frame.
    /// True once, then false until the file is written again.
    public static func takeSeededInvite() -> Bool { dev.take("dev.invite") != nil }

    /// The word the rig wrote, or nil in every ordinary run - including an
    /// ordinary DEBUG one, because the file is absent until somebody writes it.
    public static var seat: String? { dev.string(seatFile) }

    /// The identity bytes a device has when `dev.seat` holds `word`. The
    /// kernel hashes them into a seat tag exactly as it hashes a real
    /// participant, so a seeded game can name both seats without two devices.
    public static func identity(_ word: String) -> Data { Data("dev:\(word)".utf8) }

    /// `dev.rotate`: THE ID MESSAGES ROTATED. While the file exists every
    /// identity this device hands the kernel - a dev seat's or the real
    /// participant's - gets a suffix, so no tag it sealed matches any more:
    /// exactly what a reinstall or a TestFlight <-> development swap does to
    /// a real phone (PR #233). The seat must then come from the record or
    /// the sender. The records are untouched: they are keyed by the game.
    public static var rotated: Bool { dev.exists("dev.rotate") }

    /// `id`, rotated when `dev.rotate` says so.
    public static func rotate(_ id: Data) -> Data {
        rotated ? id + Data("|rotated".utf8) : id
    }

    /// Ask who this device is every time a bubble is opened. Off unless the
    /// rig writes the file, and absent from a shipping build entirely.
    public static var picker: Bool { dev.exists(pickerFile) }

    /// Set the seat from inside the app, which is what the on-screen picker
    /// does. The rig writes the same file from outside.
    public static func setSeat(_ word: String?) { dev.write(word, to: seatFile) }

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
        guard let t = dev.raw(gameFile) else { return nil }
        return t.contains(",") ? -1 : Int(t)
    }

    /// `rig.sh devgame 47,20,26,...` - an exact game as block*9+index moves,
    /// for the end states (a win, a draw) the fixed opening never reaches.
    public static var moves: [Int]? {
        guard let raw = dev.raw(gameFile), raw.contains(",") else { return nil }
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
        get { dev.string(liveFile) }
        set { dev.write(newValue, to: liveFile) }
    }

    /// The seed every seeded game uses. A constant, so two runs are the same
    /// board and a screenshot from one can be diffed against the other.
    public static let seed: Int32 = 77
}
#endif
