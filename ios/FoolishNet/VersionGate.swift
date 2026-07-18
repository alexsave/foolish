// VersionGate.swift — the live-broadcast ordering gate (§8.1, §16.D4). Direct
// port of shouldDropStaleSequence (src/state/clientReconcile.ts:44-52).
//
// Broadcasts are fired un-awaited over per-call channels, so realtime latency
// can deliver them out of order. Each carries the committed games.version; drop
// any whose version is at or below the newest already applied (it is strictly
// superseded — each sequence carries the full resulting state). Replay sequences
// have no version and are never gated.

public enum VersionGate {
    /// Returns true if the incoming sequence should be DROPPED as stale.
    public static func shouldDrop(lastApplied: Int?, incoming: Int?) -> Bool {
        guard let incoming else { return false }          // no version → never gated
        guard let lastApplied else { return false }
        return incoming <= lastApplied
    }
}
