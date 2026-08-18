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

public enum AnimLog {
    public static let on = ProcessInfo.processInfo.environment["FOOLISH_ANIMLOG"] != nil
        || ProcessInfo.processInfo.environment["HARNESS_ANIMLOG"] != nil

    /// Monotonic run id, so two overlapping streams are visibly two streams
    /// rather than one long one — the whole question in a "double animation".
    private static var seq = 0
    public static func nextRun() -> Int { seq += 1; return seq }

    public static func say(_ msg: @autoclosure () -> String) {
        guard on else { return }
        line += 1
        let text = "\(line) \(msg())"
        print("ANIMLOG \(text)")
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
