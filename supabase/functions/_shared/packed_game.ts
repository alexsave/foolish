// Packed personalized-game responses (docs/PACKED_WIRE_CUTOVER.md), shared by
// the get_game and get_my_games edge functions and — because the edge
// entrypoints themselves are behind serve() — directly testable in e2e.
//
// The caller's view of a dealt game leaves the kernel as a MASKED view blob
// (wasm_view_serialize) wrapped with the identity roster JSON; a game that
// cannot be served packed (no blob, or a WAITING lobby — lobbies are
// assembled from membership rows, never from a blob) falls back to the
// personalize_game JSON, byte-wrapped for the list envelope.
import { GAME_STATUS } from './types.ts';
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
