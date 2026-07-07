import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import { createClient } from 'jsr:@supabase/supabase-js';
import { corsHeaders, handleCors } from "../_shared/cors.ts";
import { getAuthenticatedUser } from "../_shared/auth.ts";
import { loadCompleteGame } from "../_shared/utils.ts";
import { personalize_game } from "../_shared/common_utils.ts";
import { encodeGamesList, GamesListEntry } from "../_shared/wire/view.ts";
import "jsr:@supabase/functions-js/edge-runtime.d.ts";

// Read-only list of the caller's games, personalized. Replaces the client's
// direct `player_hands`-join query (which read hands the server no longer
// writes during play). player_hands still doubles as the player<->game
// membership index — its rows are maintained at lobby lifecycle (create / join
// / exit) — so we use it only to find the caller's game_ids, then reconstruct
// each game's authoritative state from its packed blob via loadCompleteGame and
// return the caller's personalized view. Ordered most-recently-updated first.
const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || '',
);

serve(async (req: Request): Promise<Response> => {
    const cors = handleCors(req);
    if (cors) return cors;

    try {
        const user = await getAuthenticatedUser(req);

        // Membership + ordering only — no hand data is read from player_hands.
        const { data: rows, error } = await supabaseClient
            .from('player_hands')
            .select('game_id, games(updated_at)')
            .eq('player_id', user.id)
            .order('games(updated_at)', { ascending: false });
        if (error) throw new Error(error.message);

        const seen = new Set<string>();
        const gameIds = (rows ?? [])
            .map((r: any) => r.game_id)
            .filter((id: string) => (seen.has(id) ? false : (seen.add(id), true)));

        let body: any = {};
        try { body = await req.json(); } catch { /* empty body */ }

        // Packed list (docs/PACKED_WIRE_CUTOVER.md): each dealt game rides as
        // the caller's kernel-masked view blob; lobbies/legacy rows as
        // byte-wrapped personalize_game JSON. One binary response, decoded at
        // the client's render boundary.
        if (body.packed) {
            const { buildPackedGameBytes } = await import('../_shared/packed_game.ts');
            const entries: GamesListEntry[] = [];
            for (const id of gameIds) {
                try {
                    const { data: row, error: rowErr } = await supabaseClient
                        .from('games')
                        .select('id, name, status, version, state, players, good_players, good_timestamp')
                        .eq('id', id).single();
                    if (rowErr || !row) continue;
                    const packed = await buildPackedGameBytes(row, user.id);
                    if (packed) {
                        entries.push({ kind: 1, bytes: packed });
                    } else {
                        const json = personalize_game(await loadCompleteGame(id), user.id);
                        entries.push({ kind: 0, bytes: new TextEncoder().encode(JSON.stringify(json)) });
                    }
                } catch (e) {
                    console.error(`[get_my_games] skipping ${id}:`, (e as Error).message);
                }
            }
            return new Response(encodeGamesList(entries) as unknown as BodyInit, {
                headers: { ...corsHeaders, 'Content-Type': 'application/octet-stream' },
            });
        }

        // Legacy JSON list. Each game reconstructs from its blob; a game that
        // fails to load (mid teardown, etc.) is skipped rather than failing
        // the whole list.
        const games = [];
        for (const id of gameIds) {
            try {
                games.push(personalize_game(await loadCompleteGame(id), user.id));
            } catch (e) {
                console.error(`[get_my_games] skipping ${id}:`, (e as Error).message);
            }
        }

        return new Response(JSON.stringify({ games }), {
            headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    } catch (e: any) {
        return new Response(JSON.stringify({ error: e.message }), {
            status: 400, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    }
});
