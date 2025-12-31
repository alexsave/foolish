import { ExecutionParams, wrap400 } from "../_shared/utils.ts";
import { ANIMATION_EVENT_TYPE, GAME_STATUS } from "../_shared/types.ts";
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

        // Remove bot's hand from database (must happen before saveCompleteGame tries to upsert)
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

        // Remove player's hand from database (must happen before saveCompleteGame tries to upsert)
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

    // Note: saveCompleteGame (called by executeWithGameLock) will update the games table
    // with the modified players array. No manual update needed here!

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
    return { game, events: [{
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `${playerType} ${exitedPlayerName} left the game`,
        game_state: game
    }] };

}, false); 
