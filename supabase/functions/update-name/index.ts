import { loadCompleteGame, saveCompleteGame, personalize_game, wrap400, broadcastToGameUsers } from "../_shared/utils.ts";
import { GAME_STATUS, SERVER_EVENT_TYPE, Game } from "../_shared/types.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { createClient } from "npm:@supabase/supabase-js@2.39.0"

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

serve(wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id, name } = body;

    if (!name || name.trim() === '') {
        throw new Error('Game name cannot be empty');
    }

    // Load the complete game
    const game: Game = await loadCompleteGame(game_id);

    // Check if the game is in waiting status (only allow name changes in lobby)
    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error('Game name can only be changed in the lobby');
    }

    // Check if the user is in the game (only players can change the name)
    const playerExists = game.players.some(player => player.id === user_id);
    if (!playerExists) {
        throw new Error('Only players in the game can change the name');
    }

    // Update the game name
    game.name = name.trim();

    // Save the updated game
    await saveCompleteGame(game);

    // Send broadcast notification
    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.GAME_NAME_UPDATED,
        message: `Game name updated to "${name}" by ${user_name}`
    });

    return {
        game: personalize_game(game, user_id)
    };
})); 