import { wrap400 } from "../_shared/utils.ts";
import { Game, GAME_STATUS, PLAYER_STATUS, SERVER_EVENT_TYPE } from "../_shared/types.ts";
import { verify_player_in_game } from "../_shared/common_utils.ts";
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async (user, user_name, body, game) => {
    const user_id = user.id;

    // Load complete game state from separated tables
    //let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Remove player from game
    game.players = game.players.filter(player => player.player_id !== user_id);

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

    // Remove player's hand from database
    await supabaseClient
        .from('player_hands')
        .delete()
        .eq('game_id', game.id)
        .eq('player_id', user_id);

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
    }

    // If game is in progress and player leaves, they should be marked as out
    if (game.status === GAME_STATUS.PLAYING) {
        // Player is already removed from game.players above
        // Add them to elimination order if not already there
        if (!game.elimination_order.includes(user_id)) {
            game.elimination_order.push(user_id);
        }
    }

    // Save complete game state back to separated tables
    //await saveCompleteGame(game);

    return { game, events: [] };

}, false); 
