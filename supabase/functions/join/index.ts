import { verify_game_id, loadCompleteGame, saveCompleteGame, personalize_game, wrap400, broadcastToGameUsers } from "../_shared/utils.ts";
import { GAME_STATUS, PLAYER_STATUS, Game, SERVER_EVENT_TYPE, Player } from "../_shared/types.ts";
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

    // We have to load the game no matter what
    const game: Game = await loadCompleteGame(game_id);

    // Verify game exists
    await verify_game_id(game_id);

    // Check if player is already in game
    const { data: player_games, error: player_gamesError } = await supabaseClient
        .from('player_games')
        .select('*')
        .eq('game_id', game_id)
        .eq('player_id', user_id)
        .single();

    if (!player_gamesError && player_games) {
        // because of how we filter, if this is not here, then the player is not in the game
        // going tto assume that player_games is also set correctly
        // quiet return, don't worry about it
        return new Response(JSON.stringify({
            game: personalize_game(game, user_id)
        }), {
            headers: {
                ...corsHeaders,
                'Content-Type': 'application/json'
            }
        });
    }


    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game_id} is not waiting for players, wait for next game`);
    }

    // maybe move this to start function
    const publicPlayer: Player = {
        name: user_name,
        id: user_id,
        status: PLAYER_STATUS.IDLE,
        hand: [],
    }

    // Add new player to game
    game.players.push(publicPlayer);

    // Add player-game relationship FIRST
    await supabaseClient.from('player_games').insert({
        player_id: user_id,
        game_id: game_id
    });
    
    // Then save to database - this handles all separated tables
    await saveCompleteGame(game);

    // Return response
    const response = new Response(JSON.stringify({
        game: personalize_game(game, user_id)
    }), {
        headers: {
            ...corsHeaders,
            'Content-Type': 'application/json'
        }
    });

    // Send broadcast notification
    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.PLAYER_JOINED_GAME,
        message: `Player ${user_name} joined game ${game_id}`
    });

    return response;
}));

