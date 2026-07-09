import { wrap400, ExecutionParams } from "../_shared/utils.ts";
import { Game, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from "../_shared/types.ts";
import { createId } from "../_shared/common_utils.ts";
import { buildPlayerViewRows } from "../_shared/player_views.ts";
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

    // [perf] Split the single "Direct execute" timing into its two parts — the
    // masked-view build (pure TS for a lobby) vs the create_game PostgREST
    // round-trip — so the "create is slow" investigation can see which dominates.
    const tViews = performance.now();
    // p_views seeds the creator's player_views cache row (version 0) in the SAME
    // transaction as the inserts, so the new lobby is readable from the
    // dashboard's direct player_views SELECT immediately (docs/PLAYER_VIEWS.md).
    // A lobby has no hidden state, so this masked view is built by the pure-TS
    // mirror (no rules-wasm on the create cold path) — expected to be ~0ms.
    const p_views = await buildPlayerViewRows(dbGameData, null, 0);
    console.log(`[perf][create] buildPlayerViewRows ${(performance.now() - tViews).toFixed(0)}ms`);

    // [perf] Optional cold-connection probe. create_game is normally the FIRST
    // edge→PostgREST call in the isolate (auth verifies the JWT LOCALLY — no DB
    // touch), so it pays the full cold PostgREST connection/HTTP setup (~750ms
    // cold per docs/PLAYER_VIEWS.md), NOT the four tiny inserts. Set
    // CREATE_TIMING_PROBE=1 to fire a trivial round-trip first: it absorbs the
    // cold setup, so the create_game timing below then reflects the query alone —
    // isolating connection cost from query cost. Off by default (adds a hop).
    if (Deno.env.get('CREATE_TIMING_PROBE')) {
        const tProbe = performance.now();
        await supabaseClient.from('games').select('id').limit(1);
        console.log(`[perf][create] postgrest warmup probe ${(performance.now() - tProbe).toFixed(0)}ms`);
    }

    // Create the game in ONE round-trip: the create_game RPC does the three
    // inserts (games → game_decks → player_hands) + the player_views seed in a
    // single transaction, replacing what used to be sequential PostgREST calls
    // (part of #6's slow create).
    const tRpc = performance.now();
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
        p_views,
    });
    console.log(`[perf][create] create_game rpc ${(performance.now() - tRpc).toFixed(0)}ms`);
    if (createError) {
        throw new Error(`Failed to create game: ${createError.message}`);
    }

    // No broadcast on create: a just-created game has exactly one member (the
    // creator), who already has the full state above via the HTTP response, and
    // nobody is subscribed to the channels of a game that didn't exist a moment
    // ago. The old creation broadcast reached no one yet cost ~800ms of function
    // time (worse, via the Realtime→REST fallback). Returning no events skips it.
    return { game: dbGameData, events: [] };
}, false);
