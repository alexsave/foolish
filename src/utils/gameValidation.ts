// Client-side move gates for the UI (button enable / drag-drop) and the
// optimistic-apply pre-checks.
//
// The RULES now come from the C kernel (guards.wasm via ../wasm/clientGuards)
// — the SAME engine the server runs — instead of a second TypeScript
// reimplementation of handle_attack/handle_pass/handle_cover that could (and
// historically did) drift. e2e/pass_parity, e2e/attack_cover_parity and
// e2e/client_guards prove the delegation matches the authoritative kernel.
//
// What remains in TS is only the UI *affordance* layered on the rules:
// canCoverCards decides when to OFFER a one-click cover (i.e. when the covered
// set is unambiguous), a presentation choice, not a rule — and even that now
// asks the kernel (kernelUnambiguousCover), so no cover logic lives in TS.
//
// Every gate takes the board the screen holds: the kernel's TableView snapshot
// (src/state/view.ts), the viewer's seat its `mySeat`.
//
// Synchronicity: these gates are synchronous. In the browser the kernel is
// instantiated once at game load (await initClientGuards()); in Node/SSR/tests
// clientGuards falls back to a synchronous instantiate on first use.

import { kernelUnambiguousCover } from '@sdk/ts/wasm/bots.ts';
import * as guards from '../wasm/clientGuards.ts';
import { rejectMessage } from '../wasm/rejectMessages';
import { kernelTable, type TableView, type ViewCard as Card } from '../state/view';

type Cards = readonly Card[];

// ---- rule gates (delegated to the kernel) ----------------------------------

export const canAttack = (view: TableView, cards: Cards): boolean =>
    guards.canAttack(view, cards);

export const canPass = (view: TableView, cards: Cards): boolean =>
    guards.canPass(view, cards);

// May this seat take the table? The Take button's enable state, which the board
// used to spell out as "I am the defender and the table is not empty". That is
// handle_pickup's rule (c/src/game.c), and the kernel carries a third clause the
// hand-written one did not: the game must still be PLAYING.
export const canPickup = (view: TableView): boolean =>
    guards.canPickup(view);

// The seat that BECOMES the defender after the current defender passes / the
// bout ends — skips eliminated seats exactly like the server rotation. Used by
// the optimistic pass animation so its predicted defender matches the server's.
export const nextDefenderIndex = (view: TableView): number =>
    guards.nextPlayerIndex(view, view.defender);

// ---- cover offer (UI affordance over the kernel's can_cover) ----------------
// True when the selection covers uncovered attacks in exactly one unambiguous
// way — resolved in the kernel now (kernelUnambiguousCover -> legal.c
// unambiguous_cover), the one resolver every host shares (A7/F9). This is a
// display choice (whether to offer the one-click cover), not a rule.
export const canCoverCards = (view: TableView, selectedCards: Cards): boolean => {
    if (selectedCards.length === 0) return false;
    return kernelUnambiguousCover([...selectedCards], kernelTable(view.battles), view.powerSuit) !== null;
};

// ---- throwing validators (optimistic-apply pre-checks) ---------------------
// Reject an illegal optimistic move before it animates locally. The kernel is
// the authority (it enforces every rule, including the same-rank double-tap the
// TS mirror used to special-case); these throw a short reason and the server
// re-validates and returns the exact user-facing message.

export const validateAttack = (view: TableView, cards: Cards): void => {
    if (!guards.canAttack(view, cards)) throw new Error('Illegal attack');
};

export const validatePass = (view: TableView, cards: Cards): void => {
    if (!guards.canPass(view, cards)) throw new Error('Illegal pass');
};

export const validatePickup = (view: TableView): void => {
    if (!guards.canPickup(view)) throw new Error('Cannot pickup');
};

// Wire-based validator (docs/PACKED_WIRE_CUTOVER.md): gate the EXACT awire
// bytes that will be POSTed — the caller builds the buffer once
// (@shared/wire/awire.ts encodeAction) and shares it between this validation
// and the send. Throws the ENGINE_REJECT_* mirror message on an illegal or
// malformed wire. The per-move validators below stay for other callers.
export const validateActionWire = (view: TableView, wire: Uint8Array): void => {
    const code = guards.validateActionWire(view, wire);
    if (code !== 0) throw new Error(rejectMessage(code));
};

export const validateCover = (view: TableView, coverCards: Cards, attackCards: Cards): void => {
    if (coverCards.length === 0 || coverCards.length !== attackCards.length) {
        throw new Error('Cannot cover');
    }
    // guards.canCover runs the full kernel cover validation: each target must be
    // an uncovered attack on the table (exact-card match), no target named
    // twice, and every cover must legally beat its attack.
    if (!guards.canCover(view, coverCards, attackCards)) {
        throw new Error('Illegal cover');
    }
};
