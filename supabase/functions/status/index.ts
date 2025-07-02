import { wrap400, personalize_game, loadCompleteGame, verify_player_in_game } from '../_shared/utils.ts';
import { Game } from '../_shared/types.ts'; 

import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"

// TODO: just remove this. With the right policies we can query from client
serve(wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id } = body;

    // Load complete game state from separated tables
    const game: Game = await loadCompleteGame(game_id);

    // Verify player is in game
    verify_player_in_game(game, user_id);

    return {
        game: personalize_game(game, user_id)
    };
}));
