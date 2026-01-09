import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY } from '../types.ts';
import { calculateLegalMoves } from '../bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../pure_bot_actions.ts';
import { start_game, game_done, seededRandom } from '../common_utils.ts';

const game: Game = {
    players: [],
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
};

game.players.push({
    player_id: 'bot_handwritten',
    name: 'Handwritten Bot',
    status: PLAYER_STATUS.READY,
    is_ai: true,
    hand: [],
    awaiting_attack: false,
    hand_length: 0,
    strategy_key: STRATEGY_KEY.HANDWRITTEN
});

game.players.push({
    player_id: 'bot_random',
    name: 'Random Bot',
    status: PLAYER_STATUS.READY,
    is_ai: true,
    hand: [],
    awaiting_attack: false,
    hand_length: 0,
    strategy_key: STRATEGY_KEY.RANDOM
});

game.players.push({
    player_id: 'bot_gpt',
    name: 'GPT Bot',
    status: PLAYER_STATUS.READY,
    is_ai: true,
    hand: [],
    awaiting_attack: false,
    hand_length: 0,
    strategy_key: STRATEGY_KEY.GPT
});

start_game(game);

(async () => {
    console.log('\n🎴 Durak Bot Battle! 🎴');
    console.log('🤖 Handwritten Bot vs 🎲 Random Bot vs 🧠 GPT Bot\n');
    
    while (game_done(game) === null) {
        // Find all bots that can currently move
        const eligibleBots: { bot: PrivatePlayer; index: number }[] = [];
        for (let index = 0; index < game.players.length; index++) {
            const player = game.players[index];

            // Check if this bot should act based on current game state
            const shouldAct = shouldBotActCore(game, player, index);
            if (shouldAct) {
                // Double-check that they have legal moves
                const legalMoves = calculateLegalMoves(game, player.player_id);
                if (legalMoves.length > 0) {
                    eligibleBots.push({ bot: player, index });
                }
            }
        }

        // If we have eligible bots, try them until one succeeds
        if (eligibleBots.length === 0) {
            console.log(`No eligible players found for game, ending processing cycle`);
            continue;
        }

        // Shuffle the eligible bots to try them in random order (seeded for determinism)
        const shuffledBots = [...eligibleBots].sort(() => seededRandom() - 0.5);

        for (const selectedBot of shuffledBots) {
            // Try to process this player's action
            const botActionEvents = await processBotAction(game, selectedBot.bot);

            if (botActionEvents) {
                break; // Exit the loop since we successfully processed a bot
            }
        }
    }
    
    // Game over - find the loser (durak)
    const loser = game_done(game);
    const loserPlayer = game.players.find(p => p.player_id === loser);
    console.log('\n' + '🏆'.repeat(40));
    console.log(`🃏 GAME OVER! The DURAK (fool) is: ${loserPlayer?.name || loser} 🃏`);
    console.log('🏆'.repeat(40) + '\n');
})();