// Held-out eval: nitro 1v1 vs random and espresso across negative seeds.
// Reports overall win rate. Never iterate on these seeds.
//
// Usage: tsx offlinefun/localtest/nitro_eval.ts [N=10000]

import { calculateLegalMoves, registerBotStrategy } from '../../supabase/functions/_shared/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../../supabase/functions/_shared/pure_bot_actions.ts';
import { start_game, game_done } from '../../supabase/functions/_shared/common_utils.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, StrategyKey } from '../../supabase/functions/_shared/types.ts';
import { EspressoStrategy } from '../../supabase/functions/_shared/strategies/espresso_strategy.ts';
import { NitroStrategy } from '../../supabase/functions/_shared/strategies/nitro_strategy.ts';
import { setRandomSeed } from '../../supabase/functions/_shared/strategies/random_strategy.ts';
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

const createPlayer = (strategy: StrategyKey, index: number): PrivatePlayer => ({
    player_id: `bot_${index}_${strategy}`, name: `${strategy}${index}`, status: PLAYER_STATUS.READY,
    is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: strategy
});

const createGame = (heroStrat: StrategyKey, oppStrat: StrategyKey): Game => {
    const players: PrivatePlayer[] = [createPlayer(heroStrat, 0), createPlayer(oppStrat, 1)];
    return {
        players, deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
        deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
        first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
        good_timestamp: null, good_players: []
    };
};

// Normalize signed seed to unsigned 32-bit so LCG stays positive (Math.random must be in [0,1)).
const norm = (s: number): number => ((s >>> 0) || 1);

async function runOne(seed: number, oppStrat: StrategyKey, capIters = 2000): Promise<boolean> {
    _seed = norm(seed);
    setRandomSeed(norm(seed));
    const game = createGame(NITRO, oppStrat);
    start_game(game);
    let iter = 0;
    while (game_done(game) === null && iter < capIters) {
        iter++;
        const eligible: { bot: PrivatePlayer; index: number }[] = [];
        for (let i = 0; i < game.players.length; i++) {
            if (shouldBotActCore(game, game.players[i], i)) {
                const lm = calculateLegalMoves(game, game.players[i].player_id);
                if (lm.length > 0) eligible.push({ bot: game.players[i], index: i });
            }
        }
        if (eligible.length === 0) break;
        const shuffled = [...eligible];
        for (let i = shuffled.length - 1; i > 0; i--) {
            const j = Math.floor(Math.random() * (i + 1));
            [shuffled[i], shuffled[j]] = [shuffled[j], shuffled[i]];
        }
        let acted = false;
        for (const sb of shuffled) {
            const r = await processBotAction(game, sb.bot);
            if (r) { acted = true; break; }
        }
        if (!acted) break;
    }
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
