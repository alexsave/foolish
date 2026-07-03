// Nitro sweep harness: run nitro 1v1 against opponent over a range of seeds.
// Reports first loss / pass status. Used to find the frontier.
//
// Usage:
//   tsx offlinefun/localtest/nitro_sweep.ts <opp> <fromSeed> <toSeed> [--all] [--first-loss]
//   opp: "random" | "espresso"
//   --all: list every losing seed in range
//   --first-loss: stop at the first losing seed (default)

import { registerBotStrategy } from '../../supabase/functions/_shared/bot_strategy.ts';
import { start_game, game_done } from '../../supabase/functions/_shared/common_utils.ts';
import { STRATEGY_KEY, StrategyKey } from '../../supabase/functions/_shared/types.ts';
import { EspressoStrategy } from './frozen/espresso_strategy.ts';
import { NitroStrategy } from '../../supabase/functions/_shared/strategies/nitro_strategy.ts';
import { setRandomSeed } from './frozen/random_strategy.ts';
import { createGame, normSeed, runBotsToCompletion } from './harness.ts';
import * as fs from 'node:fs';

const NITRO = 'nitro' as StrategyKey;
const ESPRESSO = 'espresso' as StrategyKey;
registerBotStrategy(NITRO, new NitroStrategy());
registerBotStrategy(ESPRESSO, new EspressoStrategy());

// Deterministic Math.random keyed by seed (game uses Math.random for shuffling and draws)
let _seed = 1;
const seededRandom = () => {
    _seed = (_seed * 1664525 + 1013904223) % 4294967296;
    return _seed / 4294967296;
};
Math.random = seededRandom;

const noop = () => { };
console.log = noop; console.warn = noop; console.error = noop; console.info = noop;
const print = (...args: any[]) => fs.writeSync(1, args.map(a => typeof a === 'string' ? a : JSON.stringify(a)).join(' ') + '\n');

async function runOne(seed: number, oppStrat: StrategyKey, capIters = 2000): Promise<{ heroLost: boolean; finished: boolean }> {
    _seed = normSeed(seed);
    setRandomSeed(normSeed(seed));
    const game = createGame([NITRO, oppStrat]);
    start_game(game);
    await runBotsToCompletion(game, capIters);
    const loserId = game_done(game);
    const finished = loserId !== null;
    // Hero is bot_0_nitro. If hero is the loser, hero lost. Treat unfinished games as loss too.
    const heroLost = !finished || loserId === 'bot_0_nitro';
    return { heroLost, finished };
}

(async () => {
    const args = process.argv.slice(2);
    const opp = (args[0] ?? 'random') as StrategyKey;
    const from = parseInt(args[1] ?? '1', 10);
    const to = parseInt(args[2] ?? '1000', 10);
    const showAll = args.includes('--all');
    const firstLoss = args.includes('--first-loss') || !showAll;

    const oppStrat: StrategyKey = (opp === 'espresso' ? ESPRESSO : opp === 'random' ? STRATEGY_KEY.RANDOM : opp);
    print(`# nitro vs ${oppStrat}, seeds ${from}..${to}`);

    let losses = 0;
    let firstLossSeed = -1;
    const lossSeeds: number[] = [];
    const start = Date.now();
    for (let s = from; s <= to; s++) {
        const r = await runOne(s, oppStrat);
        if (r.heroLost) {
            losses++;
            if (firstLossSeed === -1) firstLossSeed = s;
            lossSeeds.push(s);
            if (firstLoss) {
                print(`FIRST_LOSS=${s}`);
                break;
            }
        }
    }
    const dt = ((Date.now() - start) / 1000).toFixed(2);
    if (firstLoss) {
        if (firstLossSeed === -1) print(`PASS_ALL ${from}..${to} in ${dt}s`);
        // else: we already printed FIRST_LOSS=...
    } else {
        const total = to - from + 1;
        print(`losses=${losses}/${total} (${(losses/total).toFixed(4)}) in ${dt}s`);
        if (showAll && losses > 0) print(`loss_seeds=${lossSeeds.join(',')}`);
    }
})();
