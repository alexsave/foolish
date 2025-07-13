import { wrap400, loadCompleteGame, saveCompleteGame, broadcastToGameUsers } from "../_shared/utils.ts";
import { verify_player_in_game, personalize_game, refill_deck } from "../_shared/common_utils.ts";
import { GAME_STATUS, PLAYER_STATUS, SERVER_EVENT_TYPE } from "../_shared/types.ts";
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id } = body;

    // Load complete game state using JOINs
    let game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    // Verify game is in GAME_OVER state
    if (game.status !== GAME_STATUS.GAME_OVER) {
        throw new Error(`Game ${game_id} is not in game_over state`);
    }

    // Reset game state to waiting
    game.status = GAME_STATUS.WAITING;
    
    // Reset all players to idle and clear their hands
    game.players.forEach((player) => {
        // Set bots to ready status, human players to idle
        player.status = player.is_ai ? PLAYER_STATUS.READY : PLAYER_STATUS.IDLE;
        player.hand = [];
        player.awaiting_attack = false;
        player.done_attacking_this_round = false;
    });

    // Clear game state
    game.table_battles = [];
    game.deck = refill_deck(game.players.length);
    game.elimination_order = []; // Clear elimination order
    game.discard_pile_length = 0; // Reset discard pile length
    game.flipped = null;
    game.first_attacker = 0;
    game.defender = 0;

    // Clear chat messages for the game (fire and forget)
    supabaseClient
        .from('chat_messages')
        .delete()
        .eq('game_id', game.id);

    // Save complete game state back to separated tables
    await saveCompleteGame(game);

    // Broadcast the game reset to all players
    await broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.GAME_STARTED,
        message: `Game has been reset and is ready for new players`
    });

    return {
        game: personalize_game(game, user_id)
    };
}); 