import { Card, Game, AnimationEvent, GAME_STATUS } from '../types.ts';
import { verify_card_array } from '../common_utils.ts';
import { kernelPass, kernelValidatePass } from '../wasm/engine.ts';

// The pass (perevod / transfer) rules live in the C kernel
// (cnitro/src/game.c handle_pass), compiled to WASM. This file keeps only
// the payload-shape guards and the exported API surface. The old TS
// implementation's post-mutation "Uncovered cards > defender_cards" throw is
// preserved: the kernel reports it as a rejection, so the move never commits.

function verifyShapes(game: Game, cards: Card[]): void {
    if (game.status !== GAME_STATUS.PLAYING) {
        throw new Error(`Game ${game.id} is not in playing state`);
    }
    verify_card_array(cards, 'cards');
    if (cards.length === 0) {
        throw new Error(`No cards provided`);
    }
}

// Validation function for pass moves
export function validatePass(game: Game, player_id: string, cards: Card[]): void {
    verifyShapes(game, cards);
    kernelValidatePass(game, player_id, cards);
}

// Execution function for pass moves
export function executePass(game: Game, player_id: string, cards: Card[]): AnimationEvent[] {
    // Guard against modifying game state if game is already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return [];
    }
    return kernelPass(game, player_id, cards);
}

// Combined function with validation
export function handlePass(game: Game, player_id: string, cards: Card[]): AnimationEvent[] {
    verifyShapes(game, cards);
    return kernelPass(game, player_id, cards);
}
