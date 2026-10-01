// Client-side move gates for the UI (which action buttons show, what the Cover
// button does) and the optimistic-apply pre-check.
//
// The RULES are the C kernel's. The pre-check hands the move's action wire and
// the board the screen holds to the client slot (sdk/ts/table/client_table.ts
// validate), which dry-runs the engine on that board (c/src/client_table.h
// client_validate). The engine judges a seat's own move by its own hand, the
// public table and the other seats' counts only, so the board's hidden cards
// cannot change a verdict; e2e/client_guards and c/tests
// (test_client_validate_is_the_engine) hold that to the server.
//
// Which action buttons the board shows is the kernel's answer too: boardPills hands
// the selection and the board to client_play, which answers with the PLAY_PILL_*
// bits (legal.h play_pills) - the rule the iMessage board draws by, so a
// selection never leaves Take or Good under the finger. The Cover button's aim
// comes back from the same call (coverGesture: play_best_cover_target, then
// play_resolve), so a live button can never turn out to have no move.
//
// Every gate takes the board the screen holds: the kernel's TableView snapshot
// (src/state/view.ts), the viewer's seat its `mySeat`. The gates are synchronous:
// bots.wasm is loaded before any board renders - every route that can show one
// is wrapped in src/components/KernelGate.tsx, asserted from the import graph by
// e2e/validation/kernel_gate_validation.test.ts.

import { clientTable, type ClientPlay } from '@sdk/ts/table/client_table.ts';
import { CLIENT_PLAY_COVER_BUTTON, MOVE_COVER } from '@sdk/ts/gen/view_layout.bots.ts';
import { rejectMessage } from '../wasm/rejectMessages';
import type { TableView, ViewCard as Card } from '../state/view';

type Cards = readonly Card[];

// The kernel's one answer for a selection: its PLAY_PILL_* bits and, for the
// Cover button, the move the button makes. A spectator has no pills.
//
// The website hands in no PLAY_HOST_* bit (legal.h): it has no Send to wait on,
// no newer chain, no throw-in hold and no board that refuses a play while it
// moves, and a press in flight is ActionButtons' pressedActions, per button,
// which would take every other button down with it as a host bit.
const gesture = (view: TableView, selected: Cards): ClientPlay | null =>
    view.mySeat < 0 ? null : clientTable().play(view, selected, CLIENT_PLAY_COVER_BUTTON);

/** The PLAY_PILL_* bits of the action buttons to draw for `selected` on `view`. */
export const boardPills = (view: TableView, selected: Cards): number => gesture(view, selected)?.pills ?? 0;

/**
 * What the Cover button does with this selection, or null when there is no
 * cover to make: the kernel aims it (legal.h play_best_cover_target - the
 * highest attack the selection beats, trumps outranking everything, ties to the
 * leftmost) and resolves the move that lands there (play_resolve).
 */
export const coverGesture = (view: TableView, selected: Cards): ClientPlay | null => {
    const p = selected.length === 0 ? null : gesture(view, selected);
    return p && p.moveType === MOVE_COVER ? p : null;
};

// Whether `defense` beats `attack` under the board's power suit (the kernel's can_cover).
export const canCoverPair = (attack: Card, defense: Card, powerSuit: number): boolean =>
    clientTable().canCover(attack, defense, powerSuit);

// ---- the optimistic-apply pre-check -------------------------------------------
// Reject an illegal optimistic move before it animates locally. The kernel is
// the authority; the server re-validates and returns the same message.

// Gate the EXACT awire bytes that will be POSTed - the caller builds the buffer
// once and shares it between this validation and the send. Throws the
// ENGINE_REJECT_* message on an illegal or malformed wire.
export const validateActionWire = (view: TableView, wire: Uint8Array): void => {
    const code = clientTable().validate(view, wire);
    if (code !== 0) throw new Error(rejectMessage(code));
};
