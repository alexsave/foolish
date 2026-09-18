// view.ts - the board a screen draws, as the web holds it.
//
// A board is the kernel's TableView snapshot (c/src/client_table.h), copied out
// through the generated readers (sdk/ts/gen/view_layout.bots.ts) by the client
// slot (sdk/ts/table/client_table.ts). Components read its fields by the names
// the generator emitted: seats by index, the viewer's seat as `mySeat` (-1 for a
// spectator), who said good as `goodMask`. What a board shows that is a rule of
// the game - the sword, the shield, the viewer's Good, the stock in flight, a
// bot to move - is the kernel's answer, `rulesOf`.
//
// This file holds only the names a board is read with; it knows no byte and
// decides nothing.

import * as V from '@sdk/ts/gen/view_layout.bots.ts';
import { clientTable, type TableView, type ViewBattle, type ViewCard, type ViewRules, type ViewSeat } from '@sdk/ts/table/client_table.ts';

export type { TableView, ViewBattle, ViewCard, ViewRules, ViewSeat };

/** The kernel's no-card (an uncovered battle's defense, a board with no trump face up). */
export const NO_CARD: ViewCard = { suit: V.CARD_NONE_SUIT, value: V.CARD_NONE_VALUE };

/** A card, not the kernel's no-card. */
export const isCard = (c: ViewCard): boolean => !(c.suit === V.CARD_NONE_SUIT && c.value === V.CARD_NONE_VALUE);

/** The battle's attack has been covered. */
export const covered = (b: ViewBattle): boolean => isCard(b.defense);

/** The same card. */
export const sameCard = (a: ViewCard, b: ViewCard): boolean => a.suit === b.suit && a.value === b.value;

/** What `view` shows that is a rule of the game, with `fromDeck` cards in flight out of the stock, `toFlipped` of them to the trump slot. */
export const rulesOf = (view: TableView, fromDeck = 0, toFlipped = 0): ViewRules => clientTable().rules(view, fromDeck, toFlipped);

/**
 * The name the page gives a seat's hand and ring (data-player-id, React keys): the
 * seat's player id, or, for a seat nobody is signed in as (a replay's, the
 * tutorial's), its index. A name for the DOM, not an identity.
 */
export const seatKey = (view: TableView | null | undefined, seat: number): string =>
    view?.seats[seat]?.id || `seat-${seat}`;

/** Every card on the table, attacks before their covers. */
export const tableCards = (view: TableView): ViewCard[] =>
    view.battles.flatMap((b) => (covered(b) ? [b.attack, b.defense] : [b.attack]));

// ---- a gesture, in the kernel's terms ---------------------------------------
//
// What a gesture MEANS is client_play's (legal.h play_*), and it takes a target
// and a selection. Turning a pointer into those two is the browser's half, and
// it is the only half of a gesture the kernel cannot answer: these two put it
// where a test can reach it without a DOM.

/**
 * Where a drop landed, in the kernel's terms (legal.h PLAY_TARGET_*). `battle`
 * is the battle the pointer was over, from the page's own hit-test, or null.
 * A battle index the board does not hold is the open table, never a drop on a
 * battle that is not there.
 */
export const dropTarget = (view: TableView, inHand: boolean, battle: number | null): number => {
    if (inHand) return V.PLAY_TARGET_HAND;
    return battle !== null && battle >= 0 && battle < view.battles.length ? battle : V.PLAY_TARGET_TABLE;
};

/**
 * The cards a gesture carries: the whole selection when the dragged card is part
 * of it, otherwise the dragged card on its own. Dragging an unselected card is
 * how a player plays one card while several are picked.
 */
export const gestureCards = (selected: readonly ViewCard[], dragged: ViewCard): ViewCard[] =>
    selected.length > 0 && selected.some((c) => sameCard(c, dragged)) ? [...selected] : [dragged];

/** The GAME_STATUS_* and PLAYER_STATUS_* a board carries. */
export const GAME_STATUS = { WAITING: V.GAME_STATUS_WAITING, PLAYING: V.GAME_STATUS_PLAYING, GAME_OVER: V.GAME_STATUS_GAME_OVER } as const;
export const PLAYER_STATUS = { IDLE: V.PLAYER_STATUS_IDLE, READY: V.PLAYER_STATUS_READY, IN: V.PLAYER_STATUS_IN, OUT: V.PLAYER_STATUS_OUT } as const;
