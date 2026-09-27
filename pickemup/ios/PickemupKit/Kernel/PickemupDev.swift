// PickemupDev.swift - the DEBUG dev files (shared/swift/MessagesKit/DevFlags):
// the rig's switches, read from the App Group that only a Debug build asks
// for (project.yml). Nothing here exists in a Release build.

#if DEBUG || SOLO_TESTING
import Foundation

public enum PickemupDev {
    /// The group the rig writes into (RIG_APP_GROUP in Tools/rig.env).
    public static let group = (Bundle.main.object(forInfoDictionaryKey: "PickemupAppGroup") as? String)
        ?? "group.cards.pickemup"
    static let files = DevFlags(group: group)

    /// `dev.empty`: draw nothing at all (the rig's blank-drawer baseline).
    public static var empty: Bool { files.exists("dev.empty") }

    /// `dev.nick`: the nickname a fresh simulator sits down under, so the
    /// rig never has to type into the name field.
    public static var nickname: String? { files.string("dev.nick") }

    /// `dev.persona` ("2 Bo"): this appex process sits down as ANOTHER person
    /// on the same simulator: its participant id altered by the number (1 to
    /// 255), its own seat records and its own nickname. A simulator's
    /// Messages gives this extension one participant id in every thread, so
    /// without it the rig's two-thread trick seats the same person twice
    /// (IOS_DECISIONS I41). Read once per process: leaving the thread ends
    /// the process, and that is when the rig flips the file.
    public static let persona: (n: UInt8, name: String)? = {
        guard let s = files.string("dev.persona") else { return nil }
        let parts = s.split(separator: " ", maxSplits: 1).map(String.init)
        guard parts.count == 2, let n = UInt8(parts[0]), n > 0, !parts[1].isEmpty else { return nil }
        return (n, parts[1])
    }()
}
#endif
