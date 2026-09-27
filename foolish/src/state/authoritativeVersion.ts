// The newest authoritative games.version the client has applied, per game.
//
// AnimationContext owns the live broadcast/version gate (lastAppliedVersionRef);
// ServerContext dispatches the moves. The server's round-boundary guard
// (docs/WEB_RACE_BUG_HANDOFF.md) needs each move stamped with the version the
// client composed it against (intent_version), so the two contexts share this
// tiny store instead of prop-drilling a ref across the tree. Pure, no React —
// AnimationContext writes it wherever it raises its applied version; the active
// game's version is animation-owned (player_views skips it), so this is the only
// place ServerContext can read a fresh authoritative version at tap time.

const versions = new Map<string, number>();

// Record an applied authoritative version. Monotonic: a late, out-of-order
// broadcast can never lower it (mirrors the animation feed's version gate).
export function noteAuthoritativeVersion(gameId: string | null | undefined, version: number | null | undefined): void {
    if (!gameId || typeof version !== 'number') return;
    const prev = versions.get(gameId);
    versions.set(gameId, prev === undefined ? version : Math.max(prev, version));
}

// The version to stamp an outgoing move with, or undefined if we've never seen
// an authoritative version for this game (then the move rides the legacy v1
// envelope and is simply not round-guarded).
export function authoritativeVersion(gameId: string | null | undefined): number | undefined {
    return gameId ? versions.get(gameId) : undefined;
}

export function forgetAuthoritativeVersion(gameId: string): void {
    versions.delete(gameId);
}

// Test hook.
export function __clearAuthoritativeVersions(): void { versions.clear(); }
