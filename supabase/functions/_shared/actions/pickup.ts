import { Game, AnimationEvent, GAME_STATUS } from '../types.ts';
import { kernelPickup, kernelValidatePickup } from '../sdk/ts/wasm/engine.ts';

// The pickup rules live in the C kernel (cnitro/src/game.c handle_pickup),
// compiled to WASM — including the refill and the two-step defender rotation
// that skips the picker. This file keeps only the exported API surface.

// Validation function for pickup moves
export function validatePickup(game: Game, player_id: string): void {
    kernelValidatePickup(game, player_id);
}

// Combined function with validation (rejects on a finished game, exactly
// like the old validatePickup-first path).
export function handlePickup(game: Game, player_id: string): AnimationEvent[] {
    return kernelPickup(game, player_id);
}
