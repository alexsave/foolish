import { verify_game_id, wrap400, get_next_player_index, refill, verify_player_in_game, personalize_game, broadcastToGameUsers, loadCompleteGame, saveCompleteGame } from "../_shared/utils.ts";
import { GAME_STATUS, PLAYER_STATUS, SERVER_EVENT_TYPE, Game } from "../_shared/types.ts";

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

    // Handle good logic
    game = handle_good(game, game_id, user_id);

    // Save complete game state back to separated tables
    await saveCompleteGame(game);

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

const handle_good = (game: Game, game_id: string, player_id: string): Game => {
    // player is done attacking
    // we need to check if they have any cards left in their hand

    if (game.status !== GAME_STATUS.WAIT_FOR_ATTACKERS) {
        throw new Error(`Game ${game_id} is not in wait_for_attackers mode`);
    }
    const player = game.players.find(player => player.id === player_id)!;
    // If they're in but can't play cards, just let them proceed
    if (player.status !== PLAYER_STATUS.IN && player.status !== PLAYER_STATUS.AWAITING_ATTACK) {
        throw new Error(`Player ${player_id} is not ready to attack`);
    }

    // set them to done attacking
    player.status = PLAYER_STATUS.IN;

    // ok now we need to check if all players are done attacking
    // dont count the defender
    // the status check is critical

    const playable_players = game.players.filter(player => player.id !== game.players[game.currently_attacked].id && player.hand.some(card => card.value === game.flipped!.value) && player.status === PLAYER_STATUS.AWAITING_ATTACK);

    if (playable_players.length !== 0) {
        return game;
    }

    // we are done attacking.
    // this has to be after a successful cover. Otherwise we'd still be waiting on the defender
    // shift
    // change all done_attacking to in
    game.players.forEach(player => {
        if (player.status === PLAYER_STATUS.AWAITING_ATTACK) {
            player.status = PLAYER_STATUS.IN;
        }
    });

    game.table_battles = [];
    refill(game);

    //shift 
    game.first_attacker = game.currently_attacked;
    game.currently_attacked = get_next_player_index(game, game.first_attacker);
    game.status = GAME_STATUS.FIRST_ATTACKER;

    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.SUCCESSFULLY_COVERED,
        message: `Player ${player_id} successfully defended the attack`
    });

    return game;
}