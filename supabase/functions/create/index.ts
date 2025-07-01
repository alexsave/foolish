import { createId, lobbify_game, wrap400, broadcastToGameUsers } from "../_shared/utils.ts";
import { GAME_STATUS, PLAYER_STATUS, Game } from "../_shared/types.ts";
import { emailToName } from "../_shared/common_utils.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { getAuthenticatedUser } from "../_shared/auth.ts";
import { createClient, User } from "npm:@supabase/supabase-js@2.39.0"
import { handleCors, corsHeaders } from "../_shared/cors.ts";

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

serve(wrap400(async (req) => {
    const corsResponse = handleCors(req);
    if (corsResponse) return corsResponse;

    const user: User = await getAuthenticatedUser(req);
    const user_id = user.id;
    const game_id = createId();
    const user_name = emailToName(user.email);

    // Create the game object for database insert (snake_case)
    const dbGameData: Game = {
        id: game_id,
        deck: [],
        flipped: null,
        players: [{
            name: user_name,
            id: user_id,
            status: PLAYER_STATUS.IDLE,
            hand: []
        }],
        status: GAME_STATUS.WAITING,
        power_suit: 0,
        first_attacker: 0,
        currently_attacked: 0,
        table_battles: []
    };

    // CRITICAL: Insert into player_games BEFORE returning response
    // This ensures the user has permission to subscribe to the game channel immediately
    // Do ALL database operations BEFORE returning to avoid race condition
    // SEQUENTIAL: games table first, then player_games (foreign key constraint)
    await supabaseClient.from('games').insert(dbGameData);
    await supabaseClient.from('player_games').insert({
        player_id: user_id,
        game_id: game_id
    });

    // Now it's safe to return - user has access to game channel
    const response = new Response(JSON.stringify({
        game: lobbify_game(dbGameData)
    }), {
        headers: {
            ...corsHeaders,
            'Content-Type': 'application/json'
        }
    });

    // Send broadcast notification asynchronously (non-blocking)
    broadcastToGameUsers(dbGameData, 'game_update', {
        type: 'game_created',
        message: `Game created with id ${game_id}`
    });

    return response;
}));
