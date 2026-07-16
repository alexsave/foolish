import { Card, Game, AnimationEvent, GAME_STATUS } from '../../core/types.ts';
import { verify_card_array, cardDisplay } from '../common_utils.ts';
import { kernelCover, kernelValidateCover } from '../../sdk/ts/wasm/engine.ts';

// The cover rules live in the C kernel (cnitro/src/game.c handle_cover),
// compiled to WASM — including the same-rank double-tap fix (attack cards
// matched by exact card, not value) and the whole end-of-round cascade
// (discard → refill → defender-out → rotation). This file keeps only the
// payload-shape guards and the exported API surface.

function verifyShapes(game: Game, cover_cards: Card[], attack_cards: Card[]): void {
    if (game.status !== GAME_STATUS.PLAYING) {
        throw new Error(`Game ${game.id} is not in playing state`);
    }
    verify_card_array(cover_cards, 'cover_cards');
    verify_card_array(attack_cards, 'attack_cards');
    // Kernel arrays are paired positionally; a mismatched request is a
    // malformed payload, rejected before marshaling (same message as the old
    // TS validator).
    if (cover_cards.length !== attack_cards.length) {
        throw new Error(`Cover cards ${cover_cards.map(card => cardDisplay(card)).join(', ')} and attack cards ${attack_cards.map(card => cardDisplay(card)).join(', ')} have different sizes`);
    }
}

// Validation function for cover moves
export function validateCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[]): void {
    verifyShapes(game, cover_cards, attack_cards);
    kernelValidateCover(game, player_id, cover_cards, attack_cards);
}

// Execution function for cover moves
export function executeCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[], _skipBroadcast: boolean = false): AnimationEvent[] {
    // Guard against modifying game state if game is already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return [];
    }
    return kernelCover(game, player_id, cover_cards, attack_cards);
}

// Combined function with validation
export function handleCover(game: Game, player_id: string, cover_cards: Card[], attack_cards: Card[]): AnimationEvent[] {
    verifyShapes(game, cover_cards, attack_cards);
    return kernelCover(game, player_id, cover_cards, attack_cards);
}
