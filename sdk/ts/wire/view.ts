// Masked view state ("view" v1) — TS mirror of c/src/view.c. The
// kernel's per-viewer masked put_state payload is THE representation of a
// personalized game on the wire; this module is the single place JS objects
// are materialized from it (the React render boundary / test assertions),
// plus the byte-writer the TS evwire encoder uses so the legacy JS event
// paths (bot loop, meta actions) emit byte-identical streams.
// Pure TS, no wasm imports — shared by client, edge functions and e2e.
import {
    Card, Game, GAME_STATUS, PersonalGame, PLAYER_STATUS,
    PrivatePlayer, PublicGame, PublicPlayer,
} from "@api/core/types.ts";
import { WIRE_HIDDEN, WIRE_NONE, wireCard } from "./awire.ts";
import { KernelCard, KernelState, kernelViewFromPacked } from "@sdk/ts/wasm/bots.ts";
import { decodePackedRoster, encodePackedRoster } from "./roster.ts";

export const VIEW_FORMAT_VERSION = 1;

const G_STATUS_FROM_INT = [GAME_STATUS.WAITING, GAME_STATUS.PLAYING, GAME_STATUS.GAME_OVER] as const;
const G_STATUS_TO_INT: Record<string, number> = {
    [GAME_STATUS.WAITING]: 0, [GAME_STATUS.PLAYING]: 1, [GAME_STATUS.GAME_OVER]: 2,
};
const P_STATUS_FROM_INT = [PLAYER_STATUS.IDLE, PLAYER_STATUS.READY, PLAYER_STATUS.IN, PLAYER_STATUS.OUT] as const;
const P_STATUS_TO_INT: Record<string, number> = {
    [PLAYER_STATUS.IDLE]: 0, [PLAYER_STATUS.READY]: 1, [PLAYER_STATUS.IN]: 2, [PLAYER_STATUS.OUT]: 3,
};

// Identity/presentation fields the packed formats deliberately omit — the
// same split as engine.ts's RosterTemplate.
export interface ViewRoster {
    id: string;
    name: string;
    players: { player_id: string; name: string; is_ai: boolean; strategy_key?: string }[];
}

// This file used to carry a parseMaskedState that read view.c's layout byte for
// byte. That reader is now wire/packed_read.ts (reached through
// kernelViewFromPacked) - one place, shared with the event stream, rather than
// inlined here. What is left in this file is the part the kernel structurally
// cannot do: joining the board to the roster.

// Reconstruct the good_players array (insertion-ordered) from the mask.
// The pre-known order survives; at most ONE player can be newly added per
// action (the actor), so appending any masked-but-unknown pid in seat order
// reproduces goodPlayersFromMask exactly without needing the actor id.
export function goodPlayersFromViewMask(mask: number, roster: ViewRoster, preGood: string[]): string[] {
    if (mask === 0) return [];
    const seatOf = new Map(roster.players.map((p, i) => [p.player_id, i]));
    const out = preGood.filter(pid => {
        const s = seatOf.get(pid);
        return s !== undefined && (mask & (1 << s)) !== 0;
    });
    for (let s = 0; s < roster.players.length; s++) {
        if ((mask & (1 << s)) !== 0) {
            const pid = roster.players[s].player_id;
            if (!out.includes(pid)) out.push(pid);
        }
    }
    return out;
}

export interface ViewDecodeCtx {
    preGood: string[];              // the last known good_players order
    prevGoodTs: number | null;      // the last known good_timestamp value
    now?: () => number;             // injectable clock for tests
}

// The kernel speaks {s,v}; the app speaks {suit,value}.
const card = (c: KernelCard): Card => ({ suit: c.s, value: c.v });

// Materialize the React-facing view model from a board the KERNEL decoded — the
// one place a kernel board becomes a JS game object. viewerSeat < 0 yields a
// spectator PublicGame (no self).
//
// Everything this adds is something the kernel does not have and should not:
// identity (game.h keeps it out of the blob deliberately), the good-players
// INSERTION order (needs the caller's prior order), and the good_timestamp VALUE
// (a host clock reading). The kernel says what the board is; this says who the
// seats are.
export function viewToGame(
    view: KernelState, roster: ViewRoster, viewerSeat: number, ctx: ViewDecodeCtx,
): PersonalGame | PublicGame {
    const players: PublicPlayer[] = view.players.map((vp, i) => ({
        player_id: roster.players[i]?.player_id ?? `seat-${i}`,
        name: roster.players[i]?.name ?? `seat-${i}`,
        is_ai: roster.players[i]?.is_ai ?? false,
        status: P_STATUS_FROM_INT[vp.status] ?? PLAYER_STATUS.IDLE,
        hand_length: vp.handCount,
    }));
    const base: PublicGame = {
        id: roster.id,
        name: roster.name,
        deck_length: view.deckCount,
        discard_pile_length: view.discardCount,
        flipped: view.flipped ? card(view.flipped) : null,
        players,
        status: G_STATUS_FROM_INT[view.status] ?? GAME_STATUS.WAITING,
        power_suit: view.powerSuit,
        first_attacker: view.firstAttacker,
        defender: view.defender,
        table_battles: view.battles.map(b => ({
            attack: card(b.attack),
            defense: b.defense ? card(b.defense) : null,
        })),
        elimination_order: view.eliminationOrder.map(s => roster.players[s]?.player_id ?? `seat-${s}`),
        good_players: goodPlayersFromViewMask(view.goodMask, roster, ctx.preGood),
        good_timestamp: view.hasGoodTs ? (ctx.prevGoodTs ?? (ctx.now ?? Date.now)()) : null,
    };
    if (viewerSeat < 0 || viewerSeat >= view.players.length) return base;
    const vp = view.players[viewerSeat];
    const self: PrivatePlayer = {
        ...players[viewerSeat],
        // The kernel emits "hand":null for any seat that is not the viewer. On a
        // well-formed view that cannot be the viewer's own seat; render card
        // backs rather than crash if it ever is.
        hand: vp.hand ? vp.hand.map(card) : new Array(vp.handCount).fill({ suit: -1, value: -1 }),
        awaiting_attack: vp.awaitingAttack,
        strategy_key: roster.players[viewerSeat]?.strategy_key ?? 'human',
    };
    return { ...base, self } as PersonalGame;
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
// mandatory. A payload without the flag no longer decodes at all - see
// decodePackedGame. The u16 at bytes 7-8 stays so every offset after it is
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

// Decode + materialize in one step — the client's render-boundary JS
// conversion for an authoritative fetch. Returns null on an unknown format.
export function decodePackedGame(
    buf: Uint8Array, now?: () => number,
): { game: PersonalGame | PublicGame; version: number; seat: number } | null {
    if (buf.length < 11 || buf[0] !== GAME_RESP_FORMAT) return null;
    const isPlayer = (buf[1] & 1) !== 0;
    const seat = isPlayer ? buf[2] : -1;
    const version = (buf[3] | (buf[4] << 8) | (buf[5] << 16) | (buf[6] << 24)) >>> 0;
    const rosterLen = buf[7] | (buf[8] << 8);
    if (9 + rosterLen + 2 > buf.length) return null;
    let roster: PackedGameRoster;
    let state: KernelState;
    let q = 9 + rosterLen;
    try {
        const viewLen = buf[q] | (buf[q + 1] << 8);
        // The roster is the packed trailer, and ONLY the packed trailer. A
        // payload without the flag was written before the trailer existed - an
        // idle game's cached player_views row, or an older server - and it is
        // UNREADABLE now rather than JSON-parsed, because the island it used to
        // carry is gone. Callers already treat null as unreadable, and the next
        // commit on that game rewrites its row.
        if ((buf[1] & GAME_RESP_FLAG_PACKED_ROSTER) === 0) return null;
        const packed = decodePackedRoster(buf, q + 2 + viewLen);
        if (!packed) return null;
        roster = packed.roster as PackedGameRoster;
        q += 2;
        if (q + viewLen > buf.length || buf[q] !== VIEW_FORMAT_VERSION) return null;
        // The envelope around the blob is this file's own invention (it is
        // written by encodeGameResponse a few lines up, in TypeScript, with no C
        // twin), so reading it here duplicates nothing. The BLOB inside it is
        // view.c's. Skip [fmt | viewer].
        state = kernelViewFromPacked(buf.subarray(q + 2, q + viewLen), seat);
    } catch {
        return null; // truncated/corrupt payload — caller treats as unreadable
    }
    const game = viewToGame(state, roster, seat, {
        preGood: roster.good_players ?? [],
        prevGoodTs: roster.good_timestamp ?? null,
        now,
    });
    // games.status is column-authoritative over the blob's copy — same rule
    // as loadCompleteGame.
    game.status = roster.status as PublicGame['status'];
    game.version = version;
    return { game, version, seat };
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
