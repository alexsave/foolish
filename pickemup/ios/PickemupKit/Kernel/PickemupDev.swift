// PickemupDev.swift - the DEBUG dev files (shared/swift/MessagesKit/DevFlags):
// the rig's switches, read from the App Group that only a Debug build asks
// for (project.yml). Nothing here exists in a Release build.

#if DEBUG || SOLO_TESTING
import CoreGraphics
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
    /// (IOS_DECISIONS I43). Read once per process: leaving the thread ends
    /// the process, and that is when the rig flips the file.
    public static let persona: (n: UInt8, name: String)? = {
        guard let s = files.string("dev.persona") else { return nil }
        let parts = s.split(separator: " ", maxSplits: 1).map(String.init)
        guard parts.count == 2, let n = UInt8(parts[0]), n > 0, !parts[1].isEmpty else { return nil }
        return (n, parts[1])
    }()

    /// `dev.seed` (64 hex digits): the next game this device makes deals from
    /// this seed, so the rig can replay a known game inside Messages.
    public static var seed: [UInt8]? {
        guard let hex = files.string("dev.seed"), hex.count == 64 else { return nil }
        var out: [UInt8] = []
        var i = hex.startIndex
        while i < hex.endIndex {
            let j = hex.index(i, offsetBy: 2)
            guard let b = UInt8(hex[i..<j], radix: 16) else { return nil }
            out.append(b)
            i = j
        }
        return out
    }

    /// `dev.anchors.on`: every anchor's frame is written to `dev.anchors` as it
    /// changes, one `name x y w h` line each in the extension view's points
    /// (the board inset added), so the rig taps a card where it is instead of
    /// where a screenshot suggests. Read once per process.
    public static let anchorsOn = files.exists("dev.anchors.on")

    public static func writeAnchors(_ anchors: [String: CGRect], inset: CGPoint) {
        guard anchorsOn else { return }
        let lines = anchors.keys.sorted().map { k -> String in
            let r = anchors[k]!
            return String(format: "%@ %.1f %.1f %.1f %.1f", k, r.minX + inset.x, r.minY + inset.y, r.width, r.height)
        }
        files.write(lines.joined(separator: "\n") + "\n", to: "dev.anchors")
    }

    /// `dev.slide`: the auto-collapse rides the shared CollapseSlide on the
    /// kernel's push (ANIMATION_DECISIONS A14). Read once per process.
    public static let slide = files.exists("dev.slide")

    /// `dev.sendhint`: the shared Send reminder under Messages' Send button,
    /// on the kernel's word and fuse (A15). Read once per process: a view
    /// body asks for it every frame.
    public static let sendHint = files.exists("dev.sendhint")
}
#endif
