/* =============================================================================
 * A seeded game, played and encoded — the fixture that cannot rot
 * =============================================================================
 * Tests used to hold frozen replay codes as string constants. That does not
 * work, and the repo has the scars: a replay code is only readable by the kernel
 * that cut it (the arithmetic coder's probability model IS the legal-move menu,
 * so any menu change renumbers every choice), and every frozen code is orphaned
 * the moment the menu moves. The Oracle suite's octogen-4v4 fixture had been
 * dead for exactly that reason — "leftover data after game end" — with its test
 * red and nobody the wiser.
 *
 * So: play a real seeded game with the real engine and encode it here, at the
 * version under test. Same game every run (the seed pins the deal AND the bots),
 * no constant to go stale.
 * ========================================================================== */

import { start_game } from '../../supabase/functions/_shared/common/game_lifecycle.ts';
import { game_done } from '../../supabase/functions/_shared/common/common_utils.ts';
import {
    Game, GAME_STATUS, PLAYER_STATUS, PrivatePlayer, StrategyKey,
} from '../../supabase/functions/_shared/core/types.ts';
import { shouldBotActCore, processBotAction } from '../../supabase/functions/_shared/common/pure_bot_actions.ts';
import { calculateLegalMoves } from '../../supabase/functions/_shared/common/bot_strategy.ts';
import { kernelReplayEncodeV6FromGame } from '../../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import { __setDealSeedOverride } from '../../supabase/functions/_shared/sdk/ts/wasm/engine.ts';

export const seedBytes = (np: number, s: number): Uint8Array =>
    new Uint8Array(Array.from({ length: 32 }, (_, i) => (i * 31 + s * 13 + np) & 0xff));

const seedHex = (b: Uint8Array) =>
    Array.from(b).map((x) => x.toString(16).padStart(2, '0')).join('');

export interface SeededGame {
    game: Game;
    /** The v6 replay code of the finished game, cut by the kernel under test. */
    code: Uint8Array;
    seed: Uint8Array;
}

/**
 * A seeded game played to the end and encoded as v6, exactly as the finalize
 * path produces one. Returns null if the game did not finish.
 *
 * The deal-seed override is load-bearing: without it the TS engine deals from
 * its own RNG while the kernel re-deals from the seed to encode, so it rebuilds
 * a DIFFERENT game — which surfaces as "logged attack not in menu".
 */
export async function playSeededV6(
    np: number, s: number, strategy: StrategyKey = 'handwritten' as StrategyKey,
): Promise<SeededGame | null> {
    const seed = seedBytes(np, s);
    const game = {
        players: Array.from({ length: np }, (_, i): PrivatePlayer => ({
            player_id: `seat-${i}`, name: `P${i + 1}`, status: PLAYER_STATUS.READY,
            is_ai: true, hand: [], awaiting_attack: false, hand_length: 0,
            strategy_key: strategy,
        })),
        deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
        deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
        first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
        good_timestamp: null, good_players: [], game_seed: seedHex(seed),
    } as unknown as Game;

    __setDealSeedOverride(seed);
    try {
        start_game(game);
        for (let guard = 0; guard < 20000 && game_done(game) === null; guard++) {
            let acted = false;
            for (let i = 0; i < game.players.length && !acted; i++) {
                const p = game.players[i];
                if (!shouldBotActCore(game, p, i)) continue;
                if (calculateLegalMoves(game, p.player_id).length === 0) continue;
                acted = await processBotAction(game, p);
            }
            if (!acted) return null;
        }
    } finally {
        __setDealSeedOverride(null);
    }
    if (game_done(game) === null) return null;
    return { game, seed, code: kernelReplayEncodeV6FromGame(game, seed, undefined, 1 << 20) };
}
