// clientBoards.ts - the boards the web makes itself, each made by the kernel.
//
// A screen holds boards no server wrote: a move the player made before the
// server confirms it, a push's boards with the player's pending cards kept on
// them, the board a card flies home to, the rematch's lobby before its reset
// arrives. The kernel makes every one of them from a board the screen holds
// (c/src/client_table.h client_optimistic_apply, client_board_edit,
// client_rearrange_hand), through the client slot's generated writer and reader
// (sdk/ts/table/client_table.ts). This file only names the edits; it decides
// nothing about what a board becomes, and a board the kernel refuses to change
// is null for the caller to keep what it had.

import * as V from '@sdk/ts/gen/view_layout.bots.ts';
import { clientTable, type BoardEdit } from '@sdk/ts/table/client_table.ts';
import { NO_CARD, type TableView, type ViewCard } from './view';

const edit = (op: number, cards: readonly ViewCard[] = [], more: Partial<BoardEdit> = {}): BoardEdit =>
    ({ op, firstAttacker: 0, defender: 0, target: NO_CARD, cards, ...more });

/** A pending card of mine, standing on the table: over `target` when it is a cover, as an attack when it is none. */
export interface PendingCard { card: ViewCard; target?: ViewCard | null }

/** The board once my move (its action wire) stands on it. */
export const optimisticBoard = (view: TableView, wire: Uint8Array): TableView | null =>
    clientTable().optimisticApply(view, wire);

/** The board with my pending cards kept on it, in order. */
export const keepPending = (view: TableView, pending: readonly PendingCard[]): TableView | null =>
    clientTable().edit(view, ...pending.map((p) => edit(V.CLIENT_EDIT_KEEP, [p.card], { target: p.target ?? NO_CARD })));

/** The board with the lead and the shield my pending pass gave. */
export const turnedBoard = (view: TableView, firstAttacker: number, defender: number): TableView | null =>
    clientTable().edit(view, edit(V.CLIENT_EDIT_TURN, [], { firstAttacker, defender }));

/** The board whose table is `cards`, uncovered. */
export const tableOf = (view: TableView, cards: readonly ViewCard[]): TableView | null =>
    clientTable().edit(view, edit(V.CLIENT_EDIT_TABLE, cards));

/** The board without every battle that holds one of `cards`. */
export const lifted = (view: TableView, cards: readonly ViewCard[]): TableView | null =>
    clientTable().edit(view, edit(V.CLIENT_EDIT_LIFT, cards));

/** The board with `cards` back in my hand. */
export const returnedToHand = (view: TableView, cards: readonly ViewCard[]): TableView | null =>
    clientTable().edit(view, edit(V.CLIENT_EDIT_RETURN, cards));

/** The rematch's lobby, before the server's reset arrives. */
export const lobbyBoard = (view: TableView): TableView | null =>
    clientTable().edit(view, edit(V.CLIENT_EDIT_LOBBY));

/** The outcome of a hand rearrange: the board, or why there is none. */
export type Rearranged = { kind: 'ordered'; view: TableView } | { kind: 'no-hand' } | { kind: 'not-an-order' };

/** My hand in the order `indices` gives. */
export const rearrangedBoard = (view: TableView, indices: readonly number[]): Rearranged => {
    const r = clientTable().rearrangeHand(view, indices);
    if (r.view) return { kind: 'ordered', view: r.view };
    return r.rc === V.CLIENT_E_MISMATCH ? { kind: 'no-hand' } : { kind: 'not-an-order' };
};
