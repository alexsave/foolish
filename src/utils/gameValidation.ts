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
// Synchronicity: these gates are synchronous. In the browser the kernel is
// instantiated once at game load (await initClientGuards()); in Node/SSR/tests
// clientGuards falls back to a synchronous instantiate on first use.

import { Card, PersonalGame } from '@shared/types.ts';
import { kernelUnambiguousCover } from '@shared/wasm/bots.ts';
import * as guards from '../wasm/clientGuards.ts';
import { rejectMessage } from '../wasm/rejectMessages';

// ---- rule gates (delegated to the kernel) ----------------------------------

export const canAttack = (game: PersonalGame, cards: Card[]): boolean =>
    guards.canAttack(game, cards);

export const canPass = (game: PersonalGame, cards: Card[]): boolean =>
    guards.canPass(game, cards);

// The seat that BECOMES the defender after the current defender passes / the
// bout ends — skips eliminated seats exactly like the server rotation. Used by
// the optimistic pass animation so its predicted defender matches the server's.
export const nextDefenderIndex = (game: PersonalGame): number =>
    guards.nextPlayerIndex(game, game.defender);

// ---- cover offer (UI affordance over the kernel's can_cover) ----------------
// True when the selection covers uncovered attacks in exactly one unambiguous
// way — resolved in the kernel now (kernelUnambiguousCover -> legal.c
// unambiguous_cover), the one resolver every host shares (A7/F9). This is a
// display choice (whether to offer the one-click cover), not a rule.
export const canCoverCards = (game: PersonalGame, selectedCards: Card[]): boolean => {
    if (selectedCards.length === 0) return false;
    return kernelUnambiguousCover(selectedCards, game.table_battles, game.power_suit) !== null;
};

// ---- throwing validators (optimistic-apply pre-checks) ---------------------
// Reject an illegal optimistic move before it animates locally. The kernel is
// the authority (it enforces every rule, including the same-rank double-tap the
// TS mirror used to special-case); these throw a short reason and the server
// re-validates and returns the exact user-facing message.

export const validateAttack = (game: PersonalGame, cards: Card[]): void => {
    if (!guards.canAttack(game, cards)) throw new Error('Illegal attack');
};

export const validatePass = (game: PersonalGame, cards: Card[]): void => {
    if (!guards.canPass(game, cards)) throw new Error('Illegal pass');
};

export const validatePickup = (game: PersonalGame): void => {
    if (!guards.canPickup(game)) throw new Error('Cannot pickup');
};

// Wire-based validator (docs/PACKED_WIRE_CUTOVER.md): gate the EXACT awire
// bytes that will be POSTed — the caller builds the buffer once
// (@shared/wire/awire.ts encodeAction) and shares it between this validation
// and the send. Throws the ENGINE_REJECT_* mirror message on an illegal or
// malformed wire. The per-move validators below stay for other callers.
export const validateActionWire = (game: PersonalGame, wire: Uint8Array): void => {
    const code = guards.validateActionWire(game, wire);
    if (code !== 0) throw new Error(rejectMessage(code));
};

export const validateCover = (game: PersonalGame, coverCards: Card[], attackCards: Card[]): void => {
    if (coverCards.length === 0 || coverCards.length !== attackCards.length) {
        throw new Error('Cannot cover');
    }
    // guards.canCover runs the full kernel cover validation: each target must be
    // an uncovered attack on the table (exact-card match), no target named
    // twice, and every cover must legally beat its attack.
    if (!guards.canCover(game, coverCards, attackCards)) {
        throw new Error('Illegal cover');
    }
};
