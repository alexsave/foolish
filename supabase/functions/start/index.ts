import { verify_game_id, loadCompleteGame, saveCompleteGame, wrap400, broadcastToGameUsers, verify_player_in_game, start_game, personalize_game } from "../_shared/utils.ts";
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

    // Load complete game state from separated tables
    let game = await loadCompleteGame(game_id);

    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game_id} is not waiting for players, wait for next game`);
    }
    // if player_games is set correctly, then the players shoudl be set correclty in the game too. But not vice versa

    // let the error throw

    let message: string = `Player ${user_name} is ready`;
    let type: ServerEventType = SERVER_EVENT_TYPE.PLAYER_READY;

    // Update player status to ready
    game.players.find(player => player.id === user_id)!.status = PLAYER_STATUS.READY;

    if (game.players.length >= 2 && game.players.every(player => player.status === PLAYER_STATUS.READY)) {
        // We can start the game 
        game = await start_game(game);

        message = `Player ${user_name} is ready, starting game ${game_id}`;
        type = SERVER_EVENT_TYPE.GAME_STARTED;
        await saveCompleteGame(game);

    } else {
        // Just update player status without starting
        // We don't need to save EVERYTHING, just the public game
        // TODO save less
        await saveCompleteGame(game);
    }

    broadcastToGameUsers(game, 'game_update', {
        type: type,
        message: message,
        player_id: user_id
    });

    // Return personalized game state
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
