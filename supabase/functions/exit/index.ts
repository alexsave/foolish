import { ExecutionParams, wrap400, animationEvents } from "../_shared/utils.ts";
import { GAME_STATUS } from "../_shared/types.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async ({user, body, game}: ExecutionParams) => {
    const user_id = user.id;
    const { bot_id } = body;

    let exitedPlayerName = '';

    if (bot_id) {
        // Removing a bot
        // Verify bot is in game
        const botPlayer = game.players.find(player => player.player_id === bot_id && player.is_ai);
        if (!botPlayer) {
            throw new Error(`Bot ${bot_id} is not in the game`);
        }

        exitedPlayerName = botPlayer.name;

        // Remove bot from game
        game.players = game.players.filter(player => player.player_id !== bot_id);

        // Remove bot's hand from database
        await supabaseClient
            .from('bot_hands')
            .delete()
            .eq('game_id', game.id)
            .eq('bot_id', bot_id);
    } else {
        // Removing the user
        // Verify player is in game
        verify_player_in_game(game, user_id);

        const userPlayer = game.players.find(player => player.player_id === user_id);
        if (userPlayer) {
            exitedPlayerName = userPlayer.name;
        }

        // Remove player from game
        game.players = game.players.filter(player => player.player_id !== user_id);

        // Remove player's hand from database
        await supabaseClient
            .from('player_hands')
            .delete()
            .eq('game_id', game.id)
            .eq('player_id', user_id);

        // If game is in progress and player leaves, they should be marked as out
        if (game.status === GAME_STATUS.PLAYING) {
            // Player is already removed from game.players above
            // Add them to elimination order if not already there
            if (!game.elimination_order.includes(user_id)) {
                game.elimination_order.push(user_id);
            }
        }
    }

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
        .eq('id', game.id);

    // If no players left, clean up the game
    if (game.players.length === 0) {
        await supabaseClient
            .from('games')
            .delete()
            .eq('id', game.id);
        
        await supabaseClient
            .from('game_decks')
            .delete()
            .eq('game_id', game.id);
        
        // No animation event needed if game is being deleted
        return { game, events: [] };
    }

    // Add animation event to notify remaining players
    const playerType = bot_id ? 'Bot' : 'Player';
    animationEvents.addMagicTransitionEvent(`${playerType} ${exitedPlayerName} left the game`, game);

    const events = animationEvents.getEvents();
    animationEvents.clear();

    return { game, events };

}, false); 
