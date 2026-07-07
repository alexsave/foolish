// Masked view state ("view" v1) — TS mirror of cnitro/src/view.c. The
// kernel's per-viewer masked put_state payload is THE representation of a
// personalized game on the wire; this module is the single place JS objects
// are materialized from it (the React render boundary / test assertions),
// plus the byte-writer the TS evwire encoder uses so the legacy JS event
// paths (bot loop, meta actions) emit byte-identical streams.
// Pure TS, no wasm imports — shared by client, edge functions and e2e.
import {
    Battle, Card, Game, GAME_STATUS, PersonalGame, PLAYER_STATUS,
    PrivatePlayer, PublicGame, PublicPlayer,
} from "../types.ts";
import { cardFromWireByte, WIRE_HIDDEN, WIRE_NONE, wireCard } from "./awire.ts";

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

// A parsed masked put_state payload. Hidden cards (WIRE_HIDDEN) parse to
// null; hand/deck COUNTS are always real.
export interface ViewState {
    status: number;
    numPlayers: number;
    powerSuit: number;
    firstAttacker: number;
    defender: number;
    discard: number;
    flipped: Card | null;
    goodMask: number;
    hasGoodTs: boolean;
    deckLen: number;
    battles: Battle[];
    players: { status: number; awaiting: boolean; hand: (Card | null)[] }[];
    elimination: number[];
}

const i8 = (b: number) => (b > 127 ? b - 256 : b);

// Parse one masked put_state payload starting at `off`; returns the state
// and the offset just past it. Throws RangeError on a truncated buffer —
// wrap callers that receive untrusted/corruptible bytes (decodeEventWire and
// decodePackedGame catch and surface null).
export function parseMaskedState(buf: Uint8Array, off: number): { state: ViewState; end: number } {
    let q = off;
    const need = (n: number) => {
        if (q + n > buf.length) throw new RangeError(`view: truncated state payload at ${q}+${n}/${buf.length}`);
    };
    need(17); // the fixed-size header through deck_count
    const status = buf[q++];
    const numPlayers = buf[q++];
    const powerSuit = i8(buf[q++]);
    const firstAttacker = i8(buf[q++]);
    const defender = i8(buf[q++]);
    const discard = buf[q] | (buf[q + 1] << 8); q += 2;
    const hasFlipped = buf[q++] !== 0;
    const flippedWire = buf[q++];
    const goodMask = (buf[q] | (buf[q + 1] << 8) | (buf[q + 2] << 16) | (buf[q + 3] << 24)) >>> 0; q += 4;
    const hasGoodTs = buf[q++] !== 0;
    const deckLen = buf[q] | (buf[q + 1] << 8); q += 2;
    need(deckLen + 1);
    q += deckLen; // masked deck bytes carry no information beyond the count
    const nBattles = buf[q++];
    need(nBattles * 2 + 1);
    const battles: Battle[] = [];
    for (let i = 0; i < nBattles; i++) {
        const attack = cardFromWireByte(buf[q++]);
        const dw = buf[q++];
        battles.push({ attack, defense: dw === WIRE_NONE ? null : cardFromWireByte(dw) });
    }
    const players: ViewState['players'] = [];
    for (let i = 0; i < numPlayers; i++) {
        need(3);
        const pStatus = buf[q++];
        const awaiting = buf[q++] !== 0;
        const handN = buf[q++];
        need(handN + 1);
        const hand: (Card | null)[] = new Array(handN);
        for (let j = 0; j < handN; j++) {
            const b = buf[q++];
            hand[j] = b === WIRE_HIDDEN ? null : cardFromWireByte(b);
        }
        players.push({ status: pStatus, awaiting, hand });
    }
    const elimN = buf[q++];
    need(elimN);
    const elimination: number[] = [];
    for (let i = 0; i < elimN; i++) elimination.push(i8(buf[q++]));
    return {
        state: {
            status, numPlayers, powerSuit, firstAttacker, defender, discard,
            flipped: hasFlipped ? cardFromWireByte(flippedWire) : null,
            goodMask, hasGoodTs, deckLen, battles, players, elimination,
        },
        end: q,
    };
}

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

// Materialize the React-facing view model from a parsed masked state — the
// ONE place packed bytes become JS objects on the client. viewerSeat < 0
// yields a spectator PublicGame (no self).
export function viewToGame(
    view: ViewState, roster: ViewRoster, viewerSeat: number, ctx: ViewDecodeCtx,
): PersonalGame | PublicGame {
    const players: PublicPlayer[] = view.players.map((vp, i) => ({
        player_id: roster.players[i]?.player_id ?? `seat-${i}`,
        name: roster.players[i]?.name ?? `seat-${i}`,
        is_ai: roster.players[i]?.is_ai ?? false,
        status: P_STATUS_FROM_INT[vp.status] ?? PLAYER_STATUS.IDLE,
        hand_length: vp.hand.length,
    }));
    const base: PublicGame = {
        id: roster.id,
        name: roster.name,
        deck_length: view.deckLen,
        discard_pile_length: view.discard,
        flipped: view.flipped,
        players,
        status: G_STATUS_FROM_INT[view.status] ?? GAME_STATUS.WAITING,
        power_suit: view.powerSuit,
        first_attacker: view.firstAttacker,
        defender: view.defender,
        table_battles: view.battles,
        elimination_order: view.elimination.map(s => roster.players[s]?.player_id ?? `seat-${s}`),
        good_players: goodPlayersFromViewMask(view.goodMask, roster, ctx.preGood),
        good_timestamp: view.hasGoodTs ? (ctx.prevGoodTs ?? (ctx.now ?? Date.now)()) : null,
    };
    if (viewerSeat < 0 || viewerSeat >= view.players.length) return base;
    const vp = view.players[viewerSeat];
    const self: PrivatePlayer = {
        ...players[viewerSeat],
        // A masked byte in the viewer's own hand cannot happen on a
        // well-formed view; render a card back rather than crash if it does.
        hand: vp.hand.map(c => c ?? { suit: -1, value: -1 }),
        awaiting_attack: vp.awaiting,
        strategy_key: roster.players[viewerSeat]?.strategy_key ?? 'human',
    };
    return { ...base, self } as PersonalGame;
}

// ---------------------------------------------------------------------------
// get_game packed response envelope:
//   u8 fmt | u8 flags (bit0 = caller is a player) | u8 my_seat (0xFF
//   spectator) | u32 LE version | u16 LE roster_len | roster JSON (identity +
//   the column-authoritative fields the blob omits) | u16 LE view_len |
//   masked view blob ([VIEW_FORMAT_VERSION | viewer | masked put_state])
// ---------------------------------------------------------------------------

export const GAME_RESP_FORMAT = 1;

// The roster JSON rides the identity/presentation fields, same split as
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
    const rosterBytes = new TextEncoder().encode(JSON.stringify(roster));
    const out = new Uint8Array(3 + 4 + 2 + rosterBytes.length + 2 + viewBlob.length);
    let q = 0;
    out[q++] = GAME_RESP_FORMAT;
    out[q++] = seat >= 0 ? 1 : 0;
    out[q++] = seat >= 0 ? seat : 0xff;
    out[q++] = version & 0xff; out[q++] = (version >> 8) & 0xff;
    out[q++] = (version >> 16) & 0xff; out[q++] = (version >> 24) & 0xff;
    out[q++] = rosterBytes.length & 0xff; out[q++] = (rosterBytes.length >> 8) & 0xff;
    out.set(rosterBytes, q); q += rosterBytes.length;
    out[q++] = viewBlob.length & 0xff; out[q++] = (viewBlob.length >> 8) & 0xff;
    out.set(viewBlob, q);
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
    let state: ViewState;
    let q = 9 + rosterLen;
    try {
        roster = JSON.parse(new TextDecoder().decode(buf.subarray(9, 9 + rosterLen))) as PackedGameRoster;
        const viewLen = buf[q] | (buf[q + 1] << 8); q += 2;
        if (q + viewLen > buf.length || buf[q] !== VIEW_FORMAT_VERSION) return null;
        ({ state } = parseMaskedState(buf, q + 2)); // skip [fmt | viewer]
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
// Byte writer — the TS mirror of view.c state_put(masked). Used by the
// evwire TS encoder so JS-path broadcasts (bot loop, meta) are byte-identical
// to the kernel's own serialization. `game` is a full server Game.
// ---------------------------------------------------------------------------

export function writeMaskedState(game: Game, viewerSeat: number, out: number[]): void {
    out.push(G_STATUS_TO_INT[game.status] ?? 0);
    out.push(game.players.length & 0xff);
    out.push(game.power_suit & 0xff);
    out.push(game.first_attacker & 0xff);
    out.push(game.defender & 0xff);
    out.push(game.discard_pile_length & 0xff, (game.discard_pile_length >> 8) & 0xff);
    out.push(game.flipped ? 1 : 0);
    // Canonical no-flip byte — mirrors view.c state_put exactly.
    out.push(game.flipped ? wireCard(game.flipped) : WIRE_HIDDEN);
    let mask = 0;
    for (const pid of game.good_players ?? []) {
        const s = game.players.findIndex(p => p.player_id === pid);
        // The s >= 0 guard is load-bearing: 1 << -1 is 1 << 31 in JS and
        // would phantom-set seat 31 for a good entry whose player left.
        if (s >= 0) mask |= 1 << s;
    }
    out.push(mask & 0xff, (mask >> 8) & 0xff, (mask >> 16) & 0xff, (mask >> 24) & 0xff);
    out.push(game.good_timestamp !== null && game.good_timestamp !== undefined ? 1 : 0);
    out.push(game.deck.length & 0xff, (game.deck.length >> 8) & 0xff);
    for (let i = 0; i < game.deck.length; i++) out.push(WIRE_HIDDEN);
    out.push(game.table_battles.length & 0xff);
    for (const b of game.table_battles) {
        out.push(wireCard(b.attack));
        out.push(b.defense ? wireCard(b.defense) : WIRE_NONE);
    }
    for (let i = 0; i < game.players.length; i++) {
        const p = game.players[i];
        const visible = i === viewerSeat;
        out.push(P_STATUS_TO_INT[p.status] ?? 0);
        out.push(visible && p.awaiting_attack ? 1 : 0);
        out.push(p.hand.length & 0xff);
        for (const c of p.hand) out.push(visible ? wireCard(c) : WIRE_HIDDEN);
    }
    out.push(game.elimination_order.length & 0xff);
    for (const pid of game.elimination_order) {
        const s = game.players.findIndex(p => p.player_id === pid);
        out.push(s & 0xff);
    }
}
