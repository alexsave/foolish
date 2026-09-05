// ReplayStore.swift - the local saved-replays list (§16.C3). One file in
// Application Support; newest first. No database framework (do NOT add
// SwiftData/CoreData - §16.C3).
//
// PACKED BYTES, not JSON (owner: "I REALLY don't like JSON", and "turn the
// on-disk stores away from JSON too"). The rows are a bookmark INDEX rather
// than game state - `code` is the packed v6 share string and the rest is
// list-UI metadata - which is why this outlived the wire cutover; it is bytes
// now anyway, through the same PackedWriter/PackedReader the wire codecs use.
//
// A CLEAN CUT, with the owner's blessing ("just break in progress games if you
// need to"): the file is `replays.bin` and the old `replays.json` is never read
// again. It is also never DELETED - a user's old index stays on disk, so
// nothing is destroyed, it simply stops being listed. An unreadable file is no
// rows, the same thing a missing one is.
//
// Storage/, not Net/: it deals in shareable codes but touches no network, and
// the offline app + iMessage extension both need it.

import Foundation

public struct ReplayRecord: Equatable, Identifiable, Sendable {
    public let code: String
    public let savedAt: Date
    public let players: Int
    public let fool: Int          // fool seat, or -1 if unknown/mid-game
    public let myResult: String?  // "win" / "lose" / nil for spectated/imported

    public var id: String { code }

    public init(code: String, savedAt: Date, players: Int, fool: Int, myResult: String?) {
        self.code = code
        self.savedAt = savedAt
        self.players = players
        self.fool = fool
        self.myResult = myResult
    }
}

public final class ReplayStore {
    public static let shared = ReplayStore()

    private let url: URL
    private let queue = DispatchQueue(label: "cards.foolish.replaystore")

    public init(filename: String = "replays.bin") {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        url = dir.appendingPathComponent(filename)
    }

    public func all() -> [ReplayRecord] {
        queue.sync { Self.decode(try? Data(contentsOf: url)).sorted { $0.savedAt > $1.savedAt } }
    }

    /// Save (or move-to-top) a replay by code. De-dupes on the code.
    public func save(_ record: ReplayRecord) {
        queue.sync {
            var list = Self.decode(try? Data(contentsOf: url))
            list.removeAll { $0.code == record.code }
            list.insert(record, at: 0)
            write(list)
        }
    }

    public func delete(code: String) {
        queue.sync {
            var list = Self.decode(try? Data(contentsOf: url))
            list.removeAll { $0.code == code }
            write(list)
        }
    }

    private func write(_ list: [ReplayRecord]) {
        try? Self.encode(list).write(to: url, options: .atomic)
    }

    /// fmt(1) n(u16) then n x { code(text) savedAt(f64) players(1) fool(1,
    /// 0xFF none) result(1: 0 none, 1 win, 2 lose) }.
    static let format = 1

    static func encode(_ list: [ReplayRecord]) -> Data {
        var w = PackedWriter()
        w.u8(format)
        w.u16(min(list.count, 0xFFFF))
        for r in list.prefix(0xFFFF) {
            w.text(r.code)
            w.f64(r.savedAt.timeIntervalSince1970)
            w.u8(r.players)
            w.u8(r.fool < 0 || r.fool > 0xFE ? 0xFF : r.fool)
            w.u8(r.myResult == "win" ? 1 : (r.myResult == "lose" ? 2 : 0))
        }
        return w.data
    }

    /// A file that does not decode WHOLE is no rows. Half a bookmark list read
    /// as a whole one would silently lose the tail on the next save.
    static func decode(_ data: Data?) -> [ReplayRecord] {
        guard let data else { return [] }
        var r = PackedReader(data)
        guard r.u8() == format, let n = r.u16() else { return [] }
        var out: [ReplayRecord] = []
        out.reserveCapacity(n)
        for _ in 0..<n {
            guard let code = r.text(), let at = r.f64(), let players = r.u8(),
                  let fool = r.u8(), let result = r.u8() else { return [] }
            out.append(ReplayRecord(code: code, savedAt: Date(timeIntervalSince1970: at),
                                    players: players, fool: fool == 0xFF ? -1 : fool,
                                    myResult: result == 1 ? "win" : (result == 2 ? "lose" : nil)))
        }
        return out
    }
}
