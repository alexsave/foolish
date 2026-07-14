// GameFeed.swift — the realtime subscription (§8.1, §16.D4). The AUTHORITATIVE
// masked state reaches the client as a `player_views` row (the web's `pv-` feed
// is Postgres change notifications on that table, decoded per row — verified from
// src/contexts/ServerContext.tsx). Stage C1 renders from that full state; the
// `animation_events` broadcast stream is animation polish (Stage C2, deferred).
//
// So this subscribes Postgres changes on `player_views` (players, filtered by
// player_id) or `spectator_views` (spectators, filtered by game_id), gates each
// row on its committed `version`, and hands the bare-hex `view` to the caller,
// which decodes it through PackedGame (envelope → kernel).
//
// The supabase-swift API used here was verified against the SDK source
// (2026-07): channel.postgresChange(InsertAction.self, schema:table:
// filter: RealtimePostgresFilter) -> AsyncStream, `await channel.subscribe()`
// (non-throwing), and action.decodeRecord(as:decoder:).

import Foundation
import Supabase

@MainActor
public final class GameFeed {
    private var channel: RealtimeChannelV2?
    private var lastAppliedVersion: Int?

    /// Delivers (packed view hex, committed version) for each gated row.
    private let onRow: (String, Int?) -> Void

    public init(onRow: @escaping (String, Int?) -> Void) {
        self.onRow = onRow
    }

    /// A `player_views` / `spectator_views` row — only the fields we need.
    private struct ViewRow: Decodable { let view: String; let version: Int? }

    /// Player feed: `player_views` rows for this user (channel `pv-<user_id>`).
    public func subscribe(userId: UUID) async {
        let uid = userId.uuidString.lowercased()
        await subscribeTable("pv-\(uid)", table: "player_views", filter: .eq("player_id", value: uid))
    }

    /// Spectator feed: `spectator_views` rows for this game (channel `game-<id>`).
    public func subscribePublic(gameId: String) async {
        await subscribeTable("game-\(gameId)", table: "spectator_views", filter: .eq("game_id", value: gameId))
    }

    private func subscribeTable(_ channelName: String, table: String, filter: RealtimePostgresFilter) async {
        guard let client = Backend.shared.client else { return }
        let ch = client.realtimeV2.channel(channelName)
        let inserts = ch.postgresChange(InsertAction.self, schema: "public", table: table, filter: filter)
        let updates = ch.postgresChange(UpdateAction.self, schema: "public", table: table, filter: filter)
        await ch.subscribe()
        channel = ch
        // Both InsertAction and UpdateAction expose decodeRecord(as:decoder:).
        Task { [weak self] in
            for await a in inserts {
                if let row = try? a.decodeRecord(as: ViewRow.self, decoder: JSONDecoder()) { self?.apply(row) }
            }
        }
        Task { [weak self] in
            for await a in updates {
                if let row = try? a.decodeRecord(as: ViewRow.self, decoder: JSONDecoder()) { self?.apply(row) }
            }
        }
    }

    private func apply(_ row: ViewRow) {
        guard !row.view.isEmpty else { return }
        if VersionGate.shouldDrop(lastApplied: lastAppliedVersion, incoming: row.version) { return }
        if let v = row.version { lastAppliedVersion = v }
        onRow(row.view, row.version)
    }

    public func unsubscribe() async {
        if let channel { await channel.unsubscribe() }
        channel = nil
    }

    /// Reset the gate on foreground resync so a refetched snapshot always applies.
    public func resetVersionGate() { lastAppliedVersion = nil }
}
