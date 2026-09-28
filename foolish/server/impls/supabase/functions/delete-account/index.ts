import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import * as L from "@sdk/ts/gen/game_layout.bots.ts";
import { corsHeaders, handleCors } from "@shared/adapter/cors.ts";
import { getAuthenticatedUser } from "@shared/adapter/auth.ts";
import { GameNotFound, runTableOp, TableRefusal } from "@shared/adapter/table_io.ts";
import { createClient } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts";

// Account deletion (docs/ORACLE_MONETIZATION_ENGINEERING.md §4; App Store
// Guideline 5.1.1(v); Google Play data-deletion policy). Reached two ways - the
// in-app Settings button (iOS/Android) and the standalone /delete-account web
// page (Play requires a path that works without the app). Both send the user's
// bearer token; this function authenticates it, scrubs the user's name from
// every table they sit at, then deletes the auth user (which revokes sessions and
// cascades every row the user solely owns).
//
// The scrub (docs/C_GAME_SHAPE_MIGRATION.md Q8) is one C Table operation per
// game, table_redact, committed like any other edit: it renames the seat in the
// roster AND rewrites every cached view (player_views, spectator_views), whose
// envelopes carry the names too. The delete_account RPC keeps its own part
// (the denormalized leaderboard username, and the JSONB roster of a row an older
// function still writes).
//
// Idempotent-ish: a second call after the user is gone fails auth (token
// invalid) and returns 401 - which the client treats as "already deleted".

const admin = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

/** The name a deleted user's seat keeps. */
export const DELETED_PLAYER_NAME = 'Deleted player';

serve(async (req: Request): Promise<Response> => {
    const cors = handleCors(req);
    if (cors) return cors;

    if (req.method !== 'POST') {
        return new Response(JSON.stringify({ error: 'method not allowed' }), {
            status: 405, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    }

    let userId: string;
    try {
        const user = await getAuthenticatedUser(req);
        userId = user.id;
    } catch (e) {
        // No or invalid token: treat as already deleted / unauthenticated.
        return new Response(JSON.stringify({ error: (e as Error).message }), {
            status: 401, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    }

    const reqId = `delete-${crypto.randomUUID().split('-')[0]}`;
    try {
        // 1. Every table the user sits at, before the auth delete cascades the
        //    membership rows away.
        const { data: seats, error: seatsErr } = await admin
            .from('player_hands').select('game_id').eq('player_id', userId);
        if (seatsErr) throw new Error(`membership read failed: ${seatsErr.message}`);

        // 2. Redact the name in each, one CAS commit per game.
        for (const { game_id: gameId } of seats ?? []) {
            try {
                await runTableOp({
                    gameId, reqId, viewerId: null,
                    run: ({ table }) => table.redact(userId, DELETED_PLAYER_NAME),
                    fresh: true,
                });
            } catch (e) {
                // A stale membership row (no seat, or no game) has no name to scrub.
                if (e instanceof GameNotFound) continue;
                if (e instanceof TableRefusal && e.code === L.TABLE_E_NOT_SEATED) continue;
                throw e;
            }
        }

        // 3. The rest of the shared-history scrub (leaderboard username).
        const { error: scrubErr } = await admin.rpc('delete_account', { p_user_id: userId });
        if (scrubErr) throw new Error(`scrub failed: ${scrubErr.message}`);

        // 4. Delete the auth user. Revokes sessions and cascades owned rows
        //    (player_hands, player_views, user_elo_ratings, ...).
        const { error: delErr } = await admin.auth.admin.deleteUser(userId);
        if (delErr) throw new Error(`auth delete failed: ${delErr.message}`);

        return new Response(JSON.stringify({ status: 'deleted' }), {
            status: 200, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    } catch (e) {
        console.error(`[delete-account] failed for ${userId}: ${(e as Error).message}`);
        return new Response(JSON.stringify({ error: (e as Error).message }), {
            status: 500, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    }
});
