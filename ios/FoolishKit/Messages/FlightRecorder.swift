// FlightRecorder — what the extension was doing when it stopped.
//
// The owner, round 16: "sometimes it just hangs. Even on newer devices. Can't
// tell why. Maybe you could check memory and figure it out. And more
// importantly, if there's a crash or something make it appear as a diagnostic
// dump in the UI so I can check next time it happens."
//
// "EVEN ON NEWER DEVICES" IS THE CLUE, and it is why this is a recorder rather
// than a guess. An iMessage extension's memory ceiling is a FIXED budget, not a
// share of the device's RAM, so a phone with 8GB gives the drawer no more room
// than one with 3GB - a memory kill looks device-independent, which is exactly
// how the owner described it. And a memory kill is a SIGKILL: no handler runs,
// no exception is thrown, nothing is printed, and the drawer is left showing a
// frozen snapshot that ignores taps. From inside, "hung" and "killed" are the
// same picture. The only way to tell them apart afterwards is to have been
// writing things down BEFORE it happened.
//
// So: a bounded breadcrumb trail in the App Group, appended as the extension
// works, and read back on the NEXT launch. A session that ends without writing
// its own goodbye line ended abruptly, and its trail - what it was doing, and
// what its memory footprint was at each step - is what the panel shows.
//
// THREE THINGS IT CAN ACTUALLY TELL APART:
//   * a memory kill:  no crash line, a footprint climbing toward the ceiling,
//                     usually a `memory-warning` breadcrumb just before the end.
//   * a real crash:   a `crash sig N` line, written by the signal handler.
//   * a genuine HANG: the extension is alive but the main thread stopped
//                     answering - caught by the stall watchdog below and
//                     recorded WHILE it happens, which no post-mortem can do.
//
// WHY A FILE and not UserDefaults: a signal handler may call almost nothing,
// and `write(2)` on a file descriptor opened in advance is one of the few
// things it may. The same fd carries the ordinary breadcrumbs, so the crash
// line lands in sequence with everything before it rather than in a second
// store that has to be correlated by hand.
//
// COST: one `write(2)` of a short line per breadcrumb, at events that already
// cost a kernel decode or a style transition. Nothing polls except the stall
// watchdog, which is one timer tick a second on a background queue.
import Foundation
import Darwin

/// The one-line records a session leaves behind.
public struct FlightNote: Equatable {
    /// Seconds since that session started.
    public let at: Double
    /// The event: a short slug (`open`, `decode`, `seal`, `send`, `stall`…).
    public let event: String
    /// Memory footprint in MB at that moment - the number iOS actually kills
    /// on (`phys_footprint`), not resident size.
    public let mb: Double
    /// Whatever the call site added ("4p", "sig 11", "1.4s").
    public let detail: String
}

/// One recovered session.
public struct FlightSession: Equatable {
    public let notes: [FlightNote]
    /// Did it write its own goodbye line? False means it was killed, crashed,
    /// or was torn down without warning (which Messages does routinely, so an
    /// abrupt end is evidence to read, not proof of a bug on its own).
    public let endedCleanly: Bool
    /// The highest footprint any breadcrumb saw.
    public var peakMB: Double { notes.map(\.mb).max() ?? 0 }
    /// The signal a crash handler caught, if one did.
    public var crashSignal: Int? {
        notes.last { $0.event == "crash" }.flatMap { Int($0.detail.split(separator: " ").last ?? "") }
    }
    /// Did the main thread stop answering while this session was alive?
    public var stalled: Bool { notes.contains { $0.event == "stall" } }
}

public enum FlightRecorder {

    // MARK: - Writing

    /// Open the trail for THIS session, after rotating the previous one aside.
    /// Called once, as early as the extension can manage (see
    /// MessagesViewController.viewDidLoad) - everything before it is invisible,
    /// so nothing expensive should happen first.
    public static func begin(_ detail: String = "") {
        queue.sync {
            guard fd < 0 else { return }
            guard let dir = containerURL() else { return }
            let cur = dir.appendingPathComponent(fileName)
            let prev = dir.appendingPathComponent(prevFileName)
            // The previous session becomes THE previous session, whatever it
            // was, before a single byte of this one is written. Rotating rather
            // than appending is what keeps the file bounded without a reader
            // ever having to trim it.
            try? FileManager.default.removeItem(at: prev)
            try? FileManager.default.moveItem(at: cur, to: prev)
            fd = open(cur.path, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
            guard fd >= 0 else { return }
            start = Date()
            writeLine("begin", detail)
            installCrashHandlers()
        }
        startStallWatchdog()
    }

    /// A breadcrumb. Safe from any thread and cheap enough to sit on a path
    /// that runs per move; NOT safe to call from a signal handler (that is what
    /// `crashLine` is for).
    public static func note(_ event: String, _ detail: String = "") {
        queue.async { writeLine(event, detail) }
    }

    /// The session is ending on purpose. Anything that does not reach this is
    /// reported as having ended abruptly.
    public static func end(_ detail: String = "") {
        queue.sync { writeLine(endEvent, detail) }
    }

    /// Footprint in MB - `phys_footprint`, the figure iOS jetsams against.
    /// Public because the diagnostics panel shows the LIVE number too, not just
    /// the recorded ones.
    public static func footprintMB() -> Double {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size
                                           / MemoryLayout<natural_t>.size)
        let kr = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        guard kr == KERN_SUCCESS else { return -1 }
        return Double(info.phys_footprint) / (1024 * 1024)
    }

    // MARK: - Reading

    /// The session before this one, or nil if there is no trail to read (a
    /// first launch, or a build that never wrote one).
    public static func previousSession() -> FlightSession? {
        guard let dir = containerURL() else { return nil }
        return parse(dir.appendingPathComponent(prevFileName))
    }

    /// This session so far - what the panel shows under the previous one, and
    /// the only view available when the thing being diagnosed has not happened
    /// yet.
    public static func currentSession() -> FlightSession? {
        guard let dir = containerURL() else { return nil }
        return parse(dir.appendingPathComponent(fileName))
    }

    /// Forget both trails. Offered in the panel so the owner can clear a stale
    /// report after reading it, rather than wondering whether the crash on
    /// screen is the one that just happened.
    public static func reset() {
        queue.sync {
            if fd >= 0 { close(fd); fd = -1 }
            guard let dir = containerURL() else { return }
            try? FileManager.default.removeItem(at: dir.appendingPathComponent(fileName))
            try? FileManager.default.removeItem(at: dir.appendingPathComponent(prevFileName))
        }
    }

    /// The human-readable dump, as the panel renders it. A string rather than a
    /// view so the same text can be read in a test - the format IS the feature
    /// here, and a format nothing checks rots.
    public static func report(previous: FlightSession?, current: FlightSession?) -> String {
        var out: [String] = []
        out.append(String(format: "now  %.1f MB", footprintMB()))
        if let p = previous {
            out.append("")
            out.append("── previous session ──")
            out.append(verdict(p))
            out.append(contentsOf: p.notes.map(line))
        } else {
            out.append("")
            out.append("no previous session recorded")
        }
        if let c = current, !c.notes.isEmpty {
            out.append("")
            out.append("── this session ──")
            out.append(contentsOf: c.notes.map(line))
        }
        return out.joined(separator: "\n")
    }

    /// What the trail SAYS happened, in one line. Deliberately hedged where the
    /// evidence is circumstantial: a memory kill leaves no direct trace (it is
    /// a SIGKILL), so the most this can honestly do is name it as the reading
    /// that fits.
    public static func verdict(_ s: FlightSession) -> String {
        if let sig = s.crashSignal { return "CRASHED - signal \(sig), peak \(mb(s.peakMB))" }
        if s.endedCleanly {
            return s.stalled ? "closed normally, but STALLED first - peak \(mb(s.peakMB))"
                             : "closed normally - peak \(mb(s.peakMB))"
        }
        if s.stalled { return "ENDED ABRUPTLY after a main-thread stall - peak \(mb(s.peakMB))" }
        if s.notes.contains(where: { $0.event == "memory-warning" }) {
            return "ENDED ABRUPTLY after a memory warning - peak \(mb(s.peakMB))"
                 + " (reads as the extension's memory limit)"
        }
        return "ended abruptly - peak \(mb(s.peakMB))"
             + " (no crash signal; a memory kill or an ordinary teardown both look like this)"
    }

    /// Is the previous session worth putting in front of the owner unprompted?
    /// A plain abrupt end is NOT: Messages tears the extension down whenever it
    /// feels like it, so raising a banner on every one of those would train the
    /// owner to ignore the banner that matters. A crash, a stall, or an abrupt
    /// end that a memory warning led up to are all things that should not
    /// happen.
    public static func isAlarming(_ s: FlightSession) -> Bool {
        if s.crashSignal != nil || s.stalled { return true }
        return !s.endedCleanly && s.notes.contains { $0.event == "memory-warning" }
    }

    // MARK: - The stall watchdog
    //
    // The one thing no post-mortem can recover: whether the extension was DEAD
    // or merely not answering. A background timer notes the time, hops to the
    // main queue to have it cleared, and if a later tick finds an uncleared
    // stamp older than `stallAfter`, the main thread has been busy or blocked
    // for that long and the trail says so - while it is still happening, which
    // is the only moment the fact exists.

    /// How long the main thread may be unresponsive before it counts.
    /// Deliberately well above a slow frame or a texture decode: this is for
    /// "the drawer stopped responding", not for jank.
    public static let stallAfter: TimeInterval = 2.0

    private static func startStallWatchdog() {
        watchQueue.async {
            guard watchdog == nil else { return }
            let t = DispatchSource.makeTimerSource(queue: watchQueue)
            t.schedule(deadline: .now() + 1, repeating: 1)
            t.setEventHandler {
                let now = Date()
                if let sent = pingSentAt, now.timeIntervalSince(sent) >= stallAfter {
                    // Still outstanding, and old. Record once per stall, not
                    // once per tick, or a 30-second freeze writes 30 lines and
                    // pushes the trail that explains it off the end.
                    if !stallReported {
                        stallReported = true
                        queue.async {
                            writeLine("stall", String(format: "main thread %.1fs",
                                                      now.timeIntervalSince(sent)))
                        }
                    }
                    return
                }
                guard pingSentAt == nil else { return }   // one in flight, not yet late
                pingSentAt = now
                DispatchQueue.main.async {
                    watchQueue.async {
                        if stallReported, let sent = pingSentAt {
                            let held = Date().timeIntervalSince(sent)
                            queue.async { writeLine("resumed", String(format: "after %.1fs", held)) }
                        }
                        stallReported = false
                        pingSentAt = nil
                    }
                }
            }
            t.resume()
            watchdog = t
        }
    }

    // MARK: - Crash handlers
    //
    // What they can and cannot catch. SIGSEGV/SIGBUS are a kernel-side memory
    // bug; SIGILL/SIGTRAP are how Swift ends a process on a failed precondition,
    // a nil force-unwrap or an out-of-range index; SIGABRT is an uncaught ObjC
    // exception's tail. SIGKILL - the memory kill - is NOT catchable by anyone,
    // which is precisely why the trail exists.
    //
    // The handler writes ONE fixed line with `write(2)` and then restores the
    // default disposition and re-raises, so iOS still produces its own crash
    // report. Nothing else is safe to do here: no allocation, no Foundation, no
    // Swift runtime beyond what this compiles to.

    private static func installCrashHandlers() {
        guard !handlersInstalled else { return }
        handlersInstalled = true
        flightCrashFD = fd
        for sig in [SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP] {
            signal(sig, flightCrashHandler)
        }
        NSSetUncaughtExceptionHandler { _ in flightWriteCrashLine(0) }
    }

    // MARK: - Guts

    private static let fileName = "flight.log"
    private static let prevFileName = "flight.prev.log"
    private static let endEvent = "end"
    /// Every write is funnelled through one serial queue, so a breadcrumb from
    /// the kernel actor and one from the main thread cannot interleave halfway
    /// through a line.
    private static let queue = DispatchQueue(label: "cards.foolish.flight")
    private static let watchQueue = DispatchQueue(label: "cards.foolish.flight.watch")
    private static var fd: Int32 = -1
    private static var start = Date()
    private static var handlersInstalled = false
    private static var watchdog: DispatchSourceTimer?
    private static var pingSentAt: Date?
    private static var stallReported = false

    /// The line format, which the parser and the panel both depend on:
    ///   `<seconds> <event> <mb> <detail>`
    /// Space-separated with the free-text detail last, so a detail containing
    /// spaces cannot shift a field.
    private static func writeLine(_ event: String, _ detail: String) {
        guard fd >= 0 else { return }
        let s = String(format: "%.2f %@ %.1f %@\n", Date().timeIntervalSince(start),
                       event, footprintMB(), detail)
        _ = s.withCString { write(fd, $0, strlen($0)) }
    }

    /// Where the trail lives. The App Group when there is one - it must survive
    /// the extension being killed and be readable by the next launch - and the
    /// caches directory when there is not, which is the harness, the unit tests,
    /// and any build whose entitlements have not been set up. Falling back
    /// rather than going silent matters: a recorder that quietly records nothing
    /// wherever it is not entitled is worse than no recorder, because the empty
    /// panel reads as "nothing went wrong".
    static func containerURL() -> URL? {
        if let o = directoryOverride { return o }
        if let g = FileManager.default.containerURL(
            forSecurityApplicationGroupIdentifier: MessageGameStore.defaultSuiteName) { return g }
        return FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first
    }

    /// Tests point the trail at a temp directory of their own; nothing else sets
    /// this. Not `#if DEBUG` because the tests build against the same
    /// configuration the app ships in.
    static var directoryOverride: URL?

    /// Drop the descriptor WITHOUT writing a goodbye line - which is what being
    /// killed looks like from the file's side, and the only way a test can
    /// produce the case the whole feature exists for. Deliberately not public:
    /// nothing in the app should ever want this.
    static func abandonForTesting() {
        queue.sync {
            if fd >= 0 { close(fd); fd = -1 }
            flightCrashFD = -1
            handlersInstalled = false
        }
    }

    private static func parse(_ url: URL) -> FlightSession? {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
        var notes: [FlightNote] = []
        var clean = false
        for raw in text.split(separator: "\n") {
            // The crash handler's line is the one that does not carry a clock
            // (it may not format one), so it is recognised by its marker.
            if raw.hasPrefix("+ ") {
                let d = String(raw.dropFirst(2))
                notes.append(FlightNote(at: notes.last?.at ?? 0, event: "crash",
                                        mb: notes.last?.mb ?? 0, detail: d))
                continue
            }
            let f = raw.split(separator: " ", maxSplits: 3, omittingEmptySubsequences: false)
            guard f.count >= 3, let at = Double(f[0]), let mb = Double(f[2]) else { continue }
            let event = String(f[1])
            if event == endEvent { clean = true }
            notes.append(FlightNote(at: at, event: event, mb: mb,
                                    detail: f.count > 3 ? String(f[3]) : ""))
        }
        guard !notes.isEmpty else { return nil }
        // Cap what a reader has to look at. The END of a trail is the part that
        // says what happened, so an over-long session drops its beginning.
        if notes.count > maxNotes { notes.removeFirst(notes.count - maxNotes) }
        return FlightSession(notes: notes, endedCleanly: clean)
    }

    /// How many breadcrumbs a report shows. Enough to cover a few moves and the
    /// lifecycle around them; small enough to read on a phone.
    static let maxNotes = 60

    private static func line(_ n: FlightNote) -> String {
        let d = n.detail.isEmpty ? "" : "  \(n.detail)"
        return String(format: "%6.2fs %5.1fMB  %@%@", n.at, n.mb, n.event, d)
    }

    private static func mb(_ v: Double) -> String { String(format: "%.1f MB", v) }
}

// MARK: - The signal handler, at file scope
//
// A C function pointer can only be formed from a closure that captures nothing,
// and a Swift `static` referenced from inside one counts as a capture - so the
// handler and the descriptor it writes to live out here, as plain globals.
// Which is also the more honest place for them: what a signal handler may touch
// is exactly "a global word and `write(2)`", and nothing about that should look
// like ordinary code inside a type.

/// The trail's descriptor, copied for the handler's use at install time. Its own
/// global rather than a read of the recorder's, because a handler must not read
/// a variable another thread may be reassigning.
private var flightCrashFD: Int32 = -1

/// Async-signal-safe: a fixed stack buffer and one `write`. The seconds-since-
/// start that ordinary breadcrumbs carry are skipped - formatting a Double is
/// not something a handler may do - so a crash line reads `+ crash sig 11`, and
/// its POSITION in the file is its place in time.
private func flightWriteCrashLine(_ sig: Int32) {
    guard flightCrashFD >= 0 else { return }
    var buf = [CChar](repeating: 0, count: 32)
    var n = 0
    for c in Array("+ crash sig ".utf8) { buf[n] = CChar(bitPattern: c); n += 1 }
    var v = Int(sig)
    if v == 0 {
        buf[n] = 48; n += 1
    } else {
        var digits = [CChar]()
        while v > 0 { digits.append(CChar(48 + v % 10)); v /= 10 }
        for d in digits.reversed() { buf[n] = d; n += 1 }
    }
    buf[n] = 10; n += 1
    let count = n
    _ = buf.withUnsafeBufferPointer { write(flightCrashFD, $0.baseAddress, count) }
}

/// Write the line, then put the default disposition back and re-raise, so iOS
/// still produces its own crash report. Swallowing the signal would trade a
/// real report for a one-line note, which is a bad trade.
private let flightCrashHandler: @convention(c) (Int32) -> Void = { s in
    flightWriteCrashLine(s)
    signal(s, SIG_DFL)
    raise(s)
}
