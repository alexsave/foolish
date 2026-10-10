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
    /// `dev.fill` (`echo 6 > dev.fill`), ONE-SHOT: the next game this
    /// extension makes from the + menu is a group lobby filled to that many
    /// seats (2 to 6) by made-up people and started, so the rig gets a table
    /// of six in one opening (BridgeKernel.devFill). The rig can play two
    /// people on one simulator (`dev.seat`), but six would be five rounds of
    /// kill, switch, tap and join just to sit down; the memory and bubble
    /// checks need the table, not the sitting down. Taken (deleted) on read.
    public static func takeFill() -> Int? { files.take("dev.fill").flatMap { Int($0) } }

    /// `dev.syncframes`: draw every stage frame on the main thread, as before
    /// package V1 (the measurement's "before"; the frame log says which).
    /// Read once a process (every opening of the drawer is a new one), so the
    /// frames it times read no file.
    public static let syncFrames: Bool = files.exists("dev.syncframes")

    /// `dev.straight`: the stage's pixels straight RGBA, as before package V1,
    /// which Core Animation redraws into its own form on every commit (the
    /// measurement's "before"). Read once a process.
    public static let straightFrames: Bool = files.exists("dev.straight")

    /// THE LAUNCH LOG (package M): `launch <what> <ms>` with the ms since
    /// this process started (the kernel's own start time), so the rig reads
    /// where a cold open's time goes: dyld and the extension's start, the
    /// view, the conversation, the first frame.
    public static func launch(_ what: String) {
        let ms = (Date().timeIntervalSince1970 - processStart) * 1000
        log.info("launch \(what, privacy: .public) \(String(format: "%.1f", ms), privacy: .public)")
    }

    private static let processStart: Double = {
        var info = kinfo_proc()
        var size = MemoryLayout<kinfo_proc>.stride
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        guard sysctl(&mib, 4, &info, &size, nil, 0) == 0 else { return Date().timeIntervalSince1970 }
        let t = info.kp_proc.p_un.__p_starttime
        return Double(t.tv_sec) + Double(t.tv_usec) / 1e6
    }()

    /// The first stage frame of this process is logged once.
    @MainActor static var firstFrameLogged = false

    public static var person: String? {
        guard let w = files.string("dev.seat")?.trimmingCharacters(in: .whitespacesAndNewlines), !w.isEmpty
        else { return nil }
        return w
    }
}
#endif
