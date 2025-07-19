import { wrap400 } from "../_shared/utils.ts";
import { Game, BotHand } from "../_shared/types.ts";
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async (user, user_name, body, game) => {
    const user_id = user.id;
    const { game_id, bot_strategy } = body;

    if (game.status !== 'waiting') {
        throw new Error(`Game ${game_id} is not waiting for players`);
    }

    // Find available bot by strategy
    const { data: availableBot, error } = await supabaseClient
        .from('bots')
        .select('*')
        .eq('strategy_key', bot_strategy)
        .limit(1)
        .single();

    if (error || !availableBot) {
        throw new Error(`No bot available with strategy ${bot_strategy}`);
    }

    // Add bot to game
    game.players.push({
        player_id: availableBot.id,
        name: availableBot.nickname,
        status: 'idle',
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