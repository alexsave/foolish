import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import { corsHeaders, handleCors } from "../_shared/cors.ts";
import { getAuthenticatedUser } from "../_shared/auth.ts";
import { Game, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from "../_shared/types.ts";
import { createId, personalize_game } from "../_shared/common_utils.ts";
import { buildPlayerViewRows } from "../_shared/player_views.ts";
import { createClient } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts";

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// Decode the bare-hex packed envelope buildPlayerViewRows produces into its raw
// bytes (the response body). Tiny local helper — create stays off the replay
// codec / rules-wasm boot graph.
const hexToBytes = (hex: string): Uint8Array => {
    const out = new Uint8Array(hex.length >> 1);
    for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
    return out;
};

// Create a game. A standalone handler (not wrap400) because it returns the same
// PACKED view buffer get_game returns — the client decodes it with the shared
// decodePackedGame, so create is no longer a JSON special case.
//
// `create` is the ONE handler whose response depends on NO database read: the new
// lobby is fully determined by the inputs, so we build the creator's masked view
// (which IS both the response body AND the player_views cache seed) and hand it
// back immediately, then do the DB write AFTER the response, in the background
// (EdgeRuntime.waitUntil keeps the isolate alive — same as the post-response bot
// loop). This takes the create_game PostgREST round-trip (~594ms from a cold
// isolate) off what the user waits on. Every other action must first LOAD a game,
// so none of them can defer like this.
serve(async (req: Request): Promise<Response> => {
    const cors = handleCors(req);
    if (cors) return cors;

    try {
        const user = await getAuthenticatedUser(req);
        const user_id = user.id;
        const user_name = user.user_metadata.username;

        const game_id: string = createId();
        const game_name = `${user_name}'s Game`;

        // The lobby Game — fed to the SAME masked-view builder commit_game /
        // create_game use, so the buffer we return and the row we seed are
        // byte-identical to a later player_views read / get_game fetch.
        const dbGameData: Game = {
            id: game_id, name: game_name, deck: [], deck_length: 0, discard_pile_length: 0,
            flipped: null,
            players: [{
                player_id: user_id, name: user_name, status: PLAYER_STATUS.IDLE, is_ai: false,
                hand: [], hand_length: 0, awaiting_attack: false, strategy_key: STRATEGY_KEY.HUMAN,
            }],
            status: GAME_STATUS.WAITING, power_suit: 0, first_attacker: 0, defender: 0,
            table_battles: [], elimination_order: [], good_timestamp: null, good_players: [], logs: [],
        };

        // The creator's packed view envelope (built by the pure-TS lobby mirror —
        // no rules-wasm): the response body AND the player_views cache seed.
        const p_views = await buildPlayerViewRows(dbGameData, null, 0);
        const mine = p_views.find(r => r.player_id === user_id);

        // Persist AFTER the response (create_game does the 3 inserts + the
        // player_views seed in one transaction — the fastest, atomic way). Retry
        // a few times: a background failure is invisible to the client now (it
        // already has the game), so don't silently drop the write; a
        // unique-violation means an earlier attempt landed → treat as success.
        const persist = (async () => {
            for (let attempt = 1; attempt <= 3; attempt++) {
                const { error } = await supabaseClient.rpc('create_game', {
                    p_game_id: game_id,
                    p_name: game_name,
                    p_player_id: user_id,
                    p_players: [{ player_id: user_id, name: user_name, status: PLAYER_STATUS.IDLE, is_ai: false }],
                    p_views,
                });
                if (!error) return;
                if ((error as { code?: string }).code === '23505') return; // already inserted by a prior attempt
                console.error(`[create] background persist attempt ${attempt}/3 failed for ${game_id}: ${error.message}`);
            }
            console.error(`[create] background persist GAVE UP for ${game_id} — the client holds a game that isn't in the DB`);
        })();

        const er = (globalThis as { EdgeRuntime?: { waitUntil?: (p: Promise<unknown>) => void } }).EdgeRuntime;
        if (er && typeof er.waitUntil === 'function') er.waitUntil(persist);
        else await persist; // no EdgeRuntime (local/test): don't lose the write

        // Return the packed view buffer (decodable by decodePackedGame). A human
        // creator always yields a row; the JSON fallback is defensive only.
        if (mine) {
            return new Response(hexToBytes(mine.view) as unknown as BodyInit, {
                headers: { ...corsHeaders, 'Content-Type': 'application/octet-stream' },
            });
        }
        return new Response(JSON.stringify(personalize_game(dbGameData, user_id)), {
            headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    } catch (e: any) {
        return new Response(JSON.stringify({ error: e.message }), {
            status: 400, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    }
});
