import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { Game, PlayerHand, AnimationEvent, ANIMATION_EVENT_TYPE } from "../_shared/types.ts";
import { createId } from "../_shared/common_utils.ts";
import { createClient } from 'jsr:@supabase/supabase-js';

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

wrap400(async ({user, user_name, reqId }: ExecutionParams) => {
    const createStartTime = Date.now();
    console.log(`[${reqId}][CREATE] Starting game creation for user ${user_name}`);
    
    const user_id = user.id;
    
    // Generate unique game ID
    const idGenStart = Date.now();
    let game_id: string = createId();
    const game_name = `${user_name}'s Game`;
    console.log(`[${reqId}][CREATE] Generated game_id ${game_id} (took ${Date.now() - idGenStart}ms)`);

    // Construct complete game state in memory (no DB query needed)
    const constructStart = Date.now();
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
        elimination_order: []
    };
    console.log(`[${reqId}][CREATE] Construct game object took ${Date.now() - constructStart}ms`);

    // Create game creation event
    const eventStart = Date.now();
    const creationEvent: AnimationEvent = {
        type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
        message: `Game created by ${user_name}`,
        game_state: dbGameData
    };
    console.log(`[${reqId}][CREATE] Create event took ${Date.now() - eventStart}ms`);

    const totalTime = Date.now() - createStartTime;
    console.log(`[${reqId}][CREATE] Returning response (took ${totalTime}ms) - DB inserts will happen in background`);

    // Fire-and-forget: Save to database AFTER returning response
    // By the time user navigates to lobby, this will be complete
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
        elimination_order: []
    };

    Promise.all([
        supabaseClient.from('games').insert(gameData),
        supabaseClient.from('game_decks').insert({
            game_id: game_id,
            deck: []
        }),
        supabaseClient.from('player_hands').insert({
            game_id: game_id,
            player_id: user_id,
            hand: [],
            awaiting_attack: false
        } as PlayerHand)
    ]).then(() => {
        console.log(`[${reqId}][CREATE] Background DB inserts completed (${Date.now() - createStartTime}ms after request start)`);
    }).catch(error => {
        console.error(`[${reqId}][CREATE] Background DB inserts FAILED:`, error);
    });

    return { game: dbGameData, events: [creationEvent] };
});
