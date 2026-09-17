// pushSequence.ts - a push the client slot read, as the animation pipeline's sequence.
//
// The kernel reads every push (sdk/ts/table/client_table.ts readPush) into
// steps: what moved, and the board it left. The animation pipeline
// (AnimationContext.tsx, the replay and the tutorial) plays events keyed by the
// names it keys a card's flight by - a type, the acting seat, where the cards came
// from and went - and commits each step's board, a TableView, as it lands. This
// file names a step's event that way. Every board here is the kernel's snapshot as
// read; nothing is rebuilt.

import * as V from '@sdk/ts/gen/view_layout.bots.ts';
import type { PushEvent, PushRead, TableView, ViewCard } from '@sdk/ts/table/client_table.ts';

const EVENT_TYPE: Record<number, string> = {
    [V.EVW_T_MAGIC_TRANSITION]: 'magic_transition', [V.EVW_T_DEAL]: 'deal', [V.EVW_T_FLIPPED]: 'flipped',
    [V.EVW_T_DEFENDER_MOVE]: 'defender_move', [V.EVW_T_ATTACK_PASS]: 'attack_pass', [V.EVW_T_COVER]: 'cover',
    [V.EVW_T_PICKUP]: 'pickup', [V.EVW_T_DISCARD]: 'discard', [V.EVW_T_OUT]: 'out', [V.EVW_T_REFILL]: 'refill',
    [V.EVW_T_CARDS_TO_TRASH]: 'cards_to_trash',
};
const LOCATION: Record<number, string> = {
    [V.EVW_LOC_DECK]: 'deck', [V.EVW_LOC_HAND]: 'hand', [V.EVW_LOC_TABLE]: 'table',
    [V.EVW_LOC_DISCARD]: 'discard', [V.EVW_LOC_FLIPPED]: 'flipped',
};
// The event types that always carry a card list, even an empty one.
const CARRIES_CARDS = new Set([
    V.EVW_T_ATTACK_PASS, V.EVW_T_DISCARD, V.EVW_T_PICKUP, V.EVW_T_DEAL,
    V.EVW_T_CARDS_TO_TRASH, V.EVW_T_REFILL, V.EVW_T_FLIPPED, V.EVW_T_COVER,
]);

/** One step of a push, as the animation pipeline plays it. */
export interface ViewEvent {
    type: string;
    /** The acting seat. */
    seat?: number;
    cards?: ViewCard[];
    from_location?: string;
    to_location?: string;
    target_card?: ViewCard;
    battle_index?: number;
    /** The board this step leaves. */
    game_state: TableView;
}

function eventOf(e: PushEvent, view: TableView): ViewEvent {
    const ev: ViewEvent = { type: EVENT_TYPE[e.type], game_state: view };
    if (e.seat >= 0) ev.seat = e.seat;
    if (e.cards.length > 0 || CARRIES_CARDS.has(e.type)) ev.cards = [...e.cards];
    if (e.from >= 0) ev.from_location = LOCATION[e.from];
    if (e.to >= 0) ev.to_location = LOCATION[e.to];
    if (e.hasTarget) ev.target_card = e.target;
    if (e.battle >= 0) ev.battle_index = e.battle;
    return ev;
}

/** A push as the events and committed board the animation pipeline consumes. */
export function pushToSequence(read: PushRead): { viewerSeat: number; events: ViewEvent[]; game: TableView } {
    return {
        viewerSeat: read.final.mySeat,
        events: read.steps.map((s) => eventOf(s.event, s.view)),
        game: read.final,
    };
}
