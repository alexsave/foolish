import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import { corsHeaders, handleCors } from "@shared/adapter/cors.ts";
import { getAuthenticatedUser } from "@shared/adapter/auth.ts";
import { serverTable, tableCodeName } from "@sdk/ts/table/server_table.ts";
import { base64 } from "@shared/adapter/table_io.ts";
import { createClient } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts";

const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// A new game id: six characters of a random UUID. That is 24 bits, so a draw
// can name a game that already exists: create_table refuses it with a unique
// violation, and create draws again.
const newGameId = (): string => crypto.randomUUID().slice(0, 6);

/** Draws before create gives up. One draw collides with probability (live games) / 2^24, so five in a row means something else is wrong. */
const MAX_ID_DRAWS = 5;

// Create a game (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b): table_create seats
// the caller in a new lobby, and the commit products are the whole row - the
// state and roster blobs, the creator's envelope (which is also the response
// body) and the spectator envelope.
//
// The row is stored BEFORE the response. The envelope names the game id, so an
// answer given before create_table landed could hand the creator a game that
// was never stored, or, when the id collided, somebody else's game (it used to
// write after responding and read a unique violation as "already saved").
serve(async (req: Request): Promise<Response> => {
    const cors = handleCors(req);
    if (cors) return cors;

    try {
        const user = await getAuthenticatedUser(req);
        const name = user.user_metadata.username ?? '';
        const table = await serverTable();

        for (let draw = 1; draw <= MAX_ID_DRAWS; draw++) {
            const gameId = newGameId();

            // ---- kernel section: the products name the game id, so each draw builds its own ----
            const rc = table.create(user.id, name);
            if (rc < 0) throw new Error(`create refused: ${tableCodeName(rc, ['TABLE_E_', 'ROSTER_E_'])} (detail ${table.detail()})`);
            const p = table.commit(gameId, 0, Date.now());
            if (typeof p === 'number') throw new Error(`create: no products (${tableCodeName(p, ['TABLE_E_'])})`);
            const mine = p.views[0];
            // ---- end of the kernel section ----
            if (!mine) throw new Error('create: no envelope for the creator');

            const { error } = await supabaseClient.rpc('create_table', {
                p_game_id: gameId,
                p_player_id: user.id,
                p_state: base64(p.state),
                p_roster: base64(p.roster),
                p_view: base64(mine),
                p_spectator: base64(p.spectator),
            });
            if (!error) {
                return new Response(mine as unknown as BodyInit, {
                    headers: { ...corsHeaders, 'Content-Type': 'application/octet-stream' },
                });
            }
            // unique_violation: another game holds this id (create_table's only
            // unique insert is the games row; the rest is ON CONFLICT or new keys).
            if ((error as { code?: string }).code !== '23505') throw new Error(`create: storing ${gameId} failed: ${error.message}`);
            console.warn(`[create] game id ${gameId} is taken (draw ${draw}/${MAX_ID_DRAWS}), drawing again`);
        }
        throw new Error(`create: no free game id in ${MAX_ID_DRAWS} draws`);
    } catch (e: unknown) {
        return new Response(JSON.stringify({ error: (e as Error).message }), {
            status: 400, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    }
});
