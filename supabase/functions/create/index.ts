import { createId, wrap400, broadcastToGameUsers, loadCompleteGame, personalize_game } from "../_shared/utils.ts";
import { GAME_STATUS, PLAYER_STATUS, Game, PublicGame } from "../_shared/types.ts";
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

    // Create the game object for database insert (separated schema)

    // Insert public game data (without deck/hands)
    const publicGameData: PublicGame = {
        id: game_id,
        name: `${user_name}'s Game`,
        deck_length: 0,
        flipped: null,
        players: [{
            name: user_name,
            id: user_id,
            status: PLAYER_STATUS.IDLE,
            hand_length: 0
        }],
        status: GAME_STATUS.WAITING,
        power_suit: 0,
        first_attacker: 0,
        currently_attacked: 0,
        table_battles: []
    };

    // CRITICAL: Insert into databases in correct order
    // 1. Games table (public data only)
    await supabaseClient.from('games').insert(publicGameData);
    
    // 2. Player-games relationship
    await supabaseClient.from('player_games').insert({
        player_id: user_id,
        game_id: game_id
    });

    // 3. Initialize empty deck (will be filled when game starts)
    await supabaseClient.from('game_decks').insert({
        game_id: game_id,
        deck: []
    });

    // 4. Initialize empty hand for creator
    await supabaseClient.from('player_hands').insert({
        game_id: game_id,
        player_id: user_id,
        hand: []
    });

    // Load complete game state from separated tables
    // We could just make this without loading, but I want to see what it looks like first
    // TODO: see if we can speed this up
    const dbGameData: Game = await loadCompleteGame(game_id);

    // Now it's safe to return - user has access to game channel
    const response = new Response(JSON.stringify({
        game: personalize_game(dbGameData, user_id)
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
