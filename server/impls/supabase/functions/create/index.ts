import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import { corsHeaders, handleCors } from "@shared/adapter/cors.ts";
import { getAuthenticatedUser } from "@shared/adapter/auth.ts";
import { gameStatusLabel, serverTable, tableCodeName } from "@sdk/ts/table/server_table.ts";
import { bytesToBareHex } from "@sdk/ts/wire/bytes.ts";
import { createClient } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts";

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// A new game id: six characters of a random UUID.
const newGameId = (): string => crypto.randomUUID().slice(0, 6);

// Create a game (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b): table_create seats
// the caller in a new lobby, and the commit products are the whole row - the
// state and roster blobs, the creator's envelope (which is also the response
// body) and the spectator envelope.
//
// `create` is the one handler whose response depends on NO database read, so
// the row is written AFTER the response, in the background (EdgeRuntime.waitUntil
// keeps the isolate alive). That takes the create_table round-trip off what the
// user waits on.
serve(async (req: Request): Promise<Response> => {
    const cors = handleCors(req);
    if (cors) return cors;

    try {
        const user = await getAuthenticatedUser(req);
        const gameId = newGameId();
        const table = await serverTable();

        // ---- kernel section ----
        const rc = table.create(user.id, user.user_metadata.username ?? '');
        if (rc < 0) throw new Error(`create refused: ${tableCodeName(rc, ['TABLE_E_', 'ROSTER_E_'])} (detail ${table.detail()})`);
        const p = table.commit(gameId, 0, Date.now());
        if (typeof p === 'number') throw new Error(`create: no products (${tableCodeName(p, ['TABLE_E_'])})`);
        const mine = p.views[0];
        // ---- end of the kernel section ----
        if (!mine) throw new Error('create: no envelope for the creator');

        const status = gameStatusLabel(p.status);
        const persist = (async () => {
            // Retry a few times: a background failure is invisible to the client
            // (it already has the game). A unique violation means an earlier
            // attempt landed.
            for (let attempt = 1; attempt <= 3; attempt++) {
                const { error } = await supabaseClient.rpc('create_table', {
                    p_game_id: gameId,
                    p_player_id: user.id,
                    p_state: `\\x${bytesToBareHex(p.state)}`,
                    p_roster: `\\x${bytesToBareHex(p.roster)}`,
                    p_views: [{ player_id: user.id, view: bytesToBareHex(mine), status }],
                    p_spectator: bytesToBareHex(p.spectator),
                });
                if (!error) return;
                if ((error as { code?: string }).code === '23505') return;
                console.error(`[create] background persist attempt ${attempt}/3 failed for ${gameId}: ${error.message}`);
            }
            console.error(`[create] background persist GAVE UP for ${gameId}: the client holds a game that isn't in the DB`);
        })();

        const er = (globalThis as { EdgeRuntime?: { waitUntil?: (p: Promise<unknown>) => void } }).EdgeRuntime;
        if (er && typeof er.waitUntil === 'function') er.waitUntil(persist);
        else await persist; // no EdgeRuntime (local/test): don't lose the write

        return new Response(mine as unknown as BodyInit, {
            headers: { ...corsHeaders, 'Content-Type': 'application/octet-stream' },
        });
    } catch (e: unknown) {
        return new Response(JSON.stringify({ error: (e as Error).message }), {
            status: 400, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    }
});
