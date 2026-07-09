import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import { corsHeaders, handleCors } from "../_shared/cors.ts";
import { getAuthenticatedUser } from "../_shared/auth.ts";
import { loadCompleteGame, supabaseClient } from "../_shared/utils.ts";
import { personalize_game } from "../_shared/common_utils.ts";
import { buildPackedGameBytes, gameViewFromRow } from "../_shared/packed_game.ts";
import { buildPlayerViewUpserts } from "../_shared/player_views.ts";
import "jsr:@supabase/functions-js/edge-runtime.d.ts";

// Cache-warm / BACKFILL (docs/PLAYER_VIEWS.md): when a game predating the cache
// is fetched, populate its player_views rows for ALL human participants (the
// SAME builder commit_game uses → byte-identical), so the next open reads the
// cache directly instead of hitting this function — and so one player's (or a
// spectator's) fetch backfills the whole game for everyone. Fill-if-absent
// (ignoreDuplicates): commit_game owns UPDATES under the version fence; this
// read-path write is not fenced, so it may only INSERT a missing row, never
// overwrite a newer one. Fire-and-forget so it never adds latency to the
// response it makes obsolete. (Temporary backfill — removed with get_my_games.)
function warmGameViews(data: any): void {
    const p = (async () => {
        const rows = await buildPlayerViewUpserts(gameViewFromRow(data), data.state ?? null, Number(data.version ?? 0));
        if (rows.length === 0) return;
        const { error } = await supabaseClient
            .from('player_views')
            .upsert(rows, { onConflict: 'game_id,player_id', ignoreDuplicates: true });
        if (error) console.error('[get_game] warm write failed:', error);
    })().catch((e: unknown) => console.error('[get_game] warm error:', e));
    const er = (globalThis as any).EdgeRuntime;
    if (er && typeof er.waitUntil === 'function') er.waitUntil(p);
}

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
            // Only what the packed view needs — games.logs_packed grows all
            // session and must never ride along on a state fetch.
            const { data, error } = await supabaseClient
                .from('games')
                .select('id, name, status, version, state, players, good_players, good_timestamp')
                .eq('id', game_id).single();
            if (!error && data) {
                // buildPackedGameBytes returns null for lobbies (a WAITING
                // game never loads from a blob — a stale one would serve the
                // finished session's state) and legacy blob-less rows; those
                // fall through to the JSON path below.
                const bytes = await buildPackedGameBytes(data, user.id);
                if (bytes) {
                    // Backfill rows for ALL participants of this (dealt) game.
                    warmGameViews(data);
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
