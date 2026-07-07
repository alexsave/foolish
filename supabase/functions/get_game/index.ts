import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import { corsHeaders, handleCors } from "../_shared/cors.ts";
import { getAuthenticatedUser } from "../_shared/auth.ts";
import { loadCompleteGame } from "../_shared/utils.ts";
import { personalize_game } from "../_shared/common_utils.ts";
import "jsr:@supabase/functions-js/edge-runtime.d.ts";

// Read-only personalized game fetch. Replaces the client's direct PostgREST
// reads of `games` + `player_hands`: the volatile game state now lives in the
// packed kernel blob (games.state), which only the server unpacks.
// loadCompleteGame reconstructs from the blob; personalize_game returns the
// caller's view — a PersonalGame (own hand, everyone else's as card-backs) if
// they're a player, a PublicGame if they're a spectator. No commit, no lock:
// this endpoint never mutates.
//
// Forward note: once the client runs the rules kernel in-browser, this returns
// a per-player MASKED packed blob instead of JSON and the client unpacks it —
// same versioned format (engine.ts serializeGameState), zero conversion. See
// docs/STATE_BLOB_CUTOVER.md.
serve(async (req: Request): Promise<Response> => {
    const cors = handleCors(req);
    if (cors) return cors;

    try {
        const user = await getAuthenticatedUser(req);

        let body: any = {};
        try { body = await req.json(); } catch { /* empty body */ }
        const game_id = body.game_id;
        if (!game_id) throw new Error('game_id is required');

        const game = await loadCompleteGame(game_id);
        return new Response(JSON.stringify(personalize_game(game, user.id)), {
            headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    } catch (e: any) {
        return new Response(JSON.stringify({ error: e.message }), {
            status: 400, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    }
});
