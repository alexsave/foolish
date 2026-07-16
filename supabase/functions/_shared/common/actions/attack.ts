import { Card, Game, AnimationEvent, GAME_STATUS } from '../../core/types.ts';
import { verify_card_array } from '../common_utils.ts';
import { kernelAttack, kernelValidateAttack } from '../../sdk/ts/wasm/engine.ts';

// The attack rules live in the C kernel (cnitro/src/game.c handle_attack),
// compiled to WASM. This file keeps only the payload-shape guards (malformed
// JSON never reaches the kernel) and the exported TS API surface. State
// mutation, legality, logs and animation events all come from the kernel —
// verified byte-identical to the old TS implementation by the differential
// parity harness before the swap.

// Validation function for attack moves
export function validateAttack(game: Game, player_id: string, cards: Card[]): void {
    verify_card_array(cards, 'cards');
    if (cards.length === 0) {
        throw new Error('No cards provided');
    }
    kernelValidateAttack(game, player_id, cards);
}

// Execution function for attack moves
export function executeAttack(game: Game, player_id: string, cards: Card[]): AnimationEvent[] {
    // Guard against modifying game state if game is already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return [];
    }
    return kernelAttack(game, player_id, cards);
}

// Combined function with validation
export function handleAttack(game: Game, player_id: string, cards: Card[]): AnimationEvent[] {
    verify_card_array(cards, 'cards');
    if (cards.length === 0) {
        throw new Error('No cards provided');
    }
    return kernelAttack(game, player_id, cards);
}
