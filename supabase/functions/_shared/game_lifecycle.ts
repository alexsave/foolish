// Game-start lifecycle — the one common_utils-era helper that reaches the
// wasm kernel. Split out of common_utils.ts so the CLIENT bundle (which
// imports common_utils for canCover / personalize_game / clone helpers)
// never statically pulls _shared/wasm/engine.ts and its embedded wasm
// base64; only server code and test/offline harnesses import this module.

import { Game, GAME_STATUS, AnimationEvent } from './types.ts';
import { MAX_PLAYERS } from './constants.ts';
import { applyKernelStateToGame, getLastDealSeedHex, kernelStartGame, PackedRunOk, runPackedStart } from './wasm/engine.ts';

// Starts the game with all the animations. The deal/flip/first-attacker
// rules live in the C kernel (cnitro/src/game.c start_game): player-major
// deal, non-Ace trump flip (Aces pushed back and redrawn), lowest-trump
// holder attacks first. The event stream (MAGIC → per-player DEAL → FLIPPED →
// DEFENDER_MOVE → MAGIC) is reconstructed from kernel snapshots, identical
// to the old TS implementation (verified by the differential parity harness).
export const start_game = (game: Game): AnimationEvent[] => {
    // Guard against starting game if it's already over
    if (game.status === GAME_STATUS.GAME_OVER) {
        return [];
    }
    // Defense in depth: the lobby caps players at MAX_PLAYERS, but never deal
    // an oversized lobby — more hands than the deck holds leaves a player with
    // no cards and crashes the deal. Reject cleanly instead.
    if (game.players.length > MAX_PLAYERS) {
        throw new Error(`Cannot start a game with ${game.players.length} players (max ${MAX_PLAYERS})`);
    }
    const events = kernelStartGame(game);
    game.game_seed = getLastDealSeedHex();   // persist the deal seed (audit/replay)
    return events;
}

// The packed twin (docs/PACKED_WIRE_CUTOVER.md): the deal already ran in the
// kernel; now its outputs stay bytes too — durable blob, masked log records,
// per-viewer DEAL/FLIPPED event streams — instead of JS AnimationEvents the
// TS encoder would re-pack. `game` is updated in place from the kernel state.
// Only for callers whose recipients already know the ROSTER (handleStart's
// all-ready branch); a start bundled with a roster change (add-bot
// auto-start) stays on the JS path, whose broadcast carries the
// self-describing roster extras.
export const start_game_packed = (game: Game): PackedRunOk => {
    if (game.players.length > MAX_PLAYERS) {
        throw new Error(`Cannot start a game with ${game.players.length} players (max ${MAX_PLAYERS})`);
    }
    const humanSeats = game.players.map((_, i) => i).filter(i => !game.players[i].is_ai);
    const run = runPackedStart(game, humanSeats);
    applyKernelStateToGame(game, run.post, null);
    game.game_seed = getLastDealSeedHex();   // persist the deal seed (audit/replay)
    return run;
}
