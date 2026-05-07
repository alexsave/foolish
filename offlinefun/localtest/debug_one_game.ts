import { calculateLegalMoves, registerBotStrategy } from '../../supabase/functions/_shared/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../../supabase/functions/_shared/pure_bot_actions.ts';
import { start_game, game_done, seededRandom } from '../../supabase/functions/_shared/common_utils.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, StrategyKey } from '../../supabase/functions/_shared/types.ts';
import { EspressoStrategy } from '../../supabase/functions/_shared/strategies/espresso_strategy.ts';

const ESPRESSO = 'espresso' as StrategyKey;
registerBotStrategy(ESPRESSO, new EspressoStrategy());

const noop = () => { };
console.log = noop; console.warn = noop; console.error = noop; console.info = noop;
const print = (...args: any[]) => process.stdout.write(args.map(a => typeof a === 'string' ? a : JSON.stringify(a)).join(' ') + '\n');

const createPlayer = (strategy: StrategyKey, index: number): PrivatePlayer => ({
    player_id: `bot_${index}_${strategy}`,
    name: `${strategy}${index}`,
    status: PLAYER_STATUS.READY,
    is_ai: true,
    hand: [],
    awaiting_attack: false,
    hand_length: 0,
    strategy_key: strategy
});

const createGame = (heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number): Game => {
    const players: PrivatePlayer[] = [createPlayer(heroStrat, 0)];
    for (let i = 0; i < numOpps; i++) players.push(createPlayer(oppStrat, i + 1));
    return {
        players, deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
        deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
        first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
        good_timestamp: null, good_players: [],
    };
};

(async () => {
    const game = createGame(ESPRESSO, STRATEGY_KEY.RANDOM, 2);
    start_game(game);

    print(`Game start: power=${game.power_suit}, first_attacker=${game.first_attacker}, defender=${game.defender}`);
    for (const p of game.players) {
        print(`  ${p.name}: hand_size=${p.hand.length}`);
    }

    let iter = 0;
    while (game_done(game) === null && iter < 2000) {
        iter++;
        const eligible: { bot: PrivatePlayer; index: number }[] = [];
        for (let i = 0; i < game.players.length; i++) {
            if (shouldBotActCore(game, game.players[i], i)) {
                const lm = calculateLegalMoves(game, game.players[i].player_id);
                if (lm.length > 0) eligible.push({ bot: game.players[i], index: i });
            }
        }
        if (eligible.length === 0) {
            print(`[iter ${iter}] NO ELIGIBLE BOTS. Defender=${game.defender} (${game.players[game.defender].name}). Table=${game.table_battles.length}, all covered=${game.table_battles.length > 0 && game.table_battles.every(b => b.defense !== null)}`);
            print(`  awaiting flags: ${game.players.map(p => `${p.name}:${p.awaiting_attack}`).join(', ')}`);
            print(`  good_players: ${game.good_players}`);
            print(`  hand sizes: ${game.players.map(p => `${p.name}:${p.hand.length}`).join(', ')}`);
            print(`  in_status: ${game.players.map(p => `${p.name}:${p.status}`).join(', ')}`);
            break;
        }
        const shuffled = [...eligible];
        for (let i = shuffled.length - 1; i > 0; i--) {
            const j = Math.floor(seededRandom() * (i + 1));
            [shuffled[i], shuffled[j]] = [shuffled[j], shuffled[i]];
        }
        let acted = false;
        for (const sb of shuffled) {
            const r = await processBotAction(game, sb.bot);
            if (r) { acted = true; break; }
        }
        if (!acted) {
            print(`[iter ${iter}] eligible=${eligible.length} but none acted`);
            for (const e of eligible) {
                const lm = calculateLegalMoves(game, e.bot.player_id);
                print(`  ${e.bot.name} legal moves: ${lm.map(m => m.type).join(', ')}`);
            }
            break;
        }
    }

    print(`\n=== END (iter=${iter}) ===`);
    print(`game_done = ${game_done(game)}`);
    print(`elimination order = ${game.elimination_order}`);
    print(`hand sizes = ${game.players.map(p => `${p.name}:${p.hand.length}`).join(', ')}`);
    print(`status = ${game.players.map(p => `${p.name}:${p.status}`).join(', ')}`);
})();
