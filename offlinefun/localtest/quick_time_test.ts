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
    player_id: `bot_${index}_${strategy}`, name: `${strategy}${index}`, status: PLAYER_STATUS.READY,
    is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: strategy
});

const createGame = (heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number): Game => {
    const players: PrivatePlayer[] = [createPlayer(heroStrat, 0)];
    for (let i = 0; i < numOpps; i++) players.push(createPlayer(oppStrat, i + 1));
    return { players, deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
        deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
        first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
        good_timestamp: null, good_players: [] };
};

async function runOne(heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number, capIters = 1500): Promise<{ loserStrat: StrategyKey | null; iters: number }> {
    const game = createGame(heroStrat, oppStrat, numOpps);
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
            const j = Math.floor(seededRandom() * (i + 1));
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
