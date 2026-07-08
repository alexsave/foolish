import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import { corsHeaders, handleCors } from "../_shared/cors.ts";
import { getAuthenticatedUser } from "../_shared/auth.ts";
import { personalize_game } from "../_shared/common_utils.ts";
import { encodeGamesList, GamesListEntry } from "../_shared/wire/view.ts";
import { buildPackedGameBytes, lobbyGameFromRow } from "../_shared/packed_game.ts";
import { GAME_STATUS } from "../_shared/types.ts";
import "jsr:@supabase/functions-js/edge-runtime.d.ts";

// Read-only list of the caller's games, personalized. Replaces the client's
// direct `player_hands`-join query (which read hands the server no longer
// writes during play). player_hands still doubles as the player<->game
// membership index — its rows are maintained at lobby lifecycle (create / join
// / exit) — so we use it only to find the caller's game_ids, then reconstruct
// each game's authoritative state and return the caller's personalized view.
// Ordered most-recently-updated first.
//
// COLD-START NOTE: this function deliberately does NOT import
// `@supabase/supabase-js`. Auth verifies the JWT locally (native Web Crypto in
// ../_shared/auth.ts) and the two reads below are raw PostgREST fetches with the
// service-role key. That keeps the single heaviest module out of the boot graph
// so a cold isolate doesn't pay to evaluate it. loadCompleteGame (which does
// pull supabase-js) is imported lazily and only on the extinct legacy path.
const SUPABASE_URL = Deno.env.get('SUPABASE_URL') || '';
const SERVICE_KEY = Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || '';
const REST = `${SUPABASE_URL}/rest/v1`;
const restHeaders = { apikey: SERVICE_KEY, Authorization: `Bearer ${SERVICE_KEY}` };

// The public + service-only columns needed to serve a game either packed (dealt:
// from `state`) or as JSON (lobby: rebuilt from the roster/board columns).
const GAME_COLS =
    'id,name,status,version,state,players,good_players,good_timestamp,' +
    'discard_pile_length,flipped,power_suit,first_attacker,defender,table_battles,elimination_order';

async function restGet(pathAndQuery: string): Promise<any[]> {
    const res = await fetch(`${REST}/${pathAndQuery}`, { headers: restHeaders });
    if (!res.ok) throw new Error(`PostgREST ${res.status}: ${await res.text()}`);
    return await res.json();
}

serve(async (req: Request): Promise<Response> => {
    const cors = handleCors(req);
    if (cors) return cors;

    const T0 = performance.now();
    try {
        const user = await getAuthenticatedUser(req);
        const tAfterAuth = performance.now();

        // Membership + ordering only — no hand data is read from player_hands.
        // Dedup game_ids and order by the joined games.updated_at (ISO strings
        // sort lexicographically), most-recent first.
        const memRows = await restGet(
            `player_hands?select=game_id,games(updated_at)&player_id=eq.${encodeURIComponent(user.id)}`,
        );
        const updatedAt = new Map<string, string>();
        for (const r of memRows) {
            const u: string = r.games?.updated_at ?? '';
            const prev = updatedAt.get(r.game_id);
            if (prev === undefined || u > prev) updatedAt.set(r.game_id, u);
        }
        const gameIds = [...updatedAt.keys()].sort((a, b) => {
            const ua = updatedAt.get(a)!, ub = updatedAt.get(b)!;
            return ua < ub ? 1 : ua > ub ? -1 : 0; // descending
        });

        // One batched read of every game the caller belongs to (was an N+1
        // per-game .single() loop). Map by id so we can emit in updated_at order.
        const tAfterMembership = performance.now();
        const byId = new Map<string, any>();
        if (gameIds.length > 0) {
            const inList = gameIds.map(encodeURIComponent).join(',');
            const rows = await restGet(`games?select=${GAME_COLS}&id=in.(${inList})`);
            for (const row of rows) byId.set(row.id, row);
        }
        const tAfterBatch = performance.now();
        console.log(
            `[perf] get_my_games auth ${(tAfterAuth - T0).toFixed(0)}ms | ` +
            `membership ${(tAfterMembership - tAfterAuth).toFixed(0)}ms | ` +
            `batch(${gameIds.length}) ${(tAfterBatch - tAfterMembership).toFixed(0)}ms`,
        );

        let body: any = {};
        try { body = await req.json(); } catch { /* empty body */ }

        // Packed list (docs/PACKED_WIRE_CUTOVER.md): each dealt game rides as
        // the caller's kernel-masked view blob; lobbies as byte-wrapped
        // personalize_game JSON. One binary response, decoded at the client's
        // render boundary.
        if (body.packed) {
            const entries: GamesListEntry[] = [];
            for (const id of gameIds) {
                const row = byId.get(id);
                if (!row) continue;
                try {
                    const packed = await buildPackedGameBytes(row, user.id);
                    if (packed) {
                        entries.push({ kind: 1, bytes: packed });
                    } else if (row.status === GAME_STATUS.WAITING) {
                        const json = personalize_game(lobbyGameFromRow(row), user.id);
                        entries.push({ kind: 0, bytes: new TextEncoder().encode(JSON.stringify(json)) });
                    } else {
                        // Extinct legacy: non-waiting row with no blob. Needs the
                        // real hands → the supabase-js path, imported lazily so
                        // the common list never pays for it.
                        const { loadCompleteGame } = await import('../_shared/utils.ts');
                        const json = personalize_game(await loadCompleteGame(id), user.id);
                        entries.push({ kind: 0, bytes: new TextEncoder().encode(JSON.stringify(json)) });
                    }
                } catch (e) {
                    console.error(`[get_my_games] skipping ${id}:`, (e as Error).message);
                }
            }
            console.log(
                `[perf] get_my_games build(${entries.length}) ${(performance.now() - tAfterBatch).toFixed(0)}ms | ` +
                `total ${(performance.now() - T0).toFixed(0)}ms`,
            );
            return new Response(encodeGamesList(entries) as unknown as BodyInit, {
                headers: { ...corsHeaders, 'Content-Type': 'application/octet-stream' },
            });
        }

        // Legacy JSON list (no packed client). Compat-only — the shipping client
        // always sends packed:true — so it stays on the simple lazy loadCompleteGame
        // path; a game that fails to load (mid teardown, etc.) is skipped.
        const games: any[] = [];
        if (gameIds.length > 0) {
            const { loadCompleteGame } = await import('../_shared/utils.ts');
            for (const id of gameIds) {
                try {
                    games.push(personalize_game(await loadCompleteGame(id), user.id));
                } catch (e) {
                    console.error(`[get_my_games] skipping ${id}:`, (e as Error).message);
                }
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
