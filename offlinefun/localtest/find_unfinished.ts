// Find seeds where espresso vs N coffees hits the iter cap.
import { calculateLegalMoves, registerBotStrategy } from '../../supabase/functions/_shared/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../../supabase/functions/_shared/pure_bot_actions.ts';
import { game_done } from '../../supabase/functions/_shared/common_utils.ts';
import { start_game } from '../../supabase/functions/_shared/game_lifecycle.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, StrategyKey } from '../../supabase/functions/_shared/types.ts';
import { EspressoStrategy } from './frozen/espresso_strategy.ts';
import { setRandomSeed } from './frozen/random_strategy.ts';
import { createGame, runBotsToCompletion } from './harness.ts';
import * as fs from 'node:fs';

const ESPRESSO = 'espresso' as StrategyKey;
registerBotStrategy(ESPRESSO, new EspressoStrategy());

let _seed = 1;
const seededRandom = () => {
    _seed = (_seed * 1664525 + 1013904223) % 4294967296;
    return _seed / 4294967296;
};
Math.random = seededRandom;

const noop = () => { };
console.log = noop; console.warn = noop; console.error = noop; console.info = noop;
const print = (...args: any[]) => fs.writeSync(2, args.map(a => typeof a === 'string' ? a : JSON.stringify(a)).join(' ') + '\n');

const gameFor = (heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number): Game =>
    createGame([heroStrat, ...Array(numOpps).fill(oppStrat)]);

async function runOne(seed: number, heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number, capIters = 2000): Promise<{ iters: number; finished: boolean; gameAtEnd?: Game }> {
    _seed = seed;
    setRandomSeed(seed);
    const game = gameFor(heroStrat, oppStrat, numOpps);
    start_game(game);
    const iter = await runBotsToCompletion(game, capIters);
    const finished = game_done(game) !== null;
    return { iters: iter, finished, gameAtEnd: game };
}

async function runOneVerbose(seed: number, heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number, capIters = 200): Promise<void> {
    _seed = seed;
    setRandomSeed(seed);
    const game = gameFor(heroStrat, oppStrat, numOpps);
    start_game(game);
    print(`\nseed=${seed} START. power=${game.power_suit}, defender=${game.defender}, hands=${game.players.map(p => p.hand.length).join(',')}`);
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
        if (eligible.length === 0) { print(`  iter=${iter} no eligible — break`); break; }
        const shuffled = [...eligible];
        for (let i = shuffled.length - 1; i > 0; i--) {
            const j = Math.floor(Math.random() * (i + 1));
            [shuffled[i], shuffled[j]] = [shuffled[j], shuffled[i]];
        }
        const before = `def=${game.defender} table=${game.table_battles.length}(${game.table_battles.filter(b=>b.defense!==null).length}cov) hands=[${game.players.map(p=>p.hand.length).join(',')}] good=${game.good_players.length} elim=[${game.elimination_order.join(',')}]`;
        let acted = false;
        let actorName = '';
        let movetype = '';
        for (const sb of shuffled) {
            actorName = sb.bot.name;
            // Snapshot pre-action state to infer move type
            const tableBefore = game.table_battles.length;
            const coveredBefore = game.table_battles.filter(b => b.defense !== null).length;
            const handBefore = sb.bot.hand.length;
            const goodBefore = game.good_players.length;
            const r = await processBotAction(game, sb.bot);
            if (r) {
                acted = true;
                const tableAfter = game.table_battles.length;
                const coveredAfter = game.table_battles.filter(b => b.defense !== null).length;
                const handAfter = sb.bot.hand.length;
                const goodAfter = game.good_players.length;
                if (tableAfter > tableBefore) movetype = handAfter < handBefore ? 'attack/pass' : 'attack?';
                else if (coveredAfter > coveredBefore) movetype = 'cover';
                else if (goodAfter > goodBefore) movetype = 'good';
                else if (handAfter > handBefore) movetype = 'pickup';
                else if (tableAfter === 0 && tableBefore > 0) movetype = 'roundend';
                else movetype = '?';
                break;
            }
        }
        const after = `def=${game.defender} table=${game.table_battles.length}(${game.table_battles.filter(b=>b.defense!==null).length}cov) hands=[${game.players.map(p=>p.hand.length).join(',')}] good=${game.good_players.length}`;
        if (iter < 15 || iter > capIters - 30) {
            print(`  iter=${iter} ${actorName}.${movetype}  | before:${before}  | after:${after}`);
        }
        if (!acted) { print(`  iter=${iter} not acted — break`); break; }
    }
    const g = game;
    print(`\n  END iters=${iter} done=${game_done(game) !== null}`);
    print(`  hands=${g.players.map(p => `${p.name}:${p.hand.length}`).join(',')}`);
    print(`  status=${g.players.map(p => `${p.name}:${p.status}`).join(',')}`);
    print(`  table=${g.table_battles.length} (${g.table_battles.filter(b => b.defense !== null).length} cov)`);
    print(`  defender=${g.defender}(${g.players[g.defender].name}) first_attacker=${g.first_attacker}`);
    print(`  good_players=${JSON.stringify(g.good_players)} deck=${g.deck.length} flipped=${g.flipped ? 'y' : 'n'}`);
    print(`  elimination_order=${JSON.stringify(g.elimination_order)}`);
}

(async () => {
    print(`\nFinding unfinished games (espresso vs 3 coffees, 5000 seeds)...`);
    const unfinished: number[] = [];
    for (let s = 1; s <= 5000; s++) {
        const r = await runOne(s, ESPRESSO, STRATEGY_KEY.HANDWRITTEN, 3);
        if (!r.finished) unfinished.push(s);
    }
    print(`Total unfinished: ${unfinished.length}`);
    print(`Seeds: ${unfinished.join(', ')}`);

    if (unfinished.length > 0) {
        await runOneVerbose(unfinished[0], ESPRESSO, STRATEGY_KEY.HANDWRITTEN, 3);
    }
})();
