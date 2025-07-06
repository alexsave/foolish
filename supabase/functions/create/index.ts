import { createId, wrap400, broadcastToGameUsers, loadCompleteGame, personalize_game } from "../_shared/utils.ts";
import { GAME_STATUS, PLAYER_STATUS, Game, PublicGame, PlayerHand, GameDeck } from "../_shared/types.ts";

import { createClient } from "npm:@supabase/supabase-js@2.39.0"

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async (user, user_name, body) => {
    const user_id = user.id;
    const game_id = createId();

    // Create the game object for database insert (separated schema)

    // Insert public game data (without deck/hands)
    const publicGameData: PublicGame = {
        id: game_id,
        name: `${user_name}'s Game`,
        deck_length: 0,
        flipped: null,
        players: [{
            name: user_name,
            player_id: user_id,
            status: PLAYER_STATUS.IDLE,
            hand_length: 0,
        }],
        status: GAME_STATUS.WAITING,
        power_suit: 0,
        first_attacker: 0,
        defender: 0,
        table_battles: []
    };

    // 1. Games table (public data only)
    await supabaseClient.from('games').insert(publicGameData);
    
    // 2. Initialize empty deck (will be filled when game starts)
    await supabaseClient.from('game_decks').insert({
        game_id: game_id,
        deck: []
    } as GameDeck);

    // 3. Initialize empty hand for creator (also serves as player-game relationship)
    await supabaseClient.from('player_hands').insert({
        game_id: game_id,
        player_id: user_id,
        hand: [],
        awaiting_attack: false
    } as PlayerHand);

    // Load complete game state from separated tables
    const dbGameData: Game = await loadCompleteGame(game_id);

    // Send broadcast notification asynchronously (non-blocking)
    broadcastToGameUsers(dbGameData, 'game_update', {
        type: 'game_created',
        message: `Game created with id ${game_id}`
    });

    return {
        game: personalize_game(dbGameData, user_id)
    };
});
