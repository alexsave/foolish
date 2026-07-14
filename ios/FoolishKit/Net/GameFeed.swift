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
// NOTE (Mac compile pass): supabase-swift's realtimeV2 Postgres-change API
// (channel.postgresChange(_:schema:table:filter:), AnyAction/.record) is stable
// in 2.x; confirm the exact spelling against the resolved version.

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

    /// Player feed: `player_views` rows for this user (channel `pv-<user_id>`).
    public func subscribe(userId: UUID) async {
        guard let client = Backend.shared.client else { return }
        let uid = userId.uuidString.lowercased()
        await subscribeTable("pv-\(uid)", table: "player_views", filter: "player_id=eq.\(uid)")
    }

    /// Spectator feed: `spectator_views` rows for this game (channel `game-<id>`).
    public func subscribePublic(gameId: String) async {
        await subscribeTable("game-\(gameId)", table: "spectator_views", filter: "game_id=eq.\(gameId)")
    }

    private func subscribeTable(_ channelName: String, table: String, filter: String) async {
        guard let client = Backend.shared.client else { return }
        let ch = client.realtimeV2.channel(channelName)
        // Insert + update both carry the fresh row in `.record`.
        let inserts = ch.postgresChange(InsertAction.self, schema: "public", table: table, filter: filter)
        let updates = ch.postgresChange(UpdateAction.self, schema: "public", table: table, filter: filter)
        await ch.subscribe()
        channel = ch
        Task { [weak self] in for await a in inserts { self?.handle(a.record) } }
        Task { [weak self] in for await a in updates { self?.handle(a.record) } }
    }

    private func handle(_ record: [String: AnyJSON]) {
        let version = record["version"]?.intValue
        let hex = record["view"]?.stringValue ?? ""
        guard !hex.isEmpty else { return }
        if VersionGate.shouldDrop(lastApplied: lastAppliedVersion, incoming: version) { return }
        if let version { lastAppliedVersion = version }
        onRow(hex, version)
    }

    public func unsubscribe() async {
        if let channel { await channel.unsubscribe() }
        channel = nil
    }

    /// Reset the gate on foreground resync so a refetched snapshot always applies.
    public func resetVersionGate() { lastAppliedVersion = nil }
}
