// The response envelope ("view" v1) as TypeScript still writes it: the encoder a
// server used before the C Table wrote every envelope (table.h table_envelope),
// kept for the tests that hold the shipped format (Phase 8 deletes it). The
// READER is gone: a client reads an envelope through the kernel's client slot
// (c/src/client_table.h, sdk/ts/table/client_table.ts), docs/C_GAME_SHAPE_MIGRATION.md
// Phase 5a.
import { encodePackedRoster } from "./roster.ts";

export const VIEW_FORMAT_VERSION = 1;

// Identity/presentation fields the packed formats deliberately omit — the
// same split as engine.ts's RosterTemplate.
export interface ViewRoster {
    id: string;
    name: string;
    players: { player_id: string; name: string; is_ai: boolean; strategy_key?: string }[];
}

// ---------------------------------------------------------------------------
// get_game packed response envelope:
//   u8 fmt | u8 flags (bit0 = caller is a player, bit1 = a PACKED roster
//   trailer follows the view blob) | u8 my_seat (0xFF spectator) | u32 LE
//   version | u16 LE roster_len (always 0) | u16 LE view_len | masked view
//   blob ([VIEW_FORMAT_VERSION | viewer | masked put_state])
//   | packed roster trailer (encodePackedRoster), required
//
// WHY THE ROSTER IS A TRAILER AND NOT A FIELD. It used to be an island of JSON
// sitting at roster_len, and it could not simply be replaced in place: merging
// here deploys the server IMMEDIATELY while the iOS client ships through the
// App Store, and the same envelope is STORED in
// player_views.view / spectator_views.view - a column rather than a request, so
// there was no caller to negotiate a format with. What every shipped reader had
// in common is that it reads a PREFIX: it takes flags bit0 and ignores the rest
// of that byte, and it bounds the view blob with `q + viewLen <= length` and
// ignores whatever follows. So the packed roster went in AFTER the view blob,
// announced in a flag bit older decoders discard, and both forms shipped at once
// while the field caught up.
//
// The island is now gone: roster_len is written as 0 and the trailer is
// mandatory. A payload without the flag no longer decodes at all (the client
// slot refuses it). The u16 at bytes 7-8 stays so every offset after it is
// unchanged, and e2e/packed_roster_wire.test.ts still pins the prefix with a
// frozen replica of the 1.0(43) decoder.
// ---------------------------------------------------------------------------

export const GAME_RESP_FORMAT = 1;

/** flags bit0 - the caller occupies a seat in this game. */
export const GAME_RESP_FLAG_PLAYER = 0x01;
/** flags bit1 - a packed roster trailer follows the view blob. */
export const GAME_RESP_FLAG_PACKED_ROSTER = 0x02;

// The roster carries the identity/presentation fields, same split as
// engine.ts's RosterTemplate: ids/names/is_ai, good order + timestamp value,
// and the column-authoritative game status.
export interface PackedGameRoster extends ViewRoster {
    status: string;
    good_players: string[];
    good_timestamp: number | null;
}

// retired by Phase 4b; deleted in Phase 8 (the server's envelopes are table_envelope / table_commit_products)
export function encodeGameResponse(
    version: number, seat: number, roster: PackedGameRoster, viewBlob: Uint8Array,
): Uint8Array {
    const trailer = encodePackedRoster(roster);
    // The island the roster used to ride in is a zero-length segment now - the
    // roster IS the packed trailer. Bytes 7-8 stay a u16 length so the
    // envelope's shape is unchanged and every offset below still lines up.
    const rosterBytes = new Uint8Array(0);
    // u16 — enforce, never wrap (a silent & 0xff truncation would desync the
    // whole envelope). Real payloads are ~½KB.
    if (viewBlob.length > 0xffff) throw new Error(`view: view blob ${viewBlob.length}B exceeds the u16 cap`);
    const out = new Uint8Array(3 + 4 + 2 + rosterBytes.length + 2 + viewBlob.length + trailer.length);
    let q = 0;
    out[q++] = GAME_RESP_FORMAT;
    out[q++] = (seat >= 0 ? GAME_RESP_FLAG_PLAYER : 0) | GAME_RESP_FLAG_PACKED_ROSTER;
    out[q++] = seat >= 0 ? seat : 0xff;
    out[q++] = version & 0xff; out[q++] = (version >> 8) & 0xff;
    out[q++] = (version >> 16) & 0xff; out[q++] = (version >> 24) & 0xff;
    out[q++] = rosterBytes.length & 0xff; out[q++] = (rosterBytes.length >> 8) & 0xff;
    out.set(rosterBytes, q); q += rosterBytes.length;
    out[q++] = viewBlob.length & 0xff; out[q++] = (viewBlob.length >> 8) & 0xff;
    out.set(viewBlob, q); q += viewBlob.length;
    // Everything above this line is byte for byte what pre-trailer builds wrote;
    // everything below is invisible to a reader that stops at the view blob,
    // which every shipped reader does.
    out.set(trailer, q);
    return out;
}

// ---------------------------------------------------------------------------
// The masked put_state WRITER used to live here, as a pure-TS mirror of
// view.c, so the lobby and meta paths could emit a view without loading a
// kernel. It is gone. The kernel writes its own format (wasm_view_serialize,
// via bots.ts wasmViewFromGame), and a second writer of a byte layout kept
// honest only by a parity test is exactly the thing that silently desyncs.
//
// The reason it existed had already expired: the browser runs the WHOLE kernel
// and fetches bots.wasm.gz as an asset (wasm_asset.ts, "one big module
// everywhere"), so there was no kernel-free client left to protect.
// ---------------------------------------------------------------------------
