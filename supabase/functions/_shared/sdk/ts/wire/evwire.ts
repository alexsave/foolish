// Event wire ("evwire" v1) — TS mirror of cnitro/src/evwire.h.
//
// decodeEventWire: packed bytes -> the AnimationSequenceMessage the client
// animation pipeline already consumes (events with per-step game_state
// snapshots + the final game). This is the client's render-boundary
// materialization; nothing upstream of it handles JS game objects.
//
// encodeEventWire: JS AnimationEvent[] -> the SAME bytes the kernel's
// wasm_events_serialize produces. The paths still running on JS Games (bot
// loop, meta/lobby actions) encode at the broadcast edge so the client sees
// exactly one format; e2e proves C and TS emissions byte-identical.
// Pure TS, no wasm imports.
import { AnimationEvent, ANIMATION_EVENT_TYPE, Card, Game, PersonalGame, PublicGame } from "../../../core/types.ts";
import { SUIT_MAP, VALUE_MAP } from "../../../core/constants.ts";
import { WIRE_HIDDEN, wireCard } from "./awire.ts";
import { ViewDecodeCtx, ViewRoster, viewToGame, writeMaskedState } from "./view.ts";
import { KernelState, kernelEventsFromPacked } from "../wasm/bots.ts";

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

const cardDisplay = (c: Card) => `${VALUE_MAP[c.value]} of ${SUIT_MAP[c.suit]}`;
const cardList = (cards: Card[]) => cards.map(cardDisplay).join(', ');

// The client never rendered server messages, but the reconstruction keeps
// byte/behavior parity with the retired JSON path testable end-to-end.
function reconstructMessage(
    msg: number, seatName: string, cards: Card[], target: Card | null,
    view: { defender: number; firstAttacker: number; players: { status: number }[] },
    roster: ViewRoster,
): string | undefined {
    switch (msg) {
        case EVW_MSG.ATTACKED: return `${seatName} attacked with ${cardList(cards)}`;
        case EVW_MSG.PASSED: return `${seatName} passed with ${cardList(cards)}`;
        case EVW_MSG.OUT: return `${seatName} is out`;
        case EVW_MSG.COVERED: return `${seatName} covered ${cardDisplay(target!)} with ${cardDisplay(cards[0])}`;
        case EVW_MSG.DISCARDED: return `${cards.length} cards discarded`;
        case EVW_MSG.DREW: return `${seatName} drew ${cards.length} cards`;
        case EVW_MSG.DEFENDER_MOVE: return `${seatName} is now the defender`;
        case EVW_MSG.PICKUP: return `${seatName} picked up ${cards.length} cards`;
        case EVW_MSG.GOOD_TRANSITION: {
            // transitionReason: attackers still IN at this snapshot, minus
            // the defender (player-status int 2 = IN).
            let attackers = 0;
            for (let i = 0; i < view.players.length; i++) {
                if (i !== view.defender && view.players[i].status === 2) attackers++;
            }
            return `All ${attackers} attackers said good and all attacks covered - proceeding to next round`;
        }
        case EVW_MSG.START_MAGIC: return `All players ready - starting game!`;
        case EVW_MSG.FIRST_ATTACKER: {
            const name = roster.players[view.firstAttacker]?.name ?? `seat-${view.firstAttacker}`;
            return `Player ${name} is the first attacker, wait for them to attack`;
        }
        default: return undefined;
    }
}

export interface DecodedEvent {
    type: string;
    player_id?: string;
    cards?: Card[];
    from_location?: string;
    to_location?: string;
    target_card?: Card;
    battle_index?: number;
    message?: string;
    game_state: PersonalGame | PublicGame;
}

export interface DecodedSequence {
    viewerSeat: number;   // -1 spectator
    actorSeat: number;    // -1 none
    events: DecodedEvent[];
    game: PersonalGame | PublicGame; // the committed final state (trailer)
}

// Packed sequence -> client-shape events. Returns null on anything the kernel
// cannot read — an unknown format version, a truncated payload — because a
// caller must treat that as unreadable, never as empty.
//
// The bytes are read by the KERNEL (kernelEventsFromPacked -> evwire.c's own
// reader). What is left here is the join the kernel cannot do: seat -> player_id
// and name, the message prose, and the location/type enums the app speaks. That
// is why this function still exists and why it is now this short.
export function decodeEventWire(buf: Uint8Array, roster: ViewRoster, ctx: ViewDecodeCtx): DecodedSequence | null {
    let seq;
    try {
        seq = kernelEventsFromPacked(buf);
    } catch {
        return null;
    }

    const viewerSeat = seq.viewer;
    const actorSeat = seq.actor;
    const events: DecodedEvent[] = seq.events.map((e) => {
        const type = EVENT_TYPE_FROM_INT[e.type];
        // A masked card (the DEAL/REFILL redaction) arrives as null and renders
        // as a back; the kernel never sent the identity, so there is none to lose.
        const cards: Card[] = e.cards.map((c) => (c ? { suit: c.s, value: c.v } : { suit: -1, value: -1 }));
        const target: Card | null = e.target ? { suit: e.target.s, value: e.target.v } : null;

        const ev: DecodedEvent = {
            type,
            game_state: viewToGame(e.state, roster, viewerSeat, ctx),
        };
        if (e.seat >= 0) ev.player_id = roster.players[e.seat]?.player_id ?? `seat-${e.seat}`;
        if (cards.length > 0 || type === ANIMATION_EVENT_TYPE.ATTACK_PASS
            || type === ANIMATION_EVENT_TYPE.DISCARD || type === ANIMATION_EVENT_TYPE.PICKUP
            || type === ANIMATION_EVENT_TYPE.DEAL || type === ANIMATION_EVENT_TYPE.CARDS_TO_TRASH
            || type === ANIMATION_EVENT_TYPE.REFILL || type === ANIMATION_EVENT_TYPE.FLIPPED
            || type === ANIMATION_EVENT_TYPE.COVER) ev.cards = cards;
        if (e.from !== LOC_NONE) ev.from_location = LOC_FROM_INT[e.from];
        if (e.to !== LOC_NONE) ev.to_location = LOC_FROM_INT[e.to];
        if (target) ev.target_card = target;
        if (e.battle !== undefined) ev.battle_index = e.battle;
        const seatName = e.seat >= 0 ? (roster.players[e.seat]?.name ?? `seat-${e.seat}`) : '';
        const message = reconstructMessage(e.msg, seatName, cards, target, e.state, roster);
        if (message !== undefined) ev.message = message;
        return ev;
    });

    return {
        viewerSeat, actorSeat, events,
        game: viewToGame(seq.game, roster, viewerSeat, ctx),
    };
}

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
        // Per-step snapshot, masked for this viewer.
        const snap: number[] = [];
        writeMaskedState(ev.game_state, viewerSeat, snap);
        out.push(snap.length & 0xff, (snap.length >> 8) & 0xff);
        out.push(...snap);
    }
    const fin: number[] = [];
    writeMaskedState(finalGame, viewerSeat, fin);
    out.push(fin.length & 0xff, (fin.length >> 8) & 0xff);
    out.push(...fin);
    return new Uint8Array(out);
}
