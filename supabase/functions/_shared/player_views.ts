// player_views cache writer (docs/PLAYER_VIEWS.md).
//
// The `player_views` table stores each participant's ALREADY-MASKED packed view
// of a game, written only by the server (inside commit_game's version fence),
// read only by that player under RLS. It lets the client load its dashboard
// list as a plain indexed SELECT — no get_my_games edge round-trip — and get
// live pushes for free via Realtime.
//
// This module turns a committed Game into the per-player rows commit_game
// upserts. Each row's `view` is the SAME packed single-game envelope the
// get_game / get_my_games edge functions emit (encodeGameResponse), so the
// client decodes it with the existing, shared decodePackedGame — nothing new on
// the read side.
//
// Masking stays in the C kernel wherever there is hidden state: a DEALT game's
// per-seat blob comes from wasm_view_serialize (engine.serializeViewBlobs, ONE
// deserialize for all seats). A lobby (WAITING) game has no kernel blob and no
// hidden information (empty hands, empty deck), so its view is written by the
// pure-TS mirror of the same kernel format (writeMaskedState) — the identical
// fallback get_my_games already uses for blob-less rows.
import { GAME_STATUS, Game } from './types.ts';
import {
    VIEW_FORMAT_VERSION, encodeGameResponse, writeMaskedState, PackedGameRoster,
} from './wire/view.ts';
import { bytesToBareHex } from './wire/bytes.ts';

// One upsert-ready row per HUMAN participant. `view` is bare hex (no \x prefix,
// like games.logs_packed) of the packed single-game envelope; `status` is
// denormalized for cheap list filtering; `version` mirrors the committed
// games.version (the client's reorder-drop token).
export interface PlayerViewRow {
    player_id: string;
    view: string;
    status: string;
}

// Build the identity/presentation roster the packed envelope carries alongside
// the masked view — same split as engine.ts's RosterTemplate and
// buildPackedGameBytes.
function rosterOf(game: Game): PackedGameRoster {
    return {
        id: game.id,
        name: game.name,
        status: game.status, // column-authoritative over the blob's copy
        players: game.players.map(p => ({ player_id: p.player_id, name: p.name, is_ai: p.is_ai })),
        good_players: game.good_players ?? [],
        good_timestamp: game.good_timestamp ?? null,
    };
}

// The per-player view rows for a committed game. `stateHex` is the packed kernel
// blob this commit persisted (games.state) or null for a never-dealt lobby;
// `version` is the committed games.version the rows should carry. Bots are
// skipped (they have no client to read a row). Returns [] when there are no
// human participants (an all-bot game) — commit_game reads that as "prune every
// view row for this game".
export async function buildPlayerViewRows(
    game: Game, stateHex: string | null, version: number,
): Promise<PlayerViewRow[]> {
    const roster = rosterOf(game);
    const humanSeats: number[] = [];
    for (let seat = 0; seat < game.players.length; seat++) {
        if (!game.players[seat].is_ai) humanSeats.push(seat);
    }
    if (humanSeats.length === 0) return [];

    // A dealt game (blob present) is masked in the C kernel; a lobby is masked
    // by the TS mirror. The status guard matches loadCompleteGame /
    // buildPackedGameBytes: a WAITING game must never be read through a blob
    // (a stale one would leak a finished session's hands).
    const dealt = stateHex !== null && game.status !== GAME_STATUS.WAITING;

    let viewBlobFor: (seat: number) => Uint8Array;
    if (dealt) {
        // Lazy imports so lobby/create commits never pull the rules-wasm embed
        // (same discipline as commitGame's serializeGameState import).
        const { serializeViewBlobs } = await import('./wasm/engine.ts');
        const { hexToBytes } = await import('./replay/codec.ts');
        const blobs = serializeViewBlobs(hexToBytes(stateHex!), humanSeats);
        viewBlobFor = (seat) => blobs.get(seat)!;
    } else {
        viewBlobFor = (seat) => {
            const body: number[] = [];
            writeMaskedState(game, seat, body);
            // Wrap with the [VIEW_FORMAT_VERSION | viewer | masked put_state]
            // header the kernel's wasm_view_serialize also emits, so both paths
            // produce a byte-identical envelope decodePackedGame can read.
            return Uint8Array.from([VIEW_FORMAT_VERSION, seat & 0xff, ...body]);
        };
    }

    const rows: PlayerViewRow[] = [];
    for (const seat of humanSeats) {
        const envelope = encodeGameResponse(version, seat, roster, viewBlobFor(seat));
        rows.push({
            player_id: game.players[seat].player_id,
            view: bytesToBareHex(envelope),
            status: game.status,
        });
    }
    return rows;
}
