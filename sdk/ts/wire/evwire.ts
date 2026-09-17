// Event wire ("evwire" v1) — TS mirror of c/src/evwire.h, the encoder half.
// The reader is the kernel's (c/src/client_table.h, through
// sdk/ts/table/client_table.ts, docs/C_GAME_SHAPE_MIGRATION.md Phase 5a).
//
// encodeEventWire: JS AnimationEvent[] -> the SAME bytes the kernel's
// wasm_events_serialize produces. The paths still running on JS Games (bot
// loop, meta/lobby actions) encode at the broadcast edge so the client sees
// exactly one format; e2e proves C and TS emissions byte-identical.
// Pure TS, no wasm imports.
import { AnimationEvent, ANIMATION_EVENT_TYPE, Game } from "@api/core/types.ts";
import { WIRE_HIDDEN, wireCard } from "./awire.ts";
import { wasmViewFromGame } from "@sdk/ts/wasm/bots.ts";

export const EVWIRE_FORMAT_VERSION = 1;
export const EVW_SEAT_NONE = 0xff;

// Index order is the wire encoding — mirrors EVW_T_* in evwire.h.
const EVENT_TYPE_FROM_INT = [
    ANIMATION_EVENT_TYPE.MAGIC_TRANSITION, ANIMATION_EVENT_TYPE.DEAL,
    ANIMATION_EVENT_TYPE.FLIPPED, ANIMATION_EVENT_TYPE.DEFENDER_MOVE,
    ANIMATION_EVENT_TYPE.ATTACK_PASS, ANIMATION_EVENT_TYPE.COVER,
    ANIMATION_EVENT_TYPE.PICKUP, ANIMATION_EVENT_TYPE.DISCARD,
    ANIMATION_EVENT_TYPE.OUT, ANIMATION_EVENT_TYPE.REFILL,
    ANIMATION_EVENT_TYPE.CARDS_TO_TRASH,
] as const;
const EVENT_TYPE_TO_INT = new Map<string, number>(EVENT_TYPE_FROM_INT.map((t, i) => [t, i]));

// Mirrors EVW_LOC_*.
const LOC_FROM_INT = ['deck', 'hand', 'table', 'discard', 'flipped'] as const;
const LOC_TO_INT = new Map<string, number>(LOC_FROM_INT.map((l, i) => [l, i]));
const LOC_NONE = 0xff;

// Mirrors EVW_MSG_*.
export const EVW_MSG = {
    NONE: 0, ATTACKED: 1, PASSED: 2, OUT: 3, COVERED: 4, DISCARDED: 5,
    DREW: 6, DEFENDER_MOVE: 7, PICKUP: 8, GOOD_TRANSITION: 9,
    START_MAGIC: 10, FIRST_ATTACKER: 11,
} as const;

// ---------------------------------------------------------------------------
// TS encoder — byte-for-byte what wasm_events_serialize emits, driven from
// the legacy JS AnimationEvent stream (each event carries a FULL server-side
// Game snapshot). Masking (personalization) applied here, exactly as
// convertToPersonal/PublicAnimationEvents did.
// ---------------------------------------------------------------------------

// Recover the message-template code from a built event; the JS strings are
// produced by exactly one template each (buildEvents / engine.ts).
function msgCodeOf(ev: AnimationEvent): number {
    const t = ev.type, m = ev.message;
    if (t === ANIMATION_EVENT_TYPE.ATTACK_PASS) {
        return m && m.includes(' passed with ') ? EVW_MSG.PASSED : EVW_MSG.ATTACKED;
    }
    if (t === ANIMATION_EVENT_TYPE.OUT) return EVW_MSG.OUT;
    if (t === ANIMATION_EVENT_TYPE.COVER) return EVW_MSG.COVERED;
    if (t === ANIMATION_EVENT_TYPE.DISCARD || t === ANIMATION_EVENT_TYPE.CARDS_TO_TRASH) return EVW_MSG.DISCARDED;
    if (t === ANIMATION_EVENT_TYPE.REFILL) return EVW_MSG.DREW;
    if (t === ANIMATION_EVENT_TYPE.PICKUP) return EVW_MSG.PICKUP;
    if (t === ANIMATION_EVENT_TYPE.DEFENDER_MOVE) return m ? EVW_MSG.DEFENDER_MOVE : EVW_MSG.NONE;
    if (t === ANIMATION_EVENT_TYPE.MAGIC_TRANSITION) {
        if (!m) return EVW_MSG.NONE;
        if (m === 'All players ready - starting game!') return EVW_MSG.START_MAGIC;
        if (m.startsWith('Player ')) return EVW_MSG.FIRST_ATTACKER;
        return EVW_MSG.GOOD_TRANSITION;
    }
    return EVW_MSG.NONE; // DEAL, FLIPPED
}

const sanitizedType = (t: string) =>
    t === ANIMATION_EVENT_TYPE.REFILL || t === ANIMATION_EVENT_TYPE.DEAL;

// `viewerSeat` -1 = spectator; `actorSeat` -1 = unknown/none (the decoder
// does not depend on it — see goodPlayersFromViewMask).
// retired by Phase 4b; deleted in Phase 8 (the server's pushes are table_push)
export function encodeEventWire(
    events: AnimationEvent[], finalGame: Game, viewerSeat: number, actorSeat: number,
): Uint8Array {
    // Mirror the C side's hard cap (evwire_serialize returns -1 past 255);
    // silent & 0xff truncation would desync the stream.
    if (events.length > 255) throw new Error(`evwire: ${events.length} events exceeds the wire cap of 255`);
    const out: number[] = [
        EVWIRE_FORMAT_VERSION,
        viewerSeat < 0 ? EVW_SEAT_NONE : viewerSeat,
        actorSeat < 0 ? EVW_SEAT_NONE : actorSeat,
        events.length & 0xff,
    ];
    const seatOf = new Map(finalGame.players.map((p, i) => [p.player_id, i]));
    const viewerPid = viewerSeat >= 0 ? finalGame.players[viewerSeat]?.player_id : undefined;
    for (const ev of events) {
        const typeInt = EVENT_TYPE_TO_INT.get(ev.type);
        if (typeInt === undefined) throw new Error(`evwire: unknown event type ${ev.type}`);
        out.push(typeInt);
        const seat = ev.player_id !== undefined ? seatOf.get(ev.player_id) : undefined;
        out.push(seat === undefined ? EVW_SEAT_NONE : seat);
        out.push(msgCodeOf(ev));
        out.push(ev.from_location ? (LOC_TO_INT.get(ev.from_location) ?? LOC_NONE) : LOC_NONE);
        out.push(ev.to_location ? (LOC_TO_INT.get(ev.to_location) ?? LOC_NONE) : LOC_NONE);
        const hasTarget = ev.target_card !== undefined;
        const hasBattle = ev.battle_index !== undefined;
        out.push((hasTarget ? 1 : 0) | (hasBattle ? 2 : 0));
        const cards = ev.cards ?? [];
        // The shouldSanitizeCards rule: DEAL/REFILL identities only for the
        // receiving seat; spectators see backs.
        const mask = sanitizedType(ev.type)
            && (viewerSeat < 0 || ev.player_id !== viewerPid);
        out.push(cards.length & 0xff);
        for (const c of cards) out.push(mask ? WIRE_HIDDEN : wireCard(c));
        if (hasTarget) out.push(wireCard(ev.target_card!));
        if (hasBattle) out.push(ev.battle_index! & 0xff);
        // Per-step snapshot, masked for this viewer. The 2-byte
        // [VIEW_FORMAT_VERSION | viewer] header the blob carries is the
        // envelope's, not the snapshot's, so it is trimmed here.
        const snap = wasmViewFromGame(ev.game_state as Game, viewerSeat).subarray(2);
        out.push(snap.length & 0xff, (snap.length >> 8) & 0xff);
        for (const b of snap) out.push(b);
    }
    const fin = wasmViewFromGame(finalGame, viewerSeat).subarray(2);
    out.push(fin.length & 0xff, (fin.length >> 8) & 0xff);
    for (const b of fin) out.push(b);
    return new Uint8Array(out);
}
