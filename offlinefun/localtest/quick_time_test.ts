import { registerBotStrategy } from '../../supabase/functions/_shared/common/bot_strategy.ts';
import { game_done, seededRandom } from '../../supabase/functions/_shared/common/common_utils.ts';
import { start_game } from '../../supabase/functions/_shared/common/game_lifecycle.ts';
import { STRATEGY_KEY, StrategyKey } from '../../supabase/functions/_shared/core/types.ts';
import { EspressoStrategy } from './frozen/espresso_strategy.ts';
import { createGame, runBotsToCompletion } from './harness.ts';

const ESPRESSO = 'espresso' as StrategyKey;
registerBotStrategy(ESPRESSO, new EspressoStrategy());

const noop = () => { };
console.log = noop; console.warn = noop; console.error = noop; console.info = noop;
const print = (...args: any[]) => process.stdout.write(args.map(a => typeof a === 'string' ? a : JSON.stringify(a)).join(' ') + '\n');

const gameFor = (heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number) =>
    createGame([heroStrat, ...Array(numOpps).fill(oppStrat)]);

async function runOne(heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number, capIters = 1500): Promise<{ loserStrat: StrategyKey | null; iters: number }> {
    const game = gameFor(heroStrat, oppStrat, numOpps);
    start_game(game);
    const iter = await runBotsToCompletion(game, capIters, seededRandom);
    const loserId = game_done(game);
    if (!loserId) return { loserStrat: null, iters: iter };
    const loser = game.players.find(p => p.player_id === loserId);
    return { loserStrat: loser?.strategy_key as StrategyKey || null, iters: iter };
}

(async () => {
    const NGAMES = 50;
    print(`\nESPRESSO vs N RANDOM, ${NGAMES} games each, per-game timing > 200ms shown\n`);
    for (const opps of [1, 2, 3]) {
        const start = Date.now();
        let totalIters = 0, unfinished = 0, maxIters = 0, slow: string[] = [];
        for (let i = 0; i < NGAMES; i++) {
            const t0 = Date.now();
            const r = await runOne(ESPRESSO, STRATEGY_KEY.RANDOM, opps);
            const dt = Date.now() - t0;
            if (dt > 200) slow.push(`game${i}=${dt}ms,iters=${r.iters},finished=${r.loserStrat !== null}`);
            totalIters += r.iters;
            if (r.iters > maxIters) maxIters = r.iters;
            if (r.loserStrat === null) unfinished++;
        }
        const elapsed = (Date.now() - start) / 1000;
        print(`espresso vs ${opps} random:  ${elapsed.toFixed(2)}s, avg ${(totalIters / NGAMES).toFixed(0)} iters, max ${maxIters}, unfinished=${unfinished}`);
        if (slow.length > 0) print(`  slow games: ${slow.slice(0, 5).join('; ')}${slow.length > 5 ? `...(${slow.length} total)` : ''}`);
    }
})();
