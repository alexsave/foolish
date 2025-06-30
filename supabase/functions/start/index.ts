import { verify_game_id, wrap400, broadcastToGame, verify_player_in_game, start_game, personalize_game } from "../_shared/utils.ts";
import { GAME_STATUS, PLAYER_STATUS, SERVER_EVENT_TYPE, ServerEventType } from "../_shared/types.ts";
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
    const user_name = emailToName(user.email);
    const { game_id } = await req.json();

    // This is defeinitely handled by the .single. If there is no game, it will throw an error
    await verify_game_id(game_id);
    await verify_player_in_game(game_id, user_id);

    let { data: game, error: gameError } = await supabaseClient.from('games').select('*').eq('id', game_id).single();
    if (gameError) {
        console.error('Error loading game', gameError);
        return new Response('Error loading game', { status: 500 });
    }

    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game_id} is not waiting for players, wait for next game`);
    }
    // if player_games is set correctly, then the players shoudl be set correclty in the game too. But not vice versa

    const { data: player_games, error: player_gamesError } = await supabaseClient.from('player_games').select('*').eq('game_id', game_id).eq('player_id', user_id).single();
    // let the error throw

    let message: string = `Player ${user_name} is ready`;
    let type: ServerEventType = SERVER_EVENT_TYPE.PLAYER_READY;

    game.players.find(player => player.id === user_id)!.status = PLAYER_STATUS.READY;

    if (game.players.length >= 2 && game.players.every(player => player.status === PLAYER_STATUS.READY)) {
        // We can start the game
        game = start_game(game);

        // update the entire thing
        await supabaseClient.from('games').update(game).eq('id', game_id);

        message = `Player ${user_name} is ready, starting game ${game_id}`;
        type = SERVER_EVENT_TYPE.GAME_STARTED;

    } else {
        // Return without starting the game

        // only update game.players
        await supabaseClient.from('games').update({ players: game.players }).eq('id', game_id);
    }

    broadcastToGame(game_id, {
        type: type,
        message: message,
        game_id: game_id,
        game: personalize_game(game, user_id),
        player_id: user_id
    }).catch(e => { 
        console.error('Error broadcasting game join:', e);
    });


    // Now it's safe to return - user has access to game channel
    const response = new Response(JSON.stringify({
        game: personalize_game(game, user_id)
    }), {
        headers: {
            ...corsHeaders,
            'Content-Type': 'application/json'
        }
    });

    // Send broadcast notification asynchronously (non-blocking)

    return response;
}));
