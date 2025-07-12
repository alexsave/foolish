import { loadCompleteGame, saveCompleteGame, wrap400, broadcastToGameUsers } from "../_shared/utils.ts";
import { GAME_STATUS, PLAYER_STATUS, Game, SERVER_EVENT_TYPE, PrivatePlayer } from "../_shared/types.ts";
import { MAX_PLAYERS } from "../_shared/constants.ts";
import { personalize_game } from "../_shared/common_utils.ts";

import { createClient } from "npm:@supabase/supabase-js@2.39.0"

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id } = body;

    // We have to load the game no matter what
    const game: Game = await loadCompleteGame(game_id);

    // Check if player is already in game by checking player_hands
    const { data: player_hand, error: player_handError } = await supabaseClient
        .from('player_hands')
        .select('*')
        .eq('game_id', game_id)
        .eq('player_id', user_id)
        .single();

    if (!player_handError && player_hand) {
        // Player is already in the game
        // quiet return, don't worry about it
        return {
            game: personalize_game(game, user_id)
        };
    }

    if (game.status !== GAME_STATUS.WAITING) {
        throw new Error(`Game ${game_id} is not waiting for players, wait for next game`);
    }

    if (game.players.length >= MAX_PLAYERS) {
        throw new Error(`Game ${game_id} is full, wait for next game`);
    }

    // Create new player
    const privatePlayer: PrivatePlayer = {
        name: user_name,
        player_id: user_id,
        status: PLAYER_STATUS.IDLE,
        hand: [],
        awaiting_attack: false,
        hand_length: 0,
        is_ai: false,
        done_attacking_this_round: false
    }

    // Add new player to game
    game.players.push(privatePlayer);

    // Save to database - this handles all simplified tables
    await saveCompleteGame(game);

    // Send broadcast notification
    broadcastToGameUsers(game, 'game_update', {
        type: SERVER_EVENT_TYPE.PLAYER_JOINED_GAME,
        message: `Player ${user_name} joined game ${game_id}`
    });

    return {
        game: personalize_game(game, user_id)
    };
});

