// GameFeed.swift — the realtime subscription (§8.1, §16.D4). Subscribes the
// channels, applies the version gate, and delivers the raw masked packed-view
// bytes (after the gate) to OnlineGame, which decodes them through the kernel.
// Stage C1: each broadcast carries the FULL resulting masked state, so we render
// from that — no optimistic mutation (§8.2).
//
// PROTOCOL SEAMS — TODO(D0) (docs/PROTOCOL.md §3-4): the broadcast EVENT NAME and
// the payload keys carrying the committed version and the packed view bytes must
// be pinned from the web client (src/state/RealtimeAnimationFeed.tsx,
// ServerContext.tsx). They are marked below. The channel names, the version gate,
// and the kernel decode (in OnlineGame) are final.

import Foundation
import Supabase

@MainActor
public final class GameFeed {
    private var channels: [RealtimeChannelV2] = []
    private var lastAppliedVersion: Int?

    /// Delivers (packed view bytes, viewer seat, version) after the version gate.
    private let onPacked: (Data, Int, Int?) -> Void

    public init(onPacked: @escaping (Data, Int, Int?) -> Void) {
        self.onPacked = onPacked
    }

    /// Personal feed (`pv-<user_id>`) + per-player animation feed
    /// (`gu-<gameId>-<user_id>`). `mySeat` is the viewer whose hand is real.
    public func subscribe(gameId: String, userId: UUID, mySeat: Int) async {
        guard let client = Backend.shared.client else { return }
        let uid = userId.uuidString.lowercased()
        let pv = client.realtimeV2.channel("pv-\(uid)")
        await bind(pv, viewer: mySeat)
        let gu = client.realtimeV2.channel("gu-\(gameId)-\(uid)")
        await bind(gu, viewer: mySeat)
        channels = [pv, gu]
        for ch in channels { await ch.subscribe() }
    }

    /// Spectate the public feed (`game-<gameId>`), masked to counts (viewer -1).
    public func subscribePublic(gameId: String) async {
        guard let client = Backend.shared.client else { return }
        let pub = client.realtimeV2.channel("game-\(gameId)")
        await bind(pub, viewer: -1)   // VIEW_SPECTATOR
        channels = [pub]
        for ch in channels { await ch.subscribe() }
    }

    /// Feed a sequence (from a broadcast or a resync snapshot) through the gate.
    public func feed(version: Int?, packedViewHex: String, viewer: Int) {
        if VersionGate.shouldDrop(lastApplied: lastAppliedVersion, incoming: version) { return }
        if let version { lastAppliedVersion = version }
        guard let bytes = Self.hexToData(packedViewHex) else { return }
        onPacked(bytes, viewer, version)
    }

    public func unsubscribe() async {
        for ch in channels { await ch.unsubscribe() }
        channels = []
    }

    /// Reset the gate on foreground resync so a refetched snapshot always applies.
    public func resetVersionGate() { lastAppliedVersion = nil }

    // MARK: - broadcast binding

    private func bind(_ channel: RealtimeChannelV2, viewer: Int) async {
        // TODO(D0): event name + payload keys ("version", "view") must match the
        // web broadcast. The gate + downstream kernel decode are final.
        let stream = channel.broadcastStream(event: "sequence")
        Task { [weak self] in
            for await message in stream {
                guard let self else { break }
                let version = message["version"]?.intValue
                let hex = message["view"]?.stringValue ?? ""
                if !hex.isEmpty { self.feed(version: version, packedViewHex: hex, viewer: viewer) }
            }
        }
    }

    static func hexToData(_ hex: String) -> Data? {
        let s = hex.hasPrefix("0x") ? String(hex.dropFirst(2)) : hex
        guard s.count % 2 == 0 else { return nil }
        var out = Data(capacity: s.count / 2)
        var idx = s.startIndex
        while idx < s.endIndex {
            let next = s.index(idx, offsetBy: 2)
            guard let b = UInt8(s[idx..<next], radix: 16) else { return nil }
            out.append(b)
            idx = next
        }
        return out
    }
}
