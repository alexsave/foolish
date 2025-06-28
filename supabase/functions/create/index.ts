import { createId, lobbify_game, wrap400, emailToName } from "../_shared/utils.ts";
import { GAME_STATUS, PLAYER_STATUS, Game } from "../_shared/types.ts";

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
        previous_first_attacker: 0,
        previous_currently_attacked: 0,
        table: []
    };

    // Return immediately to the user - maximum speed!
    const response = new Response(JSON.stringify({
        game: lobbify_game(dbGameData)
    }), {
        headers: {
            ...corsHeaders,
            'Content-Type': 'application/json'
        }
    });

    // Now handle all database operations asynchronously AFTER returning to user
    supabaseClient.from('games').insert(dbGameData)
        .then(({ error: gameError }) => {
            if (gameError) {
                console.error('Error creating game:', gameError);
                return; // Don't proceed with other operations if game creation failed
            }
            
            // Game created successfully, now do the other operations in parallel
            return Promise.allSettled([
                supabaseClient.from('player_games').insert({
                    player_id: user_id,
                    game_id: game_id
                }),
                supabaseClient.from('public_game_channel').insert({
                    game_id: game_id,
                    message: {
                        type: 'game_created',
                        message: `Game created with id ${game_id}`,
                        game_id: game_id
                    }
                })
            ]);
        })
        .then((results) => {
            if (results) {
                const [playerGameResult, publicChannelResult] = results;
                // Log errors if any, but user already has their response
                if (playerGameResult.status === 'rejected' || publicChannelResult.status === 'rejected') {
                    console.error('Error in background operations:', {
                        playerGame: playerGameResult.status === 'rejected' ? playerGameResult.reason : null,
                        publicChannel: publicChannelResult.status === 'rejected' ? publicChannelResult.reason : null
                    });
                }
            }
        })
        .catch((error) => {
            console.error('Error in async game creation flow:', error);
        });

    return response;
}));
