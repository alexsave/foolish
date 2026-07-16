// Shared local-simulation harness for the offlinefun/localtest scripts. These
// throwaway eval/debug scripts all build an all-bot Game, start it, and drive
// the bots to completion; this collects the pieces they had each copy-pasted.
//
// RNG note: some scripts override Math.random with a seeded LCG (so the engine's
// own draws/shuffles are deterministic); others leave Math.random alone and pass
// common_utils' seededRandom explicitly. stepBots/runBotsToCompletion therefore
// take the rng used for the eligible-bot shuffle as a parameter (default
// Math.random) so each caller keeps its exact behaviour.

import { calculateLegalMoves } from '@api/common/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '@api/common/pure_bot_actions.ts';
import {
    Game,
    PrivatePlayer,
    GAME_STATUS,
    PLAYER_STATUS,
    StrategyKey,
} from '@api/core/types.ts';

export const createPlayer = (strategy: StrategyKey, index: number): PrivatePlayer => ({
    player_id: `bot_${index}_${strategy}`,
    name: `${strategy}${index}`,
    status: PLAYER_STATUS.READY,
    is_ai: true,
    hand: [],
    awaiting_attack: false,
    hand_length: 0,
    strategy_key: strategy,
});

// Build a fresh PLAYING game with one bot seat per entry in `strategies`
// (seat 0 is the "hero" by convention). Deck/flip/positions are filled in by
// start_game(), exactly as the per-script copies did.
export const createGame = (strategies: StrategyKey[]): Game => ({
    players: strategies.map((s, i) => createPlayer(s, i)),
    deck: [],
    logs: [],
    id: 'g',
    name: 'g',
    status: GAME_STATUS.PLAYING,
    deck_length: 0,
    discard_pile_length: 0,
    flipped: null,
    power_suit: 0,
    first_attacker: 0,
    defender: 0,
    table_battles: [],
    elimination_order: [],
    good_timestamp: null,
    good_players: [],
});

// Normalize a (possibly negative) seed to an unsigned 32-bit value, never 0, so
// an LCG seeded from it stays positive and Math.random stays in [0, 1).
export const normSeed = (s: number): number => ((s >>> 0) || 1);

// One scheduling round: every bot that can act is collected, shuffled
// (Fisher-Yates — comparator shuffles can livelock V8's TimSort), and the first
// one that successfully acts ends the round. Returns whether any bot acted.
export async function stepBots(game: Game, rng: () => number = Math.random): Promise<boolean> {
    const eligible: { bot: PrivatePlayer; index: number }[] = [];
    for (let i = 0; i < game.players.length; i++) {
        if (shouldBotActCore(game, game.players[i], i)) {
            const lm = calculateLegalMoves(game, game.players[i].player_id);
            if (lm.length > 0) eligible.push({ bot: game.players[i], index: i });
        }
    }
    if (eligible.length === 0) return false;

    const shuffled = [...eligible];
    for (let i = shuffled.length - 1; i > 0; i--) {
        const j = Math.floor(rng() * (i + 1));
        [shuffled[i], shuffled[j]] = [shuffled[j], shuffled[i]];
    }

    for (const sb of shuffled) {
        const acted = await processBotAction(game, sb.bot);
        if (acted) return true;
    }
    return false;
}

import { game_done } from '@api/common/common_utils.ts';

// Drive a started game until someone is the fool, no bot can act, or capIters is
// hit. Returns the number of iterations run.
export async function runBotsToCompletion(
    game: Game,
    capIters = 2000,
    rng: () => number = Math.random
): Promise<number> {
    let iter = 0;
    while (game_done(game) === null && iter < capIters) {
        iter++;
        const acted = await stepBots(game, rng);
        if (!acted) break;
    }
    return iter;
}
