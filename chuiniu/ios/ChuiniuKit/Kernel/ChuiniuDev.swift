// ChuiniuDev.swift - the DEBUG dev files (shared/swift/MessagesKit/DevFlags):
// the rig's switches, read from the App Group that only a Debug build asks
// for (project.yml). Nothing here exists in a Release build.

#if DEBUG || SOLO_TESTING
import Foundation
import os

public enum ChuiniuDev {
    /// The Debug log the rig reads (RIG_LOG_SUBSYSTEM in Tools/rig.env).
    public static let log = Logger(subsystem: "cards.chuiniu", category: "dev")

    /// The group the rig writes into (RIG_APP_GROUP in Tools/rig.env).
    public static let group = (Bundle.main.object(forInfoDictionaryKey: "ChuiniuAppGroup") as? String)
        ?? "group.cards.chuiniu"
    static let files = DevFlags(group: group)

    /// `dev.empty`: draw nothing at all (the rig's blank-drawer baseline).
    public static var empty: Bool { files.exists("dev.empty") }

    /// `dev.nick`: the nickname a fresh simulator sits down under, so the
    /// rig never has to type into the name field (pickemup's).
    public static var nickname: String? { files.string("dev.nick") }

    /// `dev.staged` and `dev.sent`: the newest link this extension put in the
    /// input field and the newest one Messages sent, so a check outside the
    /// simulator can decode exactly what the screen drew
    /// (chuiniu/c/tests/cn_link_dump.c).
    public static func noteStaged(_ url: URL) { files.write(url.absoluteString, to: "dev.staged") }
    public static func noteSent(_ url: URL) { files.write(url.absoluteString, to: "dev.sent") }

    /// `dev.seat` (`rig.sh seat WORD`): WHO THIS DEVICE IS, for a game that
    /// needs two people on one simulator. Messages gives a conversation one
    /// local participant, so without it the invitation goes out and nobody
    /// can ever join it. With a word here the extension is that person: its
    /// identity bytes, its nickname and its own set of seat records are the
    /// word's, exactly as cn_twophone_test.c's `be()` switches phone. Read
    /// fresh on every open, never cached, because it changes between two
    /// openings a second apart.
    public static var person: String? {
        guard let w = files.string("dev.seat")?.trimmingCharacters(in: .whitespacesAndNewlines), !w.isEmpty
        else { return nil }
        return w
    }
}
#endif
