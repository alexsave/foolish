// player_views cache writer (docs/PLAYER_VIEWS.md).
//
// The `player_views` table stores each participant's ALREADY-MASKED packed view
// of a game, written only by the server (inside commit_game's version fence),
// read only by that player under RLS. It lets the client load its dashboard
// list as a plain indexed SELECT — no edge round-trip — and get live pushes for
// free via Realtime.
//
// This module turns a committed Game into the per-player rows commit_game
// upserts. Each row's `view` is the SAME packed single-game envelope the
// get_game edge function emits (encodeGameResponse), so the client decodes it
// with the existing, shared decodePackedGame — nothing new on the read side.
//
// Masking stays in the C kernel wherever there is hidden state: a DEALT game's
// per-seat blob comes from wasm_view_serialize (engine.serializeViewBlobs, ONE
// deserialize for all seats). A lobby (WAITING) game has no kernel blob and no
// hidden information (empty hands, empty deck), so its view is written by the
// pure-TS mirror of the same kernel format (writeMaskedState) — the identical
// path get_game uses for blob-less rows.
import { GAME_STATUS, Game } from '@api/core/types.ts';
import {
    VIEW_FORMAT_VERSION, encodeGameResponse, writeMaskedState, PackedGameRoster,
} from '@sdk/ts/wire/view.ts';
import { bytesToBareHex } from '@sdk/ts/wire/bytes.ts';

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
        const { serializeViewBlobs } = await import('@sdk/ts/wasm/engine.ts');
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

// The SHARED spectator view (seat -1) for a game — the fully-masked view (every
// hand a card-back, deck order hidden) that any authenticated user may read
// (spectator_views table, looser RLS), replacing get_game's spectate path. Same
// packed envelope decodePackedGame yields a PublicGame (no self) from. Masking
// stays in the C kernel for a dealt game: wasm_view_serialize(-1) (VIEW_SPECTATOR)
// via serializeViewBlobs([-1]); a lobby has no kernel state, so the pure-TS
// view.c mirror writeMaskedState(game, -1) produces the identical bytes.
export async function buildSpectatorView(
    game: Game, stateHex: string | null, version: number,
): Promise<string> {
    const roster = rosterOf(game);
    const dealt = stateHex !== null && game.status !== GAME_STATUS.WAITING;

    let viewBlob: Uint8Array;
    if (dealt) {
        const { serializeViewBlobs } = await import('@sdk/ts/wasm/engine.ts');
        const { hexToBytes } = await import('./replay/codec.ts');
        viewBlob = serializeViewBlobs(hexToBytes(stateHex!), [-1]).get(-1)!;
    } else {
        const body: number[] = [];
        writeMaskedState(game, -1, body); // -1 => no seat visible (all card-backs)
        viewBlob = Uint8Array.from([VIEW_FORMAT_VERSION, 0xff, ...body]);
    }
    return bytesToBareHex(encodeGameResponse(version, -1, roster, viewBlob));
}

// Full upsert rows (with game_id + version) for a CACHE-WARM / BACKFILL path:
// the same per-participant rows commit_game writes, but shaped for a fill-if-
// absent write from OUTSIDE a commit (whoever reads a game first can backfill the
// whole game for everyone, so a game predating the cache becomes a direct SELECT
// on the next open). No runtime caller today — get_game (its only user) was
// removed once all live games had views — but the e2e suite exercises it to pin
// the byte-identical-rebuild + fill-if-absent invariants a future backfill relies
// on.
//
// Each row is byte-identical to what commit_game wrote for that (game, player)
// at this version — same builder (buildPlayerViewRows) — so the warm write is a
// pure fill-in, never a different value. Writers MUST insert these fill-if-absent
// (ON CONFLICT DO NOTHING): commit_game owns UPDATES under the version fence, and
// a read-path write is NOT fenced, so it must never overwrite an existing
// (possibly newer) row. Empty for an all-bot game (no human rows to warm).
export interface PlayerViewUpsert {
    game_id: string;
    player_id: string;
    view: string;
    version: number;
    status: string;
}

export async function buildPlayerViewUpserts(
    game: Game, stateHex: string | null, version: number,
): Promise<PlayerViewUpsert[]> {
    const rows = await buildPlayerViewRows(game, stateHex, version);
    return rows.map(r => ({ game_id: game.id, player_id: r.player_id, view: r.view, version, status: r.status }));
}
