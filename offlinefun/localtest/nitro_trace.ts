// Trace one game between nitro and an opponent for a given seed.
//
// Usage: tsx offlinefun/localtest/nitro_trace.ts <seed> [opp=random|espresso]

import { calculateLegalMoves, registerBotStrategy } from '../../supabase/functions/_shared/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../../supabase/functions/_shared/pure_bot_actions.ts';
import { start_game, game_done } from '../../supabase/functions/_shared/common_utils.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, StrategyKey, Card } from '../../supabase/functions/_shared/types.ts';
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

const SUITS = ['♠', '♥', '♦', '♣'];
const VALUES = ['', '', '', '', '', '5', '6', '7', '8', '9', 'T', 'J', 'Q', 'K', 'A'];
const cs = (c: Card) => `${VALUES[c.value]}${SUITS[c.suit]}`;
const cl = (cards: Card[]) => '[' + cards.map(cs).join(',') + ']';

const createPlayer = (strategy: StrategyKey, index: number, label: string): PrivatePlayer => ({
    player_id: `bot_${index}_${strategy}`, name: label, status: PLAYER_STATUS.READY,
    is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: strategy
});

const createGame = (heroStrat: StrategyKey, oppStrat: StrategyKey): Game => {
    const players: PrivatePlayer[] = [
        createPlayer(heroStrat, 0, 'N0'),
        createPlayer(oppStrat, 1, oppStrat === ESPRESSO ? 'E1' : 'R1'),
    ];
    return {
        players, deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
        deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
        first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
        good_timestamp: null, good_players: []
    };
};

const norm = (s: number): number => ((s >>> 0) || 1);

async function trace(seed: number, opp: StrategyKey): Promise<void> {
    _seed = norm(seed);
    setRandomSeed(norm(seed));
    const game = createGame(NITRO, opp);
    start_game(game);
    print(`=== TRACE seed=${seed} nitro vs ${opp} ===`);
    print(`Power suit: ${SUITS[game.power_suit]}, flipped: ${game.flipped ? cs(game.flipped) : 'none'}`);
    print(`First attacker: ${game.players[game.first_attacker].name}`);
    print(`Initial hands:`);
    for (const p of game.players) print(`  ${p.name}: ${cl(p.hand)}`);
    print(`Deck size: ${game.deck.length}`);

    let iter = 0;
    while (game_done(game) === null && iter++ < 1500) {
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
        const tableBefore = game.table_battles.length;
        const coveredBefore = game.table_battles.filter(b => b.defense !== null).length;
        const goodBefore = [...game.good_players];
        for (const sb of shuffled) {
            const handBefore = [...sb.bot.hand];
            const r = await processBotAction(game, sb.bot);
            if (r) {
                const tableAfter = game.table_battles.length;
                const coveredAfter = game.table_battles.filter(b => b.defense !== null).length;
                const handAfter = sb.bot.hand;
                const goodAfter = game.good_players;
                let action = '';
                if (tableAfter === 0 && tableBefore > 0) action = 'roundend';
                else if (tableAfter > tableBefore) {
                    const newCards = handBefore.filter(hb => !handAfter.some(h => h.value === hb.value && h.suit === hb.suit));
                    action = `attack ${cl(newCards)}`;
                } else if (coveredAfter > coveredBefore) {
                    const newCards = handBefore.filter(hb => !handAfter.some(h => h.value === hb.value && h.suit === hb.suit));
                    action = `cover ${cl(newCards)}`;
                } else if (goodAfter.length > goodBefore.length) {
                    action = 'good';
                } else if (handAfter.length > handBefore.length) {
                    action = 'pickup';
                } else {
                    action = '?';
                }
                const tbStr = game.table_battles.map(b =>
                    b.defense ? `${cs(b.attack)}>${cs(b.defense)}` : cs(b.attack)
                ).join(' | ');
                const handsStr = game.players.map(p => `${p.name}:${cl(p.hand)}`).join(' ');
                print(`[${iter}] ${sb.bot.name} ${action} | def=${game.players[game.defender].name} table=[${tbStr}] ${handsStr} deck=${game.deck.length} good=${game.good_players.length}`);
                break;
            }
        }
    }
    const loserId = game_done(game);
    print(`=== END iters=${iter} loser=${loserId} ===`);
    print(`final hands: ${game.players.map(p => `${p.name}:${cl(p.hand)}`).join(' | ')}`);
}

(async () => {
    const seed = parseInt(process.argv[2] ?? '1', 10);
    const opp = (process.argv[3] ?? 'random') as StrategyKey;
    const oppStrat: StrategyKey = (opp === 'espresso' ? ESPRESSO : opp === 'random' ? STRATEGY_KEY.RANDOM : opp);
    await trace(seed, oppStrat);
})();
