import { calculateLegalMoves, registerBotStrategy } from '../../server/api/common/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../../server/api/common/pure_bot_actions.ts';
import { game_done, seededRandom } from '../../server/api/common/common_utils.ts';
import { start_game } from '../../server/api/common/game_lifecycle.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, StrategyKey } from '../../server/api/core/types.ts';
import { EspressoStrategy } from './frozen/espresso_strategy.ts';

// Register custom strategies
const ESPRESSO = 'espresso' as StrategyKey;
registerBotStrategy(ESPRESSO, new EspressoStrategy());

// Configuration
const NUM_GAMES = 30000;
const STRATEGY_1: StrategyKey = STRATEGY_KEY.HANDWRITTEN;
const STRATEGY_2: StrategyKey = ESPRESSO;

// Silence all console output during batch run
const noop = () => { };
const saved = { log: console.log, warn: console.warn, error: console.error, info: console.info };
console.log = noop;
console.warn = noop;
console.error = noop;
console.info = noop;
const print = saved.log.bind(console);

const createPlayer = (strategy: StrategyKey, index: number): PrivatePlayer => ({
    player_id: `bot_${index}_${strategy}`,
    name: `${strategy} Bot ${index}`,
    status: PLAYER_STATUS.READY,
    is_ai: true,
    hand: [],
    awaiting_attack: false,
    hand_length: 0,
    strategy_key: strategy
});

const createFreshGame = (strat1: StrategyKey, strat2: StrategyKey): Game => ({
    players: [createPlayer(strat1, 1), createPlayer(strat2, 2)],
    deck: [],
    logs: [],
    id: 'game_1',
    name: 'Game 1',
    status: GAME_STATUS.PLAYING,
    deck_length: 0,
    discard_pile_length: 0,
    flipped: null,
    power_suit: 0,
    first_attacker: 0,
    defender: 0,
    table_battles: [],
    elimination_order: [],
    good_timestamp: null,
    good_players: [],
});

async function runSingleGame(strat1: StrategyKey, strat2: StrategyKey): Promise<StrategyKey | null> {
    const game = createFreshGame(strat1, strat2);
    start_game(game);

    while (game_done(game) === null) {
        const eligibleBots: { bot: PrivatePlayer; index: number }[] = [];
        for (let index = 0; index < game.players.length; index++) {
            const player = game.players[index];
            const shouldAct = shouldBotActCore(game, player, index);
            if (shouldAct) {
                const legalMoves = calculateLegalMoves(game, player.player_id);
                if (legalMoves.length > 0) {
                    eligibleBots.push({ bot: player, index });
                }
            }
        }

        // Fisher-Yates: comparator-based shuffles can livelock V8 TimSort.
        const shuffledBots = [...eligibleBots];
        for (let i = shuffledBots.length - 1; i > 0; i--) {
            const j = Math.floor(seededRandom() * (i + 1));
            [shuffledBots[i], shuffledBots[j]] = [shuffledBots[j], shuffledBots[i]];
        }
        for (const selectedBot of shuffledBots) {
            const botActionEvents = await processBotAction(game, selectedBot.bot);
            if (botActionEvents) break;
        }
    }

    // Return the strategy of the loser
    const loserId = game_done(game);
    const loser = game.players.find(p => p.player_id === loserId);
    return loser?.strategy_key as StrategyKey || null;
}

(async () => {
    print(`\n🎴 Running ${NUM_GAMES} games: ${STRATEGY_1} vs ${STRATEGY_2} 🎴\n`);

    const losses: Record<string, number> = {};
    losses[STRATEGY_1] = 0;
    losses[STRATEGY_2] = 0;
    
    const startTime = Date.now();

    for (let i = 0; i < NUM_GAMES; i++) {
        const loserStrategy = await runSingleGame(STRATEGY_1, STRATEGY_2);
        if (loserStrategy) {
            losses[loserStrategy]++;
        }
        
        if ((i + 1) % 1000 === 0) {
            print(`Progress: ${i + 1}/${NUM_GAMES}`);
        }
    }

    const totalTime = ((Date.now() - startTime) / 1000).toFixed(2);
    const wins1 = NUM_GAMES - losses[STRATEGY_1];
    const wins2 = NUM_GAMES - losses[STRATEGY_2];

    print('\n' + '='.repeat(60));
    print('📊 RESULTS');
    print('='.repeat(60));
    print(`Total games: ${NUM_GAMES} in ${totalTime}s`);
    print(`${STRATEGY_1} WIN RATE: ${((wins1 / NUM_GAMES) * 100).toFixed(2)}% (${wins1} wins, ${losses[STRATEGY_1]} losses)`);
    print(`${STRATEGY_2} WIN RATE: ${((wins2 / NUM_GAMES) * 100).toFixed(2)}% (${wins2} wins, ${losses[STRATEGY_2]} losses)`);
    print('='.repeat(60) + '\n');
})();
