// Packed personalized-game responses (docs/PACKED_WIRE_CUTOVER.md), shared by
// the get_game and get_my_games edge functions and — because the edge
// entrypoints themselves are behind serve() — directly testable in e2e.
//
// The caller's view of a dealt game leaves the kernel as a MASKED view blob
// (wasm_view_serialize) wrapped with the identity roster JSON; a game that
// cannot be served packed (no blob, or a WAITING lobby — lobbies are
// assembled from membership rows, never from a blob) falls back to the
// personalize_game JSON, byte-wrapped for the list envelope.
import { GAME_STATUS, Game, PrivatePlayer } from './types.ts';
import { encodeGameResponse, PackedGameRoster } from './wire/view.ts';

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

// A WAITING lobby row carries the roster + board columns but no dealt state
// (no blob). This reconstructs the minimal Game the personalizer needs directly
// from those columns — hands are empty pre-deal, so it reads no player_hands and
// pulls no supabase-js. The output is field-equivalent to loadCompleteGame's
// non-blob (lobby) assembly, so personalize_game(lobbyGameFromRow(row)) matches
// personalize_game(loadCompleteGame(id)) for a WAITING game (see packed_review_gaps).
export interface LobbyGameRow extends GamesRowForView {
    players: (GamesRowForView['players'][number] & { status?: string; hand_length?: number; strategy_key?: string })[];
    discard_pile_length?: number;
    flipped?: unknown;
    power_suit?: number;
    first_attacker?: number;
    defender?: number;
    table_battles?: unknown[];
    elimination_order?: string[];
}

export function lobbyGameFromRow(row: LobbyGameRow): Game {
    const players: PrivatePlayer[] = (row.players ?? []).map((p) => ({
        player_id: p.player_id,
        name: p.name,
        status: (p.status ?? 'ready') as PrivatePlayer['status'],
        is_ai: p.is_ai,
        hand: [], // pre-deal: no cards
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
    const { serializeViewBlob } = await import('./wasm/engine.ts');
    const { hexToBytes } = await import('./replay/codec.ts');
    const viewBlob = serializeViewBlob(hexToBytes(row.state), seat);
    return encodeGameResponse(row.version ?? 0, seat, roster, viewBlob);
}
