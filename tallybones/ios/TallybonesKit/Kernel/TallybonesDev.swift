// COPIED from pickemup/ios/PickemupKit/Kernel/PickemupDev.swift at 8e216923 - the DEBUG dev files (shared/swift/MessagesKit/DevFlags):
// the rig's switches, read from the App Group that only a Debug build asks
// for (project.yml). Nothing here exists in a Release build.

#if DEBUG || SOLO_TESTING
import Foundation

public enum TallybonesDev {
    /// The group the rig writes into (RIG_APP_GROUP in Tools/rig.env).
    public static let group = (Bundle.main.object(forInfoDictionaryKey: "TallybonesAppGroup") as? String)
        ?? "group.cards.tallybones"
    static let files = DevFlags(group: group)

    /// `dev.empty`: draw nothing at all (the rig's blank-drawer baseline).
    public static var empty: Bool { files.exists("dev.empty") }

    /// `dev.nick`: the nickname a fresh simulator sits down under, so the
    /// rig never has to type into the name field.
    public static var nickname: String? { files.string("dev.nick") }

    /// `dev.who`: the PERSON this simulator plays, by name (DECISIONS T65).
    /// One simulator is one Messages identity in both of its stub threads,
    /// so without this a second participant can never be seated on it. With
    /// it, the name is the nickname, the participant id is derived from the
    /// name (so each person keeps a stable seat tag), the seat records are
    /// that person's own, and Messages' sender fact is withheld, because on a
    /// simulator every bubble reads as sent by this device.
    public static var who: String? {
        guard let w = files.string("dev.who")?.trimmingCharacters(in: .whitespacesAndNewlines), !w.isEmpty
        else { return nil }
        return w
    }

    /// The 16-byte participant id `dev.who` stands for.
    public static func participant(_ who: String) -> Data {
        var bytes = [UInt8](repeating: 0x5A, count: 16)
        for (i, b) in who.utf8.enumerated() { bytes[i % 16] = bytes[i % 16] &* 31 &+ b }
        return Data(bytes)
    }
}
#endif
