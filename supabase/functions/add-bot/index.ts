import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { ANIMATION_EVENT_TYPE, PLAYER_STATUS, AnimationEvent, GAME_STATUS, STRATEGY_KEY } from "../_shared/types.ts";
import { start_game } from "../_shared/common_utils.ts";
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// Only this user can add GPT bots (to control API costs)
const GPT_ALLOWED_USER_ID = '60a5c562-0922-40a6-b416-77e3285d87b2';

wrap400(async ({body, game, user}: ExecutionParams) => {
    const { game_id } = body;

    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game_id} is not waiting for players`);
    }

    // Fetch all bots
    const { data: allBots, error } = await supabaseClient
        .from('bots')
        .select('*');

    if (error || !allBots) {
        throw new Error(`Failed to fetch bots`);
    }

    // Get IDs of bots already in the game
    const existingBotIds = game.players
        .filter(p => p.is_ai)
        .map(p => p.player_id);

    // Filter out bots already in the game
    // Also filter out GPT bots unless the user is allowed to add them
    const availableBots = allBots.filter(bot => {
        if (existingBotIds.includes(bot.id)) return false;
        if (bot.strategy_key === STRATEGY_KEY.GPT && user.id !== GPT_ALLOWED_USER_ID) return false;
        return true;
    });

    if (availableBots.length === 0) {
        throw new Error(`No available bots to add to the game`);
    }

    // Choose a random bot from available ones
    const randomIndex = Math.floor(Math.random() * availableBots.length);
    const availableBot = availableBots[randomIndex];

    // Add bot to game
    game.players.push({
        player_id: availableBot.id,
        name: availableBot.nickname,
        status: PLAYER_STATUS.READY,
        is_ai: true,
        hand: [],
        awaiting_attack: false,
        hand_length: 0,
        strategy_key: availableBot.strategy_key
    });

    // Check if ALL players are ready AND we have at least 2 players
    const allPlayersReady = game.players.every(p => p.status === PLAYER_STATUS.READY) && game.players.length >= 2;

    let start_events: AnimationEvent[] = [];
    if (allPlayersReady) {
        // All players are ready - start the game!
        start_events = start_game(game);
    }

    // Add animation event to notify players
    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: allPlayersReady ? `All players ready - starting game!` : `Bot ${availableBot.nickname} joined the game`,
        game_state: game
    }, ...start_events] };

    // Note: saveCompleteGame (called by executeWithGameLock) will handle:
    // - Updating games table with new players array
    // - Upserting bot_hands for the new bot
    // No additional DB operations needed here!

}, false); 