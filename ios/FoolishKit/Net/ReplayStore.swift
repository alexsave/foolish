// ReplayStore.swift — the local saved-replays list (§16.C3). One JSON file in
// Application Support; newest first. No database framework (do NOT add
// SwiftData/CoreData — §16.C3). Lives in Net/ as it deals with shareable codes,
// but it touches no network.

import Foundation

public struct ReplayRecord: Codable, Equatable, Identifiable, Sendable {
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

    public init(filename: String = "replays.json") {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        url = dir.appendingPathComponent(filename)
    }

    public func all() -> [ReplayRecord] {
        queue.sync {
            guard let data = try? Data(contentsOf: url),
                  let list = try? JSONDecoder().decode([ReplayRecord].self, from: data) else { return [] }
            return list.sorted { $0.savedAt > $1.savedAt }
        }
    }

    /// Save (or move-to-top) a replay by code. De-dupes on the code.
    public func save(_ record: ReplayRecord) {
        queue.sync {
            var list = (try? Data(contentsOf: url)).flatMap { try? JSONDecoder().decode([ReplayRecord].self, from: $0) } ?? []
            list.removeAll { $0.code == record.code }
            list.insert(record, at: 0)
            if let data = try? JSONEncoder().encode(list) { try? data.write(to: url, options: .atomic) }
        }
    }

    public func delete(code: String) {
        queue.sync {
            var list = (try? Data(contentsOf: url)).flatMap { try? JSONDecoder().decode([ReplayRecord].self, from: $0) } ?? []
            list.removeAll { $0.code == code }
            if let data = try? JSONEncoder().encode(list) { try? data.write(to: url, options: .atomic) }
        }
    }
}
