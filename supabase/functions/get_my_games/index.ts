import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import { corsHeaders, handleCors } from "../_shared/cors.ts";
import { getAuthenticatedUser, unverifiedSubFromToken } from "../_shared/auth.ts";
import { personalize_game } from "../_shared/common_utils.ts";
import { encodeGamesList, GamesListEntry } from "../_shared/wire/view.ts";
import { buildPackedGameBytes, gameViewFromRow } from "../_shared/packed_game.ts";
import "jsr:@supabase/functions-js/edge-runtime.d.ts";

// Read-only list of the caller's games, personalized. player_hands is the
// player<->game membership index (maintained at lobby lifecycle: create / join /
// exit), so we read the caller's games straight off it in ONE embedded query and
// return each game's personalized view. Ordered most-recently-updated first.
//
// PERF: this function is engineered to be O(1) work per game with no per-game
// round-trips and no supabase-js:
//   - auth verifies the JWT locally (native, ../_shared/auth.ts),
//   - ONE PostgREST read pulls every game via the player_hands embed,
//   - a dealt game becomes its kernel-masked view blob (pure wasm), and any
//     non-dealt game (waiting lobby, or finished/legacy with no blob) is built
//     from the row itself (gameViewFromRow) — NOT loadCompleteGame, which would
//     re-import supabase-js and do per-game DB reads (a build-loop N+1).
// loadCompleteGame is only touched on the dead legacy-JSON path below.
const SUPABASE_URL = Deno.env.get('SUPABASE_URL') || '';
const SERVICE_KEY = Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || '';
const REST = `${SUPABASE_URL}/rest/v1`;
const restHeaders = { apikey: SERVICE_KEY, Authorization: `Bearer ${SERVICE_KEY}` };

// The public + service-only columns needed to serve a game either packed (dealt:
// from `state`) or from the row (lobby/finished: roster + board columns).
const GAME_COLS =
    'id,name,status,version,state,players,good_players,good_timestamp,updated_at,' +
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
        // Verify the JWT signature and fetch the caller's games CONCURRENTLY.
        // They're two independent network round-trips (JWKS verify + PostgREST)
        // that only need the subject — which the token already carries — so
        // running them in series (as before) doubled the latency floor. We fire
        // the query on the CLAIMED (unverified) sub, but return NOTHING until the
        // signature verifies: a forged token wastes one query, never leaks data.
        const token = (req.headers.get('Authorization') || '').replace('Bearer ', '');
        const claimedSub = unverifiedSubFromToken(token);
        if (!claimedSub) throw new Error('Invalid token');

        const authP = getAuthenticatedUser(req);
        const fetchP = restGet(`player_hands?select=games(${GAME_COLS})&player_id=eq.${encodeURIComponent(claimedSub)}`);
        const [authR, fetchR] = await Promise.allSettled([authP, fetchP]);

        if (authR.status === 'rejected') throw authR.reason; // auth failed → the fetched rows are discarded
        const user = authR.value;
        if (fetchR.status === 'rejected') throw fetchR.reason;
        let rows = fetchR.value;
        // Paranoia: the verified subject must match what we queried. For a valid
        // token it always does (same bytes), but never serve another user's rows.
        if (user.id !== claimedSub) {
            rows = await restGet(`player_hands?select=games(${GAME_COLS})&player_id=eq.${encodeURIComponent(user.id)}`);
        }
        const seen = new Set<string>();
        const games: any[] = [];
        for (const r of rows) {
            const g = r.games;
            if (!g || seen.has(g.id)) continue;
            seen.add(g.id);
            games.push(g);
        }
        games.sort((a, b) => {
            const ua = a.updated_at ?? '', ub = b.updated_at ?? '';
            return ua < ub ? 1 : ua > ub ? -1 : 0; // descending
        });
        const tAfterFetch = performance.now();

        let body: any = {};
        try { body = await req.json(); } catch { /* empty body */ }

        // Packed list (docs/PACKED_WIRE_CUTOVER.md): each dealt game rides as the
        // caller's kernel-masked view blob; every other game as byte-wrapped
        // personalize_game JSON built from the row. One binary response.
        if (body.packed) {
            const entries: GamesListEntry[] = [];
            let nPacked = 0, nRow = 0;
            for (const row of games) {
                try {
                    const packed = await buildPackedGameBytes(row, user.id);
                    if (packed) {
                        nPacked++;
                        entries.push({ kind: 1, bytes: packed });
                    } else {
                        nRow++;
                        const json = personalize_game(gameViewFromRow(row), user.id);
                        entries.push({ kind: 0, bytes: new TextEncoder().encode(JSON.stringify(json)) });
                    }
                } catch (e) {
                    console.error(`[get_my_games] skipping ${row?.id}:`, (e as Error).message);
                }
            }
            const tEnd = performance.now();
            console.log(
                `[perf] get_my_games auth+fetch(${games.length}) ${(tAfterFetch - T0).toFixed(0)}ms overlapped | ` +
                `build ${(tEnd - tAfterFetch).toFixed(0)}ms [packed=${nPacked} row=${nRow}] | ` +
                `total ${(tEnd - T0).toFixed(0)}ms`,
            );
            return new Response(encodeGamesList(entries) as unknown as BodyInit, {
                headers: { ...corsHeaders, 'Content-Type': 'application/octet-stream' },
            });
        }

        // Legacy JSON list (no packed client). Compat-only — the shipping client
        // always sends packed:true — so it stays on the simple lazy loadCompleteGame
        // path; a game that fails to load (mid teardown, etc.) is skipped.
        const out: any[] = [];
        if (games.length > 0) {
            const { loadCompleteGame } = await import('../_shared/utils.ts');
            for (const g of games) {
                try {
                    out.push(personalize_game(await loadCompleteGame(g.id), user.id));
                } catch (e) {
                    console.error(`[get_my_games] skipping ${g?.id}:`, (e as Error).message);
                }
            }
        }

        return new Response(JSON.stringify({ games: out }), {
            headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    } catch (e: any) {
        return new Response(JSON.stringify({ error: e.message }), {
            status: 400, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
        });
    }
});
