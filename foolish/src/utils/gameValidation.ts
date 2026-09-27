// Client-side move gates for the UI (button enable / drag-drop) and the
// optimistic-apply pre-checks.
//
// The RULES are the C kernel's: every gate hands the move, as its action wire
// (@sdk/ts/wire/awire.ts encodeAction), and the board the screen holds to the
// client slot (sdk/ts/table/client_table.ts validate), which dry-runs the engine
// on that board (c/src/client_table.h client_validate). The engine judges a
// seat's own move by its own hand, the public table and the other seats' counts
// only, so the board's hidden cards cannot change a verdict; e2e/client_guards
// and c/tests (test_client_validate_is_the_engine) hold that to the server.
//
// The Cover button is the kernel's too: coverGesture hands the selection and
// the board to client_play, which aims the button (play_best_cover_target) and
// resolves the move that lands there (play_resolve) in one call, so the button's
// enable state and its click can never be two different answers.
//
// Every gate takes the board the screen holds: the kernel's TableView snapshot
// (src/state/view.ts), the viewer's seat its `mySeat`. The gates are synchronous:
// bots.wasm is loaded before any board renders - every route that can show one
// is wrapped in src/components/KernelGate.tsx, asserted from the import graph by
// e2e/validation/kernel_gate_validation.test.ts.

import { encodeAction, type AwireMove } from '@sdk/ts/wire/awire.ts';
import { clientTable, type ClientPlay } from '@sdk/ts/table/client_table.ts';
import { CLIENT_PLAY_COVER_BUTTON, MOVE_COVER } from '@sdk/ts/gen/view_layout.bots.ts';
import { rejectMessage } from '../wasm/rejectMessages';
import type { TableView, ViewCard as Card } from '../state/view';

type Cards = readonly Card[];

// The kernel's verdict on a move: 0 legal. A move the wire cannot carry is no move.
const verdict = (view: TableView, action: AwireMove): number => {
    let wire: Uint8Array;
    try { wire = encodeAction(action); } catch { return -1; }
    return clientTable().validate(view, wire);
};

// ---- rule gates ------------------------------------------------------------

export const canAttack = (view: TableView, cards: Cards): boolean =>
    cards.length > 0 && verdict(view, { kind: 'attack', cards: [...cards] }) === 0;

export const canPass = (view: TableView, cards: Cards): boolean =>
    cards.length > 0 && verdict(view, { kind: 'pass', cards: [...cards] }) === 0;

// May this seat take the table? The Take button's enable state (handle_pickup's rule).
export const canPickup = (view: TableView): boolean =>
    verdict(view, { kind: 'pickup' }) === 0;

// ---- the Cover button ------------------------------------------------------
// What the button does with this selection, and so whether to show it at all:
// the kernel aims it (legal.h play_best_cover_target - the highest attack the
// selection beats, trumps outranking everything, ties to the leftmost) and
// resolves the move that lands there (play_resolve). One answer for the enable
// state and the click, so a live button cannot turn out to have no move.
export const coverGesture = (view: TableView, selectedCards: Cards): ClientPlay | null => {
    if (selectedCards.length === 0) return null;
    const p = clientTable().play(view, selectedCards, CLIENT_PLAY_COVER_BUTTON);
    return p.moveType === MOVE_COVER ? p : null;
};

/** Whether the Cover button is live for this selection. */
export const canCoverCards = (view: TableView, selectedCards: Cards): boolean =>
    coverGesture(view, selectedCards) !== null;

// Whether `defense` beats `attack` under the board's power suit (the kernel's can_cover).
export const canCoverPair = (attack: Card, defense: Card, powerSuit: number): boolean =>
    clientTable().canCover(attack, defense, powerSuit);

// ---- throwing validators (optimistic-apply pre-checks) ---------------------
// Reject an illegal optimistic move before it animates locally. The kernel is
// the authority; these throw a short reason and the server re-validates and
// returns the exact user-facing message.

export const validateAttack = (view: TableView, cards: Cards): void => {
    if (!canAttack(view, cards)) throw new Error('Illegal attack');
};

export const validatePass = (view: TableView, cards: Cards): void => {
    if (!canPass(view, cards)) throw new Error('Illegal pass');
};

export const validatePickup = (view: TableView): void => {
    if (!canPickup(view)) throw new Error('Cannot pickup');
};

// Gate the EXACT awire bytes that will be POSTed - the caller builds the buffer
// once and shares it between this validation and the send. Throws the
// ENGINE_REJECT_* message on an illegal or malformed wire.
export const validateActionWire = (view: TableView, wire: Uint8Array): void => {
    const code = clientTable().validate(view, wire);
    if (code !== 0) throw new Error(rejectMessage(code));
};

export const validateCover = (view: TableView, coverCards: Cards, attackCards: Cards): void => {
    if (coverCards.length === 0 || coverCards.length !== attackCards.length) {
        throw new Error('Cannot cover');
    }
    // The kernel's cover validation: each target must be an uncovered attack on
    // the table (exact-card match), no target named twice, and every cover must
    // legally beat its attack.
    if (verdict(view, { kind: 'cover', cards: [...coverCards], attack_cards: [...attackCards] }) !== 0) {
        throw new Error('Illegal cover');
    }
};
