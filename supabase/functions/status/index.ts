import { wrap400, personalize_game, loadCompleteGame } from '../_shared/utils.ts';
import { Game, PublicGame, PublicPlayer } from '../_shared/types.ts'; 

import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"

// TODO: just remove this. With the right policies we can query from client
serve(wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const { game_id } = body;

    // Load complete game state from separated tables
    const game: Game = await loadCompleteGame(game_id);

    // Check if player is in game
    const playerInGame = game.players.find(player => player.id === user_id);
    
    if (playerInGame) {
        // Player is in game, return personalized view
        return {
            game: personalize_game(game, user_id)
        };
    } else {
        // Player is not in game, return public view for spectating
        const publicGame: PublicGame = {
            id: game.id,
            name: game.name,
            deck_length: game.deck.length,
            flipped: game.flipped,
            players: game.players.map(player => ({
                name: player.name,
                id: player.id,
                status: player.status,
                hand_length: player.hand.length
            } as PublicPlayer)),
            status: game.status,
            power_suit: game.power_suit,
            first_attacker: game.first_attacker,
            defender: game.defender,
            table_battles: game.table_battles,
        };
        
        return {
            game: publicGame
        };
    }
}));
