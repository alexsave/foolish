// AnimLog — a switchable trace of what the board actually animates.
//
// Animation bugs on this board are all "how many times did that run, and with
// what". Reading the code cannot answer that: the triggers are SwiftUI
// lifecycle events (`onChange`, `.task`, a view rebuilt by an `.id` change),
// and which of them fired — and how often — is exactly what is in dispute. So
// the board says so out loud, and the answer is read off a real run.
//
// DEBUG ONLY. A Debug build (the device install the owner judges from, the
// harness, the rig) always traces to the unified log, so a pickup/discard can
// be reproduced on the real extension and read back with `log collect`. A
// Release build compiles `say` to nothing: `on` is the literal `false` and
// `say` is an empty inlinable function, so under whole-module optimisation
// every call site's message closure is dead and the linker drops it.
//
// It used to be an environment switch in Release (FOOLISH_ANIMLOG /
// HARNESS_ANIMLOG), which nothing in the repo set for a Release build - an App
// Store extension cannot be handed an environment - and which cost 130 KB of
// __text in the shipped FoolishKit: the optimiser specialised `say` once per
// call site with that site's string interpolation propagated in (242 symbols),
// all to print nothing. docs/CODE_SIZE.md has the measurement.
import Foundation
import os

public enum AnimLog {
    #if DEBUG
    public static let on = true

    /// The unified-log channel, so a headless run can be captured with
    /// `log stream --predicate 'subsystem == "cards.foolish.anim"'` (or read back
    /// from a device with `log collect`). `print` alone is not captured when the
    /// app is launched detached, which is exactly how the harness runs in CI /
    /// under `simctl` - so the trace was invisible there.
    private static let logger = Logger(subsystem: "cards.foolish.anim", category: "anim")

    public static func say(_ msg: @autoclosure () -> String) {
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
    #else
    /// A literal, not a stored `let`, so `if AnimLog.on { … }` folds away.
    @inlinable public static var on: Bool { false }

    /// Empty on purpose: the autoclosure is never evaluated, and after inlining
    /// it is never even built.
    @inlinable public static func say(_ msg: @autoclosure () -> String) {}
    #endif

    /// Monotonic run id, so two overlapping streams are visibly two streams
    /// rather than one long one - the whole question in a "double animation".
    private static var seq = 0
    public static func nextRun() -> Int { seq += 1; return seq }
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
