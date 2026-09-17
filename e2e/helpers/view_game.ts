// view_game.ts - TEST ONLY: a TableView snapshot as the PersonalGame shape, and back.
//
// The web renders the kernel's TableView snapshots (src/state/view.ts) since
// docs/C_GAME_SHAPE_MIGRATION.md Phase 6a. Tests written against the old
// PersonalGame shape - most of them server tests asserting on what a player is
// served - still read a board field by field that way, so the mapping the web
// used to carry (src/state/snapshotToGame.ts) lives here for them, exactly as it
// was: the retired TS decoder's shape, with good_players in seat order and no
// event message prose (Q1, Q7). `gameToView` is its inverse, for the tests that
// hand a PersonalGame literal to code that now takes a board.
//
// It knows names, not bytes: every value comes out of, or goes into, a generated
// snapshot shape. Retired with the TS game shape in Phase 8.

import {
    ANIMATION_EVENT_TYPE, Card, GAME_STATUS, PersonalGame, PLAYER_STATUS, PrivatePlayer, PublicGame, PublicPlayer,
} from '../../server/api/core/types.ts';
import * as V from '../../sdk/ts/gen/view_layout.bots.ts';
import { clientTable, PushEvent, PushRead, TableView, ViewCard } from '../../sdk/ts/table/client_table.ts';

const G_STATUS = [GAME_STATUS.WAITING, GAME_STATUS.PLAYING, GAME_STATUS.GAME_OVER] as const;
const P_STATUS = [PLAYER_STATUS.IDLE, PLAYER_STATUS.READY, PLAYER_STATUS.IN, PLAYER_STATUS.OUT] as const;

const EVENT_TYPE: Record<number, string> = {
    [V.EVW_T_MAGIC_TRANSITION]: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION, [V.EVW_T_DEAL]: ANIMATION_EVENT_TYPE.DEAL,
    [V.EVW_T_FLIPPED]: ANIMATION_EVENT_TYPE.FLIPPED, [V.EVW_T_DEFENDER_MOVE]: ANIMATION_EVENT_TYPE.DEFENDER_MOVE,
    [V.EVW_T_ATTACK_PASS]: ANIMATION_EVENT_TYPE.ATTACK_PASS, [V.EVW_T_COVER]: ANIMATION_EVENT_TYPE.COVER,
    [V.EVW_T_PICKUP]: ANIMATION_EVENT_TYPE.PICKUP, [V.EVW_T_DISCARD]: ANIMATION_EVENT_TYPE.DISCARD,
    [V.EVW_T_OUT]: ANIMATION_EVENT_TYPE.OUT, [V.EVW_T_REFILL]: ANIMATION_EVENT_TYPE.REFILL,
    [V.EVW_T_CARDS_TO_TRASH]: ANIMATION_EVENT_TYPE.CARDS_TO_TRASH,
};
const LOCATION: Record<number, string> = {
    [V.EVW_LOC_DECK]: 'deck', [V.EVW_LOC_HAND]: 'hand', [V.EVW_LOC_TABLE]: 'table',
    [V.EVW_LOC_DISCARD]: 'discard', [V.EVW_LOC_FLIPPED]: 'flipped',
};
// The event types that always carry a card list, even an empty one.
const CARRIES_CARDS = new Set<string>([
    ANIMATION_EVENT_TYPE.ATTACK_PASS, ANIMATION_EVENT_TYPE.DISCARD, ANIMATION_EVENT_TYPE.PICKUP, ANIMATION_EVENT_TYPE.DEAL,
    ANIMATION_EVENT_TYPE.CARDS_TO_TRASH, ANIMATION_EVENT_TYPE.REFILL, ANIMATION_EVENT_TYPE.FLIPPED, ANIMATION_EVENT_TYPE.COVER,
]);

const card = (c: ViewCard): Card => ({ suit: c.suit, value: c.value });

export interface SnapshotOptions {
    /** The good timestamp to carry when the board has one running (its value is not on the wire). */
    prevGoodTs?: number | null;
    now?: () => number;
    /** Seat names for a view that names no one (a replay's frames). */
    names?: (string | null)[] | null;
    /** The game id and title of a view that names no one. */
    gameId?: string;
    title?: string;
}

/** The player id of a seat: the roster's, or the seat's own name when the view names no one. */
function seatId(v: TableView, s: number): string {
    return v.seats[s]?.id || `seat-${s}`;
}

/** A view as today's PersonalGame (a seat's) or PublicGame (a spectator's). */
export function snapshotToGame(v: TableView, opts: SnapshotOptions = {}): PersonalGame | PublicGame {
    const players: PublicPlayer[] = v.seats.map((seat, s) => ({
        player_id: seatId(v, s),
        name: seat.id ? seat.name : (opts.names?.[s] || (opts.names ? `P${s + 1}` : `seat-${s}`)),
        is_ai: seat.isAi,
        status: P_STATUS[seat.status] ?? PLAYER_STATUS.IDLE,
        hand_length: seat.handCount,
    }));
    const named = v.seats.length > 0 && v.seats[0].id !== '';
    const base: PublicGame = {
        id: named ? v.gameId : (opts.gameId ?? ''),
        name: named ? v.title : (opts.title ?? ''),
        deck_length: v.deckCount,
        discard_pile_length: v.discardPileLength,
        flipped: v.hasFlipped ? card(v.flipped) : null,
        players,
        status: G_STATUS[v.status] ?? GAME_STATUS.WAITING,
        power_suit: v.powerSuit,
        first_attacker: v.firstAttacker,
        defender: v.defender,
        table_battles: v.battles.map((b) => ({ attack: card(b.attack), defense: b.defense.value < 1 ? null : card(b.defense) })),
        elimination_order: v.elimination.map((s) => seatId(v, s)),
        good_players: players.filter((_, s) => ((v.goodMask >>> s) & 1) !== 0).map((p) => p.player_id),
        good_timestamp: v.hasGoodTimestamp ? (opts.prevGoodTs ?? (opts.now ?? Date.now)()) : null,
    };
    if (v.mySeat < 0 || v.mySeat >= players.length) return base;
    const self: PrivatePlayer = {
        ...players[v.mySeat],
        hand: v.myHand.map(card),
        awaiting_attack: v.seats[v.mySeat].awaitingAttack,
        strategy_key: 'human',
    };
    return { ...base, self } as PersonalGame;
}

/** An envelope's game, as the retired decodePackedGame returned it; null when it does not read whole. */
export function decodeEnvelope(bytes: Uint8Array, now?: () => number): { game: PersonalGame | PublicGame; version: number; seat: number } | null {
    const v = clientTable().adoptEnvelope(bytes);
    if (!v) return null;
    const game = snapshotToGame(v, { now });
    game.version = v.version;
    return { game, version: v.version, seat: v.mySeat };
}

/** One step of a push, as the animation pipeline's event. */
export interface SnapshotEvent {
    type: string;
    player_id?: string;
    cards?: Card[];
    from_location?: string;
    to_location?: string;
    target_card?: Card;
    battle_index?: number;
    game_state: PersonalGame | PublicGame;
}

function eventOf(e: PushEvent, view: TableView, opts: SnapshotOptions): SnapshotEvent {
    const type = EVENT_TYPE[e.type];
    const ev: SnapshotEvent = { type, game_state: snapshotToGame(view, opts) };
    if (e.seat >= 0) ev.player_id = seatId(view, e.seat);
    if (e.cards.length > 0 || CARRIES_CARDS.has(type)) ev.cards = e.cards.map(card);
    if (e.from >= 0) ev.from_location = LOCATION[e.from];
    if (e.to >= 0) ev.to_location = LOCATION[e.to];
    if (e.hasTarget) ev.target_card = card(e.target);
    if (e.battle >= 0) ev.battle_index = e.battle;
    return ev;
}

/** A push as the events and committed game the animation pipeline consumes. */
export function pushToSequence(read: PushRead, opts: SnapshotOptions = {}): {
    viewerSeat: number; events: SnapshotEvent[]; game: PersonalGame | PublicGame;
} {
    return {
        viewerSeat: read.final.mySeat,
        events: read.steps.map((s) => eventOf(s.event, s.view, opts)),
        game: snapshotToGame(read.final, opts),
    };
}

const G_CODE: Record<string, number> = {
    [GAME_STATUS.WAITING]: V.GAME_STATUS_WAITING, [GAME_STATUS.PLAYING]: V.GAME_STATUS_PLAYING, [GAME_STATUS.GAME_OVER]: V.GAME_STATUS_GAME_OVER,
};
const P_CODE: Record<string, number> = {
    [PLAYER_STATUS.IDLE]: V.PLAYER_STATUS_IDLE, [PLAYER_STATUS.READY]: V.PLAYER_STATUS_READY,
    [PLAYER_STATUS.IN]: V.PLAYER_STATUS_IN, [PLAYER_STATUS.OUT]: V.PLAYER_STATUS_OUT,
};
const NONE: ViewCard = { suit: V.CARD_NONE_SUIT, value: V.CARD_NONE_VALUE };

/**
 * A PersonalGame (or a spectator's PublicGame) as the board the web holds: the
 * inverse of snapshotToGame, for tests that hand a game literal to code that
 * takes a TableView. The viewer's seat is the seat `self` names (-1 without one).
 */
export function gameToView(g: PersonalGame | PublicGame): TableView {
    const self = (g as PersonalGame).self ?? null;
    const seatOf = (id: string) => g.players.findIndex((p) => p.player_id === id);
    const mySeat = self ? seatOf(self.player_id) : -1;
    let goodMask = 0;
    for (const id of g.good_players ?? []) { const s = seatOf(id); if (s >= 0) goodMask |= 1 << s; }
    return {
        status: G_CODE[g.status] ?? V.GAME_STATUS_WAITING,
        powerSuit: g.power_suit,
        firstAttacker: g.first_attacker,
        defender: g.defender,
        mySeat,
        fool: -1,
        deckCount: g.deck_length ?? 0,
        discardPileLength: g.discard_pile_length ?? 0,
        hasFlipped: !!g.flipped,
        hasGoodTimestamp: g.good_timestamp != null,
        flipped: g.flipped ? { suit: g.flipped.suit, value: g.flipped.value } : NONE,
        goodMask: goodMask >>> 0,
        version: g.version ?? 0,
        battles: (g.table_battles ?? []).map((b) => ({ attack: b.attack, defense: b.defense ?? NONE })),
        seats: g.players.map((p, s) => ({
            status: P_CODE[p.status] ?? V.PLAYER_STATUS_IDLE,
            handCount: p.hand_length ?? 0,
            awaitingAttack: s === mySeat && !!self?.awaiting_attack,
            isAi: !!p.is_ai,
            id: p.player_id,
            name: p.name ?? '',
        })),
        myHand: self?.hand ?? [],
        elimination: (g.elimination_order ?? []).map(seatOf).filter((s) => s >= 0),
        gameId: g.id ?? '',
        title: g.name ?? '',
    };
}
