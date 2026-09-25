// DevFlags.swift - the one reader for DEBUG dev files in an App Group.
//
// NEVER COMPILED INTO A SHIPPING BUILD: the whole file is inside
// `#if DEBUG || SOLO_TESTING`, and a TestFlight or App Store build defines
// neither, so a Release binary has no dev-file reader, no `dev.` file name and
// nothing a stray file on a customer's phone could switch on.
// (shared/tools/release_strings.sh checks that on the built product.)
//
// A dev flag is a FILE in the App Group, not a UserDefaults key: a `defaults
// write` from outside the sandbox lands in the wrong domain, and cfprefsd
// caches App Group preferences until the device reboots. A file is read by the
// process that wants it, when it wants it - the rig writes it with `cp` on a
// simulator and `devicectl device copy to` on a phone.
//
// WHAT IS CACHED AND WHAT IS NOT. The group's DIRECTORY is looked up once per
// process: `containerURL(forSecurityApplicationGroupIdentifier:)` takes dyld's
// loader lock and an XPC round trip, and a `sample` of an opening drawer put
// ~40 ms of main-thread time in it under ONE view body that asked whether the
// ruler was on. The directory of an App Group never moves while the process
// lives. The FILES are read fresh on every call; a product that wants a value
// once per process (because it is asked for on every frame) keeps it in a
// `static let` of its own.
//
// Each product keeps its own KEYS and what they mean; this file only knows how
// a dev file is found, read, written and parsed.

#if DEBUG || SOLO_TESTING
import Foundation

public struct DevFlags: Sendable {
    public let group: String

    public init(group: String) { self.group = group }

    /// The App Group's directory, or nil when the process has no such group.
    public var directory: URL? { Self.container(group) }

    /// Where the dev file `name` lives (whether or not it exists).
    public func url(_ name: String) -> URL? { directory?.appendingPathComponent(name) }

    /// Is the dev file there? An empty file is a flag that is ON.
    public func exists(_ name: String) -> Bool {
        guard let u = url(name) else { return false }
        return FileManager.default.fileExists(atPath: u.path)
    }

    /// The file's contents with surrounding whitespace trimmed - "" for an
    /// empty file, nil when it is absent or unreadable.
    public func raw(_ name: String) -> String? {
        guard let u = url(name), let s = try? String(contentsOf: u, encoding: .utf8) else { return nil }
        return s.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// Like `raw`, but an empty file reads as absent.
    public func string(_ name: String) -> String? {
        guard let s = raw(name), !s.isEmpty else { return nil }
        return s
    }

    public func int(_ name: String) -> Int? { raw(name).flatMap { Int($0) } }

    public func double(_ name: String) -> Double? { raw(name).flatMap { Double($0) } }

    /// Read the file and delete it: a one-shot message from the rig.
    public func take(_ name: String) -> String? {
        guard let u = url(name), let s = try? String(contentsOf: u, encoding: .utf8) else { return nil }
        try? FileManager.default.removeItem(at: u)
        return s.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// Write `value` to the file, or remove the file when it is nil or empty.
    /// Best-effort: a failed write costs the driver a retry, never a frame.
    public func write(_ value: String?, to name: String) {
        guard let u = url(name) else { return }
        if let value, !value.isEmpty {
            try? value.write(to: u, atomically: true, encoding: .utf8)
        } else {
            try? FileManager.default.removeItem(at: u)
        }
    }

    /// Write raw bytes (a receipt a shell compares byte for byte).
    public func write(bytes: Data, to name: String) {
        guard let u = url(name) else { return }
        try? bytes.write(to: u)
    }

    /// `key=value` pairs separated by spaces or newlines, in file order.
    /// Anything without an `=` is skipped.
    public func pairs(_ name: String) -> [(key: String, value: String)] {
        guard let u = url(name), let s = try? String(contentsOf: u, encoding: .utf8) else { return [] }
        var out: [(key: String, value: String)] = []
        for pair in s.split(whereSeparator: { $0 == " " || $0 == "\n" }) {
            let kv = pair.split(separator: "=", maxSplits: 1)
            guard kv.count == 2 else { continue }
            out.append((String(kv[0]), String(kv[1])))
        }
        return out
    }

    /// `key=0|1` pairs (also true/false, on/off); an unparseable value is
    /// skipped, so the caller's shipping default stands for that key.
    public func bools(_ name: String) -> [String: Bool] {
        var out: [String: Bool] = [:]
        for (k, v) in pairs(name) {
            switch v {
            case "1", "true", "on": out[k] = true
            case "0", "false", "off": out[k] = false
            default: continue
            }
        }
        return out
    }

    // MARK: - the directory, once per process per group

    private static func container(_ group: String) -> URL? {
        lock.lock(); defer { lock.unlock() }
        if let u = containers[group] { return u }
        let u = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: group)
        containers[group] = u
        return u
    }
    nonisolated(unsafe) private static var containers: [String: URL] = [:]
    private static let lock = NSLock()
}
#endif
