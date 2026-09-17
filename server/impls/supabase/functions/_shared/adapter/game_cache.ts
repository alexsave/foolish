// Per-isolate row cache (docs/PACKED_WIRE_CUTOVER.md, "end to end";
// docs/C_GAME_SHAPE_MIGRATION.md Phase 4b).
//
// A table operation needs exactly one thing from the DB before the kernel runs:
// the row's state and roster blobs at a known version. This isolate usually
// WROTE that row a moment ago (the previous human move, or the bot loop driving
// the same game via EdgeRuntime.waitUntil), so it remembers what it committed
// and skips the load round-trip. Correctness never depends on freshness: every
// commit is CAS-fenced on the version, so a stale entry costs one conflict and a
// reload, never a wrong write. The cache holds opaque bytes and bookkeeping
// columns only; what they mean is the kernel's.

export interface CachedRow {
    version: number;
    // The version at which the current round began (games.round_epoch), for the
    // kernel's round guard. Written by the same commit as `version`, so a stale
    // entry pairs a version with ITS epoch: it can lag, never mislead.
    roundEpoch: number;
    stateHex: string;   // \x-prefixed, exactly as commit_table stores it
    rosterHex: string;  // \x-prefixed
    gameSeed: string | null;
}

const CACHE_CAP = 256;
const cache = new Map<string, CachedRow>();

export function getCachedRow(gameId: string): CachedRow | undefined {
    return cache.get(gameId);
}

export function invalidateCachedRow(gameId: string): void {
    cache.delete(gameId);
}

// Called after a successful version-gated commit of a row that is in play.
// Rows outside play (a lobby, a finished game) are evicted instead: they are
// edited from many isolates, and the next read should see the table as stored.
export function noteCommittedRow(gameId: string, row: CachedRow | null): void {
    if (!row) {
        cache.delete(gameId);
        return;
    }
    if (!cache.has(gameId) && cache.size >= CACHE_CAP) {
        // Evict the oldest entry (Map preserves insertion order).
        const oldest = cache.keys().next().value;
        if (oldest !== undefined) cache.delete(oldest);
    }
    cache.delete(gameId); // reinsert to refresh recency
    cache.set(gameId, row);
}

// Test hook.
export function __clearGameCache(): void { cache.clear(); }
