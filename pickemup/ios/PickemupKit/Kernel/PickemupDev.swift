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

    /// `dev.slide`: the auto-collapse rides the shared CollapseSlide on the
    /// kernel's push (ANIMATION_DECISIONS A14). Read once per process.
    public static let slide = files.exists("dev.slide")

    /// `dev.sendhint`: the shared Send reminder under Messages' Send button,
    /// on the kernel's word and fuse (A15). Read once per process: a view
    /// body asks for it every frame.
    public static let sendHint = files.exists("dev.sendhint")
}
#endif
