// Packed personalized-game responses (docs/PACKED_WIRE_CUTOVER.md), shared by
// the get_game and get_my_games edge functions and — because the edge
// entrypoints themselves are behind serve() — directly testable in e2e.
//
// The caller's view of a dealt game leaves the kernel as a MASKED view blob
// (wasm_view_serialize) wrapped with the identity roster JSON; a game that
// cannot be served packed (no blob, or a WAITING lobby — lobbies are
// assembled from membership rows, never from a blob) falls back to the
// personalize_game JSON, byte-wrapped for the list envelope.
import { GAME_STATUS, Game, PrivatePlayer } from '@api/core/types.ts';
import { encodeGameResponse, PackedGameRoster } from '@sdk/ts/wire/view.ts';

// A lazy import that resolves ONCE. The deferral is deliberate (a cold start must
// not pull the rules-wasm embed it never uses); re-RESOLVING the specifier on
// every call was not - see the note on `lazy` in
// server/impls/supabase/functions/_shared/adapter/utils.ts.
const lazy = <T>(load: () => Promise<T>): (() => Promise<T>) => {
    let mod: Promise<T> | undefined;
    return () => (mod ??= load());
};
const engineMod = lazy(() => import('@sdk/ts/wasm/engine.ts'));
const codecMod = lazy(() => import('./replay/codec.ts'));


export interface GamesRowForView {
    id: string;
    name: string;
    status: string;
    version: number | null;
    state: string | null;
    players: { player_id: string; name: string; is_ai: boolean }[];
    good_players: string[] | null;
    good_timestamp: number | null;
}

// Reconstruct the minimal Game the personalizer needs directly from a games
// row, for any game that CAN'T be served packed — a WAITING lobby (no deal yet)
// or a finished/legacy game with no state blob. It reads only the row's roster +
// board columns: no player_hands read, no loadCompleteGame, no supabase-js. Hands
// are empty, which is exactly what loadCompleteGame's non-blob branch also
// produces (post-cutover the JSONB hand tables aren't written during play), so
// personalize_game(gameViewFromRow(row)) matches personalize_game(
// loadCompleteGame(id)) for these games (see packed_review_gaps). This is what
// keeps the list build loop O(1) per game instead of an N+1 of per-game loads.
export interface RowGameView extends GamesRowForView {
    players: (GamesRowForView['players'][number] & { status?: string; hand_length?: number; strategy_key?: string })[];
    discard_pile_length?: number;
    flipped?: unknown;
    power_suit?: number;
    first_attacker?: number;
    defender?: number;
    table_battles?: unknown[];
    elimination_order?: string[];
}

export function gameViewFromRow(row: RowGameView): Game {
    const players: PrivatePlayer[] = (row.players ?? []).map((p) => ({
        player_id: p.player_id,
        name: p.name,
        status: (p.status ?? 'ready') as PrivatePlayer['status'],
        is_ai: p.is_ai,
        hand: [], // no blob → no hidden hands to show (empty, as loadCompleteGame also yields here)
        hand_length: p.hand_length ?? 0,
        awaiting_attack: false,
        strategy_key: p.strategy_key ?? 'human',
    }));
    return {
        id: row.id,
        name: row.name,
        version: row.version ?? 0,
        deck: [],
        deck_length: 0,
        discard_pile_length: row.discard_pile_length ?? 0,
        flipped: (row.flipped ?? null) as Game['flipped'],
        players,
        status: row.status as Game['status'],
        power_suit: row.power_suit ?? 0,
        first_attacker: row.first_attacker ?? 0,
        defender: row.defender ?? 0,
        table_battles: (row.table_battles ?? []) as Game['table_battles'],
        elimination_order: row.elimination_order ?? [],
        good_timestamp: row.good_timestamp ?? null,
        good_players: row.good_players ?? [],
        logs: [],
    };
}

// One game -> the packed single-game envelope, or null when the row must be
// served as JSON (no blob / lobby). The status guard mirrors loadCompleteGame:
// a WAITING game never loads from a blob (a stale one — pre-fix `continue`
// resets — would serve the finished session's state).
export async function buildPackedGameBytes(row: GamesRowForView, userId: string): Promise<Uint8Array | null> {
    if (!row.state || row.status === GAME_STATUS.WAITING) return null;
    const seat = row.players.findIndex(p => p.player_id === userId);
    const roster: PackedGameRoster = {
        id: row.id,
        name: row.name,
        status: row.status, // column-authoritative over the blob's copy
        players: row.players.map(p => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai })),
        good_players: row.good_players || [],
        good_timestamp: row.good_timestamp || null,
    };
    // Lazy import: only a dealt game pulls the rules-wasm embed.
    const { serializeViewBlob } = await engineMod();
    const { hexToBytes } = await codecMod();
    const viewBlob = serializeViewBlob(hexToBytes(row.state), seat);
    return encodeGameResponse(row.version ?? 0, seat, roster, viewBlob);
}
