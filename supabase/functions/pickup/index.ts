import { verify_game_id, wrap400, validate_defender_status, get_next_player_index, refill, verify_player_in_game, personalize_game, broadcastToGame } from "../_shared/utils.ts";
import { GAME_STATUS, PLAYER_STATUS, SERVER_EVENT_TYPE, Game } from "../_shared/types.ts";

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
    const { game_id } = await req.json();

    // This is defeinitely handled by the .single. If there is no game, it will throw an error
    await verify_game_id(game_id);
    await verify_player_in_game(game_id, user_id);

    let { data: game, error: gameError } = await supabaseClient.from('games').select('*').eq('id', game_id).single();
    if (gameError) {
        console.error('Error loading game', gameError);
        return new Response('Error loading game', { status: 500 });
    }

    game = handle_pickup(game, game_id, user_id);

    await supabaseClient.from('games').update(game).eq('id', game_id);

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



const handle_pickup = (game: Game, game_id: string, player_id: string): Game => {
    if (game.status !== GAME_STATUS.FREE_PLAY && game.status !== GAME_STATUS.ONLY_DEFEND) {
        throw new Error(`Game ${game_id} is not in free_play or only_defend mode`);
    }

    // check if player is the defender
    validate_defender_status(game, player_id, true);
    // TODO add a timer + check to make sure they don't pick up too quickly

    // check if there are cards on the table
    if (game.table_battles.length === 0) {
        throw new Error(`No cards on the table`);
    }

    // ok let's just pick it up

    // add cards from table to hand
    game.table_battles.forEach(battle => {
        game.players[game.currently_attacked].hand.push(battle.attack);
        if (battle.defense) {
            game.players[game.currently_attacked].hand.push(battle.defense);
        }
    });

    // clear table
    game.table_battles = [];

    // Draw cards starting from first attacker

    refill(game);

    // shift
    game.first_attacker = get_next_player_index(game, game.currently_attacked);
    game.currently_attacked = get_next_player_index(game, game.first_attacker);
    game.status = GAME_STATUS.FIRST_ATTACKER;

    broadcastToGame(game_id, {
        type: SERVER_EVENT_TYPE.PICKUP_PLAYED,
        message: `Player ${player_id} picked up cards`,
        game: personalize_game(game, null)
    });

    return game;
}