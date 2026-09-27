// ChuiniuDev.swift - the DEBUG dev files (shared/swift/MessagesKit/DevFlags):
// the rig's switches, read from the App Group that only a Debug build asks
// for (project.yml). Nothing here exists in a Release build.

#if DEBUG || SOLO_TESTING
import Foundation

public enum ChuiniuDev {
    /// The group the rig writes into.
    public static let group = (Bundle.main.object(forInfoDictionaryKey: "ChuiniuAppGroup") as? String)
        ?? "group.cards.chuiniu"
    static let files = DevFlags(group: group)

    /// `dev.empty`: draw nothing at all (the rig's blank-drawer baseline).
    public static var empty: Bool { files.exists("dev.empty") }

    /// `dev.scene`: which scripted scene the scaffold's FakeKernel opens on
    /// (FakeKernel.Scene's raw values). Gone with the fake.
    public static var scene: FakeKernel.Scene? { files.string("dev.scene").flatMap(FakeKernel.Scene.init(rawValue:)) }
}
#endif
