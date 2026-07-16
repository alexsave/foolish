// Find a seed where espresso loses 1v1 vs random, then trace the full game.
import { calculateLegalMoves, registerBotStrategy } from '@api/common/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '@api/common/pure_bot_actions.ts';
import { game_done } from '@api/common/common_utils.ts';
import { start_game } from '@api/common/game_lifecycle.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, StrategyKey, Card } from '@api/core/types.ts';
import { EspressoStrategy } from './frozen/espresso_strategy.ts';
import { setRandomSeed } from './frozen/random_strategy.ts';
import { runBotsToCompletion } from './harness.ts';
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

const SUITS = ['♠', '♥', '♦', '♣'];
const VALUES = ['', '', '', '', '', '5', '6', '7', '8', '9', 'T', 'J', 'Q', 'K', 'A'];
const cs = (c: Card) => `${VALUES[c.value]}${SUITS[c.suit]}`;
const cl = (cards: Card[]) => '[' + cards.map(cs).join(',') + ']';

const createPlayer = (strategy: StrategyKey, index: number): PrivatePlayer => ({
    player_id: `bot_${index}_${strategy}`, name: `${strategy === 'espresso' ? 'E' : strategy === 'random' ? 'R' : 'C'}${index}`,
    status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: strategy
});
const createGame = (heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number): Game => {
    const players: PrivatePlayer[] = [createPlayer(heroStrat, 0)];
    for (let i = 0; i < numOpps; i++) players.push(createPlayer(oppStrat, i + 1));
    return { players, deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
        deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
        first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
        good_timestamp: null, good_players: [] };
};

async function runOne(seed: number): Promise<{ heroLost: boolean; iters: number }> {
    _seed = seed; setRandomSeed(seed);
    const game = createGame(ESPRESSO, STRATEGY_KEY.HANDWRITTEN, 1);
    start_game(game);
    const iter = await runBotsToCompletion(game, 1500);
    const loserId = game_done(game);
    const heroLost = loserId === 'bot_0_espresso';
    return { heroLost, iters: iter };
}

async function trace(seed: number): Promise<void> {
    _seed = seed; setRandomSeed(seed);
    const game = createGame(ESPRESSO, STRATEGY_KEY.HANDWRITTEN, 1);
    start_game(game);

    print(`\n=== TRACE seed=${seed} ===`);
    print(`Power suit: ${SUITS[game.power_suit]}, flipped: ${game.flipped ? cs(game.flipped) : 'none'}`);
    print(`First attacker: ${game.first_attacker === 0 ? 'E0 (espresso)' : 'R1 (random)'}`);
    print(`Initial hands:`);
    for (const p of game.players) print(`  ${p.name}: ${cl(p.hand)}`);
    print(`Deck size: ${game.deck.length}, discard: ${game.discard_pile_length}`);

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

        // Snapshot pre-state
        const tableBefore = game.table_battles.length;
        const coveredBefore = game.table_battles.filter(b => b.defense !== null).length;
        const handsBefore = game.players.map(p => p.hand.length);
        const goodBefore = [...game.good_players];

        for (const sb of shuffled) {
            const handBefore = [...sb.bot.hand];
            const r = await processBotAction(game, sb.bot);
            if (r) {
                // Infer move type
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
                    action = 'pass?';
                }
                const tbStr = game.table_battles.map(b =>
                    b.defense ? `${cs(b.attack)}>${cs(b.defense)}` : cs(b.attack)
                ).join(' | ');
                print(`[${iter}] ${sb.bot.name} ${action} | def=${game.players[game.defender].name} table=[${tbStr}] hands=[E:${game.players[0].hand.length},R:${game.players[1].hand.length}] deck=${game.deck.length} good=${game.good_players.length}`);
                break;
            }
        }
    }
    print(`\n=== END iters=${iter} ===`);
    const loserId = game_done(game);
    print(`loser=${loserId} (${loserId === 'bot_0_espresso' ? 'ESPRESSO LOST' : 'random lost'})`);
    print(`final hands: ${game.players.map(p => `${p.name}:${p.hand.length}`).join(' ')}`);
    print(`elimination_order: ${JSON.stringify(game.elimination_order)}`);
}

(async () => {
    print('Searching for espresso loss in 1v1 vs coffee...');
    let found = -1;
    for (let s = 1; s <= 5000; s++) {
        const r = await runOne(s);
        if (r.heroLost) { found = s; break; }
    }
    if (found === -1) { print('No loss found!'); return; }
    print(`Found loss at seed=${found}. Tracing...`);
    await trace(found);
})();
