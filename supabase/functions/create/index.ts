import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { Game, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from "../_shared/types.ts";
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

    // Create the game in ONE round-trip: the create_game RPC does the three
    // inserts (games → game_decks → player_hands) in a single transaction,
    // replacing what used to be sequential PostgREST calls (part of #6's slow
    // create). The lobby player row carries only public fields; hand/length are
    // filled in below for the returned in-memory game.
    const { error: createError } = await supabaseClient.rpc('create_game', {
        p_game_id: game_id,
        p_name: game_name,
        p_player_id: user_id,
        p_players: [{
            player_id: user_id,
            name: user_name,
            status: PLAYER_STATUS.IDLE,
            is_ai: false
        }],
    });
    if (createError) {
        throw new Error(`Failed to create game: ${createError.message}`);
    }

    // Construct complete game state directly from inserted data (no need to query back).
    // Returned synchronously in the HTTP response — which is the ONLY consumer.
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
            status: PLAYER_STATUS.IDLE,
            is_ai: false,
            hand: [],
            hand_length: 0,
            awaiting_attack: false,
            strategy_key: STRATEGY_KEY.HUMAN
        }],
        status: GAME_STATUS.WAITING,
        power_suit: 0,
        first_attacker: 0,
        defender: 0,
        table_battles: [],
        elimination_order: [],
        good_timestamp: null,
        good_players: [],
        logs: []
    };

    // No broadcast on create: a just-created game has exactly one member (the
    // creator), who already has the full state above via the HTTP response, and
    // nobody is subscribed to the channels of a game that didn't exist a moment
    // ago. The old creation broadcast reached no one yet cost ~800ms of function
    // time (worse, via the Realtime→REST fallback). Returning no events skips it.
    return { game: dbGameData, events: [] };
}, false);
