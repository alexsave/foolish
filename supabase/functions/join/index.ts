import { verify_game_id, lobbify_game, wrap400, emailToName, broadcastToGame } from "../_shared/utils.ts";
import { GAME_STATUS, PLAYER_STATUS, Game, SERVER_EVENT_TYPE } from "../_shared/types.ts";

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
    const user_name = emailToName(user.email);
    const { game_id } = await req.json();

    // This is defeinitely handled by the .single. If there is no game, it will throw an error
    await verify_game_id(game_id);

    const { data: game, error: gameError } = await supabaseClient.from('games').select('*').eq('id', game_id).single();
    if (gameError) {
        console.error('Error loading game', gameError);
        return new Response('Error loading game', { status: 500 });
    }

    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game_id} is not waiting for players, wait for next game`);
    }
    // if player_games is set correctly, then the players shoudl be set correclty in the game too. But not vice versa

    const { data: player_games, error: player_gamesError } = await supabaseClient.from('player_games').select('*').eq('game_id', game_id).eq('player_id', user_id).single();

    if (!player_gamesError && player_games) {
        // because of how we filter, if this is not here, then the player is not in the game
        // going tto assume that player_games is also set correctly
        // quiet return, don't worry about it
        return new Response(JSON.stringify({
            game: lobbify_game(game)
        }), {
            headers: {
                ...corsHeaders,
                'Content-Type': 'application/json'
            }
        });
    }

    // Create the game object for database insert (snake_case)
    const dbGameData: Game = {
        ...game,
        players: [...game.players, {
            name: user_name,
            id: user_id,
            status: PLAYER_STATUS.IDLE,
            hand: []
        }]
    };

    // CRITICAL: Insert into player_games BEFORE returning response
    // This ensures the user has permission to subscribe to the game channel immediately
    // Do the database operations BEFORE returning to avoid race condition
    // SEQUENTIAL: games table first, then player_games (foreign key constraint)
    await supabaseClient.from('games').update({ players: dbGameData.players }).eq('id', game_id);
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
    broadcastToGame(game_id, {
        type: SERVER_EVENT_TYPE.PLAYER_JOINED_GAME,
        message: `Player ${user_name} joined game ${game_id}`,
        game_id: game_id,
        game: lobbify_game(dbGameData)
    }).catch(error => {
        console.error('Error broadcasting game join:', error);
    });

    return response;
}));

