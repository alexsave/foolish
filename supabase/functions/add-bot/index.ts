import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { BotHand, PLAYER_STATUS } from "../_shared/types.ts";
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async ({body, game}: ExecutionParams) => {
    const { game_id } = body;

    if (game.status !== 'waiting') {
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
    const availableBots = allBots.filter(bot => !existingBotIds.includes(bot.id));

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
        done_attacking_this_round: false,
        hand_length: 0
    });

    // Update game in database
    await supabaseClient
        .from('games')
        .update({ 
            players: game.players.map(p => ({
                player_id: p.player_id,
                name: p.name,
                status: p.status,
                is_ai: p.is_ai,
                hand_length: p.hand_length
            }))
        })
        .eq('id', game_id);

    // Initialize empty hand for bot
    await supabaseClient.from('bot_hands').insert({
        game_id: game_id,
        bot_id: availableBot.id,
        hand: [],
        awaiting_attack: false,
        done_attacking_this_round: false
    } as BotHand);

    return { game, events: [] };

}, false); 