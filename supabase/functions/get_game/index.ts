import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import { corsHeaders, handleCors } from "../_shared/cors.ts";
import { getAuthenticatedUser } from "../_shared/auth.ts";
import { loadCompleteGame, supabaseClient } from "../_shared/utils.ts";
import { personalize_game } from "../_shared/common_utils.ts";
import { buildPackedGameBytes } from "../_shared/packed_game.ts";
import "jsr:@supabase/functions-js/edge-runtime.d.ts";

// Read-only personalized game fetch. Two shapes:
//
// - body {game_id, packed: true} (the current client): the response body is
//   BINARY — the caller's kernel-masked view blob straight from the C rules
//   kernel (wasm_view_serialize) plus a small identity roster JSON section.
//   The state blob is never unpacked into a JS Game on this path; masking
//   ("you only see your own hand") happens inside the kernel. See
//   docs/PACKED_WIRE_CUTOVER.md.
// - body {game_id} (legacy): personalize_game JSON, same as before. Also the
//   fallback for lobby/legacy rows that carry no state blob.
//
// No commit, no lock: this endpoint never mutates.
serve(async (req: Request): Promise<Response> => {
    const cors = handleCors(req);
    if (cors) return cors;

    try {
        const user = await getAuthenticatedUser(req);

        let body: any = {};
        try { body = await req.json(); } catch { /* empty body */ }
        const game_id = body.game_id;
        if (!game_id) throw new Error('game_id is required');

        if (body.packed) {
            const { data, error } = await supabaseClient
                .from('games').select('*').eq('id', game_id).single();
            if (!error && data) {
                // buildPackedGameBytes returns null for lobbies (a WAITING
                // game never loads from a blob — a stale one would serve the
                // finished session's state) and legacy blob-less rows; those
                // fall through to the JSON path below.
                const bytes = await buildPackedGameBytes(data, user.id);
                if (bytes) {
                    return new Response(bytes as unknown as BodyInit, {
                        headers: { ...corsHeaders, 'Content-Type': 'application/octet-stream' },
                    });
                }
            }
        }

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
