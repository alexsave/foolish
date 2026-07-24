// AnimLog — a switchable trace of what the board actually animates.
//
// Animation bugs on this board are all "how many times did that run, and with
// what". Reading the code cannot answer that: the triggers are SwiftUI
// lifecycle events (`onChange`, `.task`, a view rebuilt by an `.id` change),
// and which of them fired — and how often — is exactly what is in dispute. So
// the board says so out loud, and the answer is read off a real run.
//
// Off unless FOOLISH_ANIMLOG is set in the environment (the harness passes it
// through as HARNESS_ANIMLOG), so a shipping build prints nothing and pays a
// single Bool check per event.
import Foundation
import os

public enum AnimLog {
    #if DEBUG
    // A Debug build (the device install the owner judges from) always traces to
    // the unified log, so a pickup/discard can be reproduced on the real
    // extension and read back with `log collect` - no env var to thread through
    // an app extension the harness can't set. Release stays silent.
    public static let on = true
    #else
    public static let on = ProcessInfo.processInfo.environment["FOOLISH_ANIMLOG"] != nil
        || ProcessInfo.processInfo.environment["HARNESS_ANIMLOG"] != nil
    #endif

    /// The unified-log channel, so a headless run can be captured with
    /// `log stream --predicate 'subsystem == "cards.foolish.anim"'` (or read back
    /// from a device with `log collect`). `print` alone is not captured when the
    /// app is launched detached, which is exactly how the harness runs in CI /
    /// under `simctl` - so the trace was invisible there.
    private static let logger = Logger(subsystem: "cards.foolish.anim", category: "anim")

    /// Monotonic run id, so two overlapping streams are visibly two streams
    /// rather than one long one — the whole question in a "double animation".
    private static var seq = 0
    public static func nextRun() -> Int { seq += 1; return seq }

    public static func say(_ msg: @autoclosure () -> String) {
        guard on else { return }
        line += 1
        let text = "\(line) \(msg())"
        print("ANIMLOG \(text)")
        logger.log("ANIMLOG \(text, privacy: .public)")
        // Also on screen: the trace matters most on a real phone, where nobody
        // is reading a console. Hopping to the main actor can in principle
        // reorder two entries, which is why every line carries its own index.
        Task { @MainActor in AnimLogStore.shared.append(text) }
    }
    private static var line = 0
}

/// The last few trace lines, for the harness's on-screen panel. Dev-only: the
/// shipping extension never sets the env var, so nothing is ever appended.
@MainActor
public final class AnimLogStore: ObservableObject {
    public static let shared = AnimLogStore()
    @Published public private(set) var lines: [String] = []
    private let cap = 60

    public func append(_ s: String) {
        lines.append(s)
        if lines.count > cap { lines.removeFirst(lines.count - cap) }
    }
    public func clear() { lines.removeAll() }
}
