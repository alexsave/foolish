import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import { corsHeaders, handleCors } from "../_shared/adapter/cors.ts";
import { getAuthenticatedUser } from "../_shared/adapter/auth.ts";
import { createClient } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts";

// Account deletion (docs/ORACLE_MONETIZATION_ENGINEERING.md §4; App Store
// Guideline 5.1.1(v); Google Play data-deletion policy). Reached two ways — the
// in-app Settings button (iOS/Android) and the standalone /delete-account web
// page (Play requires a path that works without the app). Both send the user's
// bearer token; this function authenticates it, scrubs shared-history PII via
// the delete_account RPC, then deletes the auth user (which revokes sessions and
// cascades every row the user solely owns).
//
// Idempotent-ish: a second call after the user is gone fails auth (token
// invalid) and returns 401 — which the client treats as "already deleted".

const admin = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

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
        // No/!valid token → treat as already-deleted / unauthenticated.
        return new Response(JSON.stringify({ error: (e as Error).message }), {
            status: 401, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    }

    try {
        // 1. Scrub PII the user shares with other players' rows (their name in
        //    games.players; the denormalized elo username). Owned rows cascade.
        const { error: scrubErr } = await admin.rpc('delete_account', { p_user_id: userId });
        if (scrubErr) throw new Error(`scrub failed: ${scrubErr.message}`);

        // 2. Delete the auth user. Revokes sessions and cascades owned rows
        //    (player_hands, player_views, user_elo_ratings, …).
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
