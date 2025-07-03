import { loadCompleteGame, saveCompleteGame, personalize_game, wrap400, broadcastToGameUser } from "../_shared/utils.ts";
import { SERVER_EVENT_TYPE, Game } from "../_shared/types.ts";

import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"

serve(wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id, card_indices } = body;

    if (!card_indices || !Array.isArray(card_indices)) {
        throw new Error('Missing required field: card_indices');
    }

    // Load the complete game
    const game: Game = await loadCompleteGame(game_id);

    // Check if the user is in the game
    const player = game.players.find(player => player.id === user_id);
    if (!player) {
        throw new Error('You are not in this game');
    }

    // Validate indices
    if (card_indices.length !== player.hand.length || 
        !card_indices.every((idx: number) => idx >= 0 && idx < player.hand.length)) {
        throw new Error('Invalid card indices');
    }

    // Rearrange the hand according to the provided indices
    const rearrangedHand = card_indices.map((index: number) => player.hand[index]);

    // Update the player's hand in the game object
    player.hand = rearrangedHand;

    // Update the player's hand in the database
    await saveCompleteGame(game);

    // This doesn't need a broadcast to everyone
    broadcastToGameUser(game, SERVER_EVENT_TYPE.HAND_REARRANGED, {
        message: `${user_name} rearranged their hand`
    }, user_id);

    return {
        game: personalize_game(game, user_id)
    };
})); 