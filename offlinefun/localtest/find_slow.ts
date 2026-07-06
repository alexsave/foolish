// Goal: deterministic seed, find which coffee-vs-2-randoms game is slow.
// Run 100 games with seed reset each game to seed=i. Print per-game iter & ms.
import { calculateLegalMoves, registerBotStrategy } from '../../supabase/functions/_shared/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../../supabase/functions/_shared/pure_bot_actions.ts';
import { start_game, game_done } from '../../supabase/functions/_shared/common_utils.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, StrategyKey } from '../../supabase/functions/_shared/types.ts';
import { EspressoStrategy } from './frozen/espresso_strategy.ts';
import { setRandomSeed } from './frozen/random_strategy.ts';
import { createGame } from './harness.ts';

const ESPRESSO = 'espresso' as StrategyKey;
registerBotStrategy(ESPRESSO, new EspressoStrategy());

// Seed Math.random deterministically (LCG). Game uses Math.random in draw() and shuffles.
let _seed = 1;
const seededRandom = () => {
    _seed = (_seed * 1664525 + 1013904223) % 4294967296;
    return _seed / 4294967296;
};
Math.random = seededRandom;

const noop = () => { };
console.log = noop; console.warn = noop; console.error = noop; console.info = noop;
import * as fs from 'node:fs';
const print = (...args: any[]) => {
    const msg = args.map(a => typeof a === 'string' ? a : JSON.stringify(a)).join(' ');
    fs.writeSync(2, msg + '\n'); // stderr fd, sync
};

const gameFor = (heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number) =>
    createGame([heroStrat, ...Array(numOpps).fill(oppStrat)]);

(async () => {
    print('coffee vs 2 randoms — per-game (seeded), per-game cap 1.5s wall');
    let totalStart = Date.now();
    let totalGames = 0;
    for (let s = 167; s <= 167; s++) {
        print(`SEED=${s}`);
        _seed = s;
        setRandomSeed(s);
        const game = gameFor(STRATEGY_KEY.HANDWRITTEN, STRATEGY_KEY.RANDOM, 2);
        start_game(game);
        const t0 = Date.now();
        let iter = 0;
        const cap = 5000;
        let timedOut = false;
        let lastPrint = 0;
        while (game_done(game) === null && iter < cap) {
            if (Date.now() - t0 > 2000) { timedOut = true; break; }
            const iterT0 = Date.now();
            // skip the debug-only scan; rely on the real one below
            const iterMs = Date.now() - iterT0;
            if (false) {
                const a = game.players[0]; const b = game.players[1]; const c = game.players[2];
                print(`  [iter=${iter} legalMoveScan=${iterMs}ms] table=${game.table_battles.length}(${game.table_battles.filter(x => x.defense !== null).length}cov) def=${game.defender}(${game.players[game.defender].name}) hands=[${a.hand.length},${b.hand.length},${c.hand.length}] eligible=[${eligibleSummary}]`);
            }
            iter++;
            const eligible: { bot: PrivatePlayer; index: number }[] = [];
            for (let i = 0; i < game.players.length; i++) {
                if (shouldBotActCore(game, game.players[i], i)) {
                    const lmT = Date.now();
                    print(`  [iter ${iter}] calculateLegalMoves(${game.players[i].name}) defender=${game.defender}(${game.players[game.defender].name}) hands=[${game.players.map(p=>p.hand.length).join(',')}] table=${game.table_battles.length}(${game.table_battles.filter(b=>b.defense!==null).length}cov)`);
                    const lm = calculateLegalMoves(game, game.players[i].player_id);
                    const lmMs = Date.now() - lmT;
                    print(`  [iter ${iter}]   ${game.players[i].name}: ${lm.length} moves in ${lmMs}ms`);
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
                const pbT = Date.now();
                print(`  [iter ${iter}] processBotAction(${sb.bot.name})...`);
                const r = await processBotAction(game, sb.bot);
                print(`  [iter ${iter}]   ${sb.bot.name} result=${!!r} in ${Date.now() - pbT}ms`);
                if (r) { acted = true; break; }
            }
            if (!acted) break;
        }
        totalGames++;
        const dt = Date.now() - t0;
        const status = timedOut ? 'TIMEOUT' : (game_done(game) ? 'done' : 'STUCK');
        if (dt > 200 || timedOut || iter >= cap || !game_done(game)) {
            print(`  seed=${s}: ${dt}ms iters=${iter} ${status}`);
            if (timedOut || !game_done(game)) {
                print(`    hands=${game.players.map(p => `${p.name}:${p.hand.length}`).join(',')} good=${JSON.stringify(game.good_players)}`);
                print(`    awaiting=${game.players.map(p => p.awaiting_attack).join(',')} status=${game.players.map(p => p.status).join(',')}`);
                print(`    table=${game.table_battles.length} (covered=${game.table_battles.filter(b => b.defense !== null).length}) defender=${game.defender}(${game.players[game.defender].name}) deck=${game.deck.length} flipped=${game.flipped ? 'y' : 'n'}`);
            }
        }
    }
    print(`\nTotal: ${(Date.now() - totalStart) / 1000}s`);
})();
