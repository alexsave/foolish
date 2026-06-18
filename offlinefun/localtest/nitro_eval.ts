// Held-out eval: nitro 1v1 vs random and espresso across negative seeds.
// Reports overall win rate. Never iterate on these seeds.
//
// Usage: tsx offlinefun/localtest/nitro_eval.ts [N=10000]

import { registerBotStrategy } from '../../supabase/functions/_shared/bot_strategy.ts';
import { start_game, game_done } from '../../supabase/functions/_shared/common_utils.ts';
import { STRATEGY_KEY, StrategyKey } from '../../supabase/functions/_shared/types.ts';
import { EspressoStrategy } from '../../supabase/functions/_shared/strategies/espresso_strategy.ts';
import { NitroStrategy } from '../../supabase/functions/_shared/strategies/nitro_strategy.ts';
import { setRandomSeed } from '../../supabase/functions/_shared/strategies/random_strategy.ts';
import { createGame, normSeed, runBotsToCompletion } from './harness.ts';
import * as fs from 'node:fs';

const NITRO = 'nitro' as StrategyKey;
const ESPRESSO = 'espresso' as StrategyKey;
registerBotStrategy(NITRO, new NitroStrategy());
registerBotStrategy(ESPRESSO, new EspressoStrategy());

let _seed = 1;
const seededRandom = () => {
    _seed = (_seed * 1664525 + 1013904223) % 4294967296;
    return _seed / 4294967296;
};
Math.random = seededRandom;

const noop = () => { };
console.log = noop; console.warn = noop; console.error = noop; console.info = noop;
const print = (...args: any[]) => fs.writeSync(1, args.map(a => typeof a === 'string' ? a : JSON.stringify(a)).join(' ') + '\n');

async function runOne(seed: number, oppStrat: StrategyKey, capIters = 2000): Promise<boolean> {
    _seed = normSeed(seed);
    setRandomSeed(normSeed(seed));
    const game = createGame([NITRO, oppStrat]);
    start_game(game);
    await runBotsToCompletion(game, capIters);
    const loserId = game_done(game);
    if (loserId === null) return false; // unfinished = treat as nitro loss
    return loserId !== 'bot_0_nitro';
}

(async () => {
    const N = parseInt(process.argv[2] ?? '10000', 10);
    const start = Date.now();

    let randWins = 0;
    for (let i = 1; i <= N; i++) {
        const seed = -i;
        const won = await runOne(seed, STRATEGY_KEY.RANDOM);
        if (won) randWins++;
    }
    const randRate = randWins / N;

    let espWins = 0;
    for (let i = 1; i <= N; i++) {
        const seed = -i;
        const won = await runOne(seed, ESPRESSO);
        if (won) espWins++;
    }
    const espRate = espWins / N;

    const dt = ((Date.now() - start) / 1000).toFixed(2);
    print(`eval N=${N} vs random: ${randRate.toFixed(4)} (${randWins}/${N})`);
    print(`eval N=${N} vs espresso: ${espRate.toFixed(4)} (${espWins}/${N})`);
    print(`# rand=${randRate.toFixed(4)} esp=${espRate.toFixed(4)} dt=${dt}s`);
})();
