// Per-isolate packed-state cache (docs/PACKED_WIRE_CUTOVER.md, "end to end").
//
// The packed action path needs exactly one thing from the DB before the
// kernel runs: the current blob + roster at a known version. This isolate
// usually WROTE that state a moment ago (the previous human move, or the bot
// loop driving the same game via EdgeRuntime.waitUntil) — so remember it and
// skip the load round-trip. Correctness never depends on freshness: the
// commit is CAS-fenced on the version, so a stale entry costs one conflict
// and a reload, never a wrong write. commitGame keeps the cache current for
// EVERY dealt commit (packed and JS paths alike) and evicts on any
// non-PLAYING transition; cross-isolate writers surface as conflicts.
import { GAME_STATUS, Game } from './types.ts';

export interface CachedGame {
    version: number;
    stateHex: string; // \x-prefixed, exactly as commit_game stores it
    name: string;
    status: string;
    players: { player_id: string; name: string; is_ai: boolean }[];
    good_players: string[];
    good_timestamp: number | null;
}

const CACHE_CAP = 256;
const cache = new Map<string, CachedGame>();

export function getCachedGame(gameId: string): CachedGame | undefined {
    return cache.get(gameId);
}

export function invalidateCachedGame(gameId: string): void {
    cache.delete(gameId);
}

// Called by commitGame after a successful version-gated write.
export function noteCommittedGame(game: Game, version: number, stateHex: string | null): void {
    if (!stateHex || game.status !== GAME_STATUS.PLAYING) {
        // Game over / lobby reset: the next read must see the columns
        // (finalize, moot checks, lobby assembly) — never a cached blob.
        cache.delete(game.id);
        return;
    }
    if (!cache.has(game.id) && cache.size >= CACHE_CAP) {
        // Evict the oldest entry (Map preserves insertion order).
        const oldest = cache.keys().next().value;
        if (oldest !== undefined) cache.delete(oldest);
    }
    cache.delete(game.id); // reinsert to refresh recency
    cache.set(game.id, {
        version,
        stateHex,
        name: game.name,
        status: game.status,
        players: game.players.map(p => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai })),
        good_players: [...game.good_players],
        good_timestamp: game.good_timestamp,
    });
}

// Test hook.
export function __clearGameCache(): void { cache.clear(); }
