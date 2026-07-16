// Server-side bot driver — replaces the dependency on a browser tab's poll to keep
// games progressing. Invoked by a pg_cron job every few seconds (see
// migrations/20260616040000_bot_heartbeat_cron.sql). All the "which games need
// driving" logic lives HERE in TypeScript, not in SQL — the cron is a dumb trigger.
//
// Two modes (one function, so the cron only needs one URL):
//   SCAN  (no game_id, the cron): find stalled bot games in TS, then dispatch one
//         self-call per game. Each dispatch is a SEPARATE request => its own fresh
//         ~2s CPU budget (cordite is CPU-bound; sharing one budget across many games
//         would starve them).
//   DRIVE (game_id present): run one lockedBotLoop segment for that game.
//
// Auth: the function relies on the platform's JWT verification — the cron and the
// self-dispatch both present the service-role key, which passes. We never call
// getAuthenticatedUser (there is no user here).
import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import { createClient } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts";
import { lockedBotLoop } from '../_shared/adapter/bot_actions.ts';
import { corsHeaders } from '../_shared/adapter/cors.ts';

const SUPABASE_URL = Deno.env.get('SUPABASE_URL') || '';
const SERVICE_KEY = Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || '';
const SELF_URL = `${SUPABASE_URL}/functions/v1/bot-heartbeat`;

const supabaseClient = createClient(SUPABASE_URL, SERVICE_KEY);

// A game untouched for this long (no commit) might be stalled with bot work pending.
const STALE_MS = 10_000;
// Ignore games untouched for ages — those are abandoned, not stalled; don't keep
// bumping them forever.
const ABANDON_MS = 60 * 60 * 1000; // 1 hour
const MAX_GAMES = 100; // safety cap per scan

const json = (body: unknown, status = 200): Response =>
    new Response(JSON.stringify(body), {
        status, headers: { ...corsHeaders, 'Content-Type': 'application/json' },
    });

const runInBackground = (p: Promise<unknown>): void => {
    const er = (globalThis as any).EdgeRuntime;
    if (er && typeof er.waitUntil === 'function') er.waitUntil(p);
};

serve(async (req: Request): Promise<Response> => {
    if (req.method === 'OPTIONS') return new Response('ok', { headers: corsHeaders });

    let body: any = {};
    try { body = await req.json(); } catch { /* empty body == scan */ }

    // ---- DRIVE: advance one game (fresh request => fresh CPU budget) ----
    if (body && body.game_id) {
        runInBackground(
            lockedBotLoop(body.game_id).catch(e =>
                console.error(`[heartbeat] drive error for ${body.game_id}:`, e)),
        );
        return json({ ok: true, drove: body.game_id });
    }

    // ---- SCAN: find stalled bot games and dispatch a drive per game ----
    const now = Date.now();
    const { data, error } = await supabaseClient
        .from('games')
        .select('id, players, updated_at')
        .eq('status', 'playing')
        .lt('updated_at', new Date(now - STALE_MS).toISOString())
        .gt('updated_at', new Date(now - ABANDON_MS).toISOString())
        .limit(MAX_GAMES);

    if (error) {
        console.error('[heartbeat] scan query failed:', error);
        return json({ error: error.message }, 500);
    }

    // Needs driving iff at least one bot (is_ai) is still IN. This excludes
    // 100%-human games (no bots) and games where every bot is already OUT. Deleted
    // games simply aren't in the result set.
    const needsDriving = (data ?? []).filter((g: any) =>
        Array.isArray(g.players) &&
        g.players.some((p: any) => p.is_ai && p.status === 'in'));

    // Dispatch one fresh drive request per game.
    await Promise.all(needsDriving.map((g: any) =>
        fetch(SELF_URL, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Authorization': `Bearer ${SERVICE_KEY}`,
            },
            body: JSON.stringify({ game_id: g.id }),
        }).catch(e => console.error(`[heartbeat] dispatch failed for ${g.id}:`, e)),
    ));

    console.log(`[heartbeat] scanned=${data?.length ?? 0} dispatched=${needsDriving.length}`);
    return json({ scanned: data?.length ?? 0, dispatched: needsDriving.length });
});
