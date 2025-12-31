import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, AnimationEvent } from '../types.ts';
import { calculateLegalMoves } from '../bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../pure_bot_actions.ts';
import { start_game, game_done } from '../common_utils.ts';

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
    player_id: 'bot_1',
    name: 'Bot 1',
    status: PLAYER_STATUS.READY,
    is_ai: true,
    hand: [],
    awaiting_attack: false,
    done_attacking_this_round: false,
    hand_length: 0,
    strategy_key: 'random'
});

game.players.push({
    player_id: 'bot_2',
    name: 'Bot 2',
    status: PLAYER_STATUS.READY,
    is_ai: true,
    hand: [],
    awaiting_attack: false,
    done_attacking_this_round: false,
    hand_length: 0,
    strategy_key: 'random'
});

start_game(game);

(async () => {
    while (game_done(game) === null) {
        console.log(game.table_battles);
        let botProcessed = false;

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
            console.log(`No eligible bots found for game, ending bot processing cycle`);
            continue;
        }

        console.log(`Found ${eligibleBots.length} eligible bots: ${eligibleBots.map(b => b.bot.name).join(', ')}`);

        // Shuffle the eligible bots to try them in random order
        const shuffledBots = [...eligibleBots].sort(() => Math.random() - 0.5);

        for (const selectedBot of shuffledBots) {

            // Try to process this bot's action
            const botActionEvents = await processBotAction(game, selectedBot.bot);

            if (botActionEvents) {
                botProcessed = true;
                break; // Exit the loop since we successfully processed a bot
            }
        }

    }
})();