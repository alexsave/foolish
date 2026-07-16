import { Game, AnimationEvent, GAME_STATUS } from '../../core/types.ts';
import { kernelGood, kernelRoundTransition } from '../../sdk/ts/wasm/engine.ts';

// The good rules live in the C kernel (cnitro/src/game.c handle_good),
// compiled to WASM — including the all-attackers-good + all-covered round
// transition (discard, refill, rotation). This file keeps only the exported
// API surface. The old 60-second auto-discard timeout stays disabled, same
// as the TS implementation it replaces.

// Shared logic for discarding cards and transitioning to next round.
// Kept exported for API compatibility (the kernel runs it inside handle_good;
// this standalone entry mirrors the old executeRoundTransition export).
export function executeRoundTransition(game: Game, reason: string): AnimationEvent[] {
    if (game.status === GAME_STATUS.GAME_OVER) {
        return [];
    }
    return kernelRoundTransition(game, reason);
}

// Combined function with validation (rejects on a finished game, exactly
// like the old validateGood-first path).
export function handleGood(game: Game, player_id: string): AnimationEvent[] {
    return kernelGood(game, player_id);
}
