import { verify_game_id, wrap400, validate_defender_status, get_next_player_index, refill, verify_player_in_game, personalize_game, broadcastToGameUsers, loadCompleteGame, saveCompleteGame } from "../_shared/utils.ts";
import { GAME_STATUS, SERVER_EVENT_TYPE, Game, Player } from "../_shared/types.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { getAuthenticatedUser } from "../_shared/auth.ts";
import { User } from "npm:@supabase/supabase-js@2.39.0"
import { handleCors, corsHeaders } from "../_shared/cors.ts";

serve(wrap400(async (req) => {
    const corsResponse = handleCors(req);
    if (corsResponse) return corsResponse;

    const user: User = await getAuthenticatedUser(req);
    const user_id = user.id;
    const { game_id } = await req.json();

    // Verify game exists and player is in game
    await verify_game_id(game_id);
    await verify_player_in_game(game_id, user_id);

    // Load complete game state using JOINs
    let game = await loadCompleteGame(game_id);

    // Handle pickup logic
    game = handle_pickup(game, game_id, user_id);

    // Save complete game state back to separated tables
    await saveCompleteGame(game);

    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.PICKUP_PLAYED,
        message: `Player ${user_id} picked up cards`
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
    const defender: Player = game.players.find(player => player.id === player_id)!;

    // add cards from table to hand
    game.table_battles.forEach(battle => {
        defender.hand.push(battle.attack);
        if (battle.defense) {
            defender.hand.push(battle.defense);
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

    // Broadcasting will be handled by the main function

    return game;
}