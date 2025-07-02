import { wrap400, personalize_game, loadCompleteGame, verify_game_id, verify_player_in_game } from '../_shared/utils.ts';
import { Game } from '../_shared/types.ts'; 

import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { getAuthenticatedUser } from "../_shared/auth.ts";
import { User } from "npm:@supabase/supabase-js@2.39.0"
import { handleCors, corsHeaders } from "../_shared/cors.ts";

// TODO: just remove this. With the right policies we can query from client
serve(wrap400(async (req) => {
    const corsResponse = handleCors(req);
    if (corsResponse) return corsResponse;

    const user: User = await getAuthenticatedUser(req);
    const user_id = user.id;
    const { game_id } = await req.json();

    // Verify game exists and player is in game. 
    // TODO do this after loading the game since we load it anyways
    await verify_game_id(game_id); 
    await verify_player_in_game(game_id, user_id);

    // Load complete game state from separated tables
    const game: Game = await loadCompleteGame(game_id);

    const response = new Response(JSON.stringify({
        game: personalize_game(game, user_id)
    }), {
        headers: {
            ...corsHeaders,
            'Content-Type': 'application/json'
        }
    });

    return response;
}));
