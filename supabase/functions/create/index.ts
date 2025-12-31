import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { Game, PlayerHand, AnimationEvent, ANIMATION_EVENT_TYPE } from "../_shared/types.ts";
import { createId } from "../_shared/common_utils.ts";
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async ({ user, user_name }: ExecutionParams) => {
    const user_id = user.id;
    
    // Generate unique game ID
    let game_id: string = createId();
    const game_name = `${user_name}'s Game`;

    // 1. Create the game with placeholder data
    const gameData = {
        id: game_id,
        name: game_name,
        players: [{
            player_id: user_id,
            name: user_name,
            status: 'idle',
            is_ai: false
        }],
        status: 'waiting',
        deck_length: 0,
        discard_pile_length: 0,
        flipped: null,
        power_suit: 0,
        first_attacker: 0,
        defender: 0,
        table_battles: [],
        elimination_order: [],
        good_timestamp: null,
        good_players: [],
        logs: []
    };

    await supabaseClient.from('games').insert(gameData);

    // 2. Initialize empty deck
    await supabaseClient.from('game_decks').insert({
        game_id: game_id,
        deck: []
    });

    // 3. Initialize empty hand for creator (also serves as player-game relationship)
    await supabaseClient.from('player_hands').insert({
        game_id: game_id,
        player_id: user_id,
        hand: [],
        awaiting_attack: false
    } as PlayerHand);

    // Construct complete game state directly from inserted data (no need to query back)
    const dbGameData: Game = {
        id: game_id,
        name: game_name,
        deck: [],
        deck_length: 0,
        discard_pile_length: 0,
        flipped: null,
        players: [{
            player_id: user_id,
            name: user_name,
            status: 'idle',
            is_ai: false,
            hand: [],
            hand_length: 0,
            awaiting_attack: false,
            done_attacking_this_round: false
        }],
        status: 'waiting',
        power_suit: 0,
        first_attacker: 0,
        defender: 0,
        table_battles: [],
        elimination_order: [],
        good_timestamp: null,
        good_players: [],
        logs: []
    };

    // Create game creation event
    const creationEvent: AnimationEvent = {
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `Game created by ${user_name}`,
        game_state: dbGameData
    };

    return { game: dbGameData, events: [creationEvent] };
});
