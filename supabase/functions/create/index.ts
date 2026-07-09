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

    // Persist AFTER the response, in the background. `create` is the ONE handler
    // whose response depends on NO database read — the new lobby is fully
    // determined by the inputs above, so the client can be handed the finished
    // game immediately while the DB write happens off the critical path. Every
    // other action must first LOAD the game, so none of them can defer like this.
    // EdgeRuntime.waitUntil keeps the isolate alive until the write settles
    // (same mechanism as the post-response bot loop); this takes the create_game
    // PostgREST round-trip (~594ms from a cold isolate) off what the user waits on.
    //
    // The create_game RPC does the three inserts (games → game_decks →
    // player_hands) + the player_views seed in one transaction; p_views (built by
    // the pure-TS lobby mirror, no rules-wasm) makes the new lobby readable from
    // the dashboard's direct player_views SELECT. Retry a few times so a
    // background failure (which the client can't see — it already got the game)
    // doesn't silently drop the write; a unique-violation means an earlier
    // attempt actually landed, so treat it as success.
    const persist = (async () => {
        const p_views = await buildPlayerViewRows(dbGameData, null, 0);
        for (let attempt = 1; attempt <= 3; attempt++) {
            const { error } = await supabaseClient.rpc('create_game', {
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
            if (!error) return;
            if ((error as { code?: string }).code === '23505') return; // already inserted by a prior attempt
            console.error(`[create] background persist attempt ${attempt}/3 failed for ${game_id}: ${error.message}`);
        }
        console.error(`[create] background persist GAVE UP for ${game_id} — the client holds a game that isn't in the DB`);
    })();

    const er = (globalThis as { EdgeRuntime?: { waitUntil?: (p: Promise<unknown>) => void } }).EdgeRuntime;
    if (er && typeof er.waitUntil === 'function') {
        er.waitUntil(persist); // survive the response; write in the background
    } else {
        await persist; // no EdgeRuntime (local/test): don't lose the write
    }

    // No broadcast on create: a just-created game has exactly one member (the
    // creator), who already has the full state above via the HTTP response, and
    // nobody is subscribed to the channels of a game that didn't exist a moment
    // ago. The old creation broadcast reached no one yet cost ~800ms of function
    // time (worse, via the Realtime→REST fallback). Returning no events skips it.
    return { game: dbGameData, events: [] };
}, false);
