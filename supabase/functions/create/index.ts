import { createId, lobbify_game, wrap400, emailToName } from "../_shared/utils.ts";
import { GAME_STATUS, PLAYER_STATUS } from "../_shared/types.ts";

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
    console.log('Received request:', {
        method: req.method,
        url: req.url,
        headers: Object.fromEntries(req.headers.entries())
    });

    const corsResponse = handleCors(req);
    if (corsResponse) return corsResponse;

    console.log('Authenticating user');
    const user: User = await getAuthenticatedUser(req);
    console.log('Successfully authenticated user' + JSON.stringify(user));
    const user_id = user.id;

    // if we want to use types we need to copy them to _shared


    // Create a new game
    const { data, error } = await supabaseClient.from('games').insert({
        id: createId(),
        deck: [],
        flipped: null,
        players: [{
            name: emailToName(user.email),
            id: user.id,
            status: PLAYER_STATUS.IDLE,
            hand: []
        }],
        status: GAME_STATUS.WAITING,
        power_suit: 0,
        first_attacker: 0,
        currently_attacked: 0,
        previous_first_attacker: 0,
        previous_currently_attacked: 0,
        table_battles: []
    }).select().single();

    if (error) {
        console.error('Error creating game:', error);
        throw error;
    }

    console.log('Successfully created game' + JSON.stringify(data));

    const game_id = data.id;

    // we have user id and game id. put them into the player_games table
    const { data: player_game_data, error: player_game_error } = await supabaseClient.from('player_games').insert({
        player_id: user_id,
        game_id: game_id
    });
    console.log('Successfully added player to game' + JSON.stringify(player_game_data));


    // Add it to the public game channel. Anyone subscribing to the public game channel will get this message.
    // Consider lobbifying this message
    const { data: public_game_channel_data, error: public_game_channel_error } = await supabaseClient.from('public_game_channel').insert({
        game_id: game_id,
        message: {
            type: 'game_created',
            message: `Game created with id ${game_id}`,
            game_id: game_id
        }
    });

    return new Response(JSON.stringify({
        game_id: game_id,
        game: lobbify_game(data)
    }), {
        headers: {
            ...corsHeaders,
            'Content-Type': 'application/json'
        }
    });
}));
