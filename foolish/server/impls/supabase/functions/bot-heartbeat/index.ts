// Server-side bot driver - replaces the dependency on a browser tab's poll to keep
// games progressing. Invoked by a pg_cron job every few seconds (see
// seed.sql's SCHEDULED JOBS section). Which games need driving is
// the kernel's verdict, stored as games.needs_bots by every commit (table.h
// table_needs_bots: PLAYING, and a bot seat still IN); the scan only adds the
// staleness window.
//
// The cron tick is gated (the same section): it posts only when some
// row the kernel says needs bots has committed within ABANDON_MS, so an idle
// database costs nothing. The gate is strictly weaker than the filter below -
// it does not know about STALE_MS - so every tick this function would have
// dispatched on still arrives, and the cadence while a game is live is the
// unchanged 10 seconds.
//
// Two modes (one function, so the cron only needs one URL):
//   SCAN  (no `game` in the URL, the cron): find stalled bot games, then dispatch
//         one self-call per game. Each dispatch is a SEPARATE request => its own
//         fresh ~2s CPU budget (cordite is CPU-bound; sharing one budget across
//         many games would starve them).
//   DRIVE (`?game=<id>`): run one lockedBotLoop segment for that game.
//
// THE REQUEST BODY IS NEVER READ, and neither answer has one. The mode is the
// URL's: the one thing a drive must be told is a game id, and an id is a URL's
// to carry. The cron still POSTs `{}` (seed.sql): pg_net's http_post takes a
// jsonb body and nothing else, so those two bytes are pg_net's, and this
// function does not look at them. A scan's counts go to the log; nothing ever
// read them from the response but pg_net's own response table.
//
// Auth: the function relies on the platform's JWT verification - the cron and the
// self-dispatch both present the service-role key, which passes. We never call
// getAuthenticatedUser (there is no user here).
import { serve } from "https://deno.land/std@0.168.0/http/server.ts";
import { createClient } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts";
import { lockedBotLoop } from '@shared/adapter/bot_actions.ts';
import { corsHeaders } from '@shared/adapter/cors.ts';

const SUPABASE_URL = Deno.env.get('SUPABASE_URL') || '';
const SERVICE_KEY = Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || '';
const SELF_URL = `${SUPABASE_URL}/functions/v1/bot-heartbeat`;

const supabaseClient = createClient(SUPABASE_URL, SERVICE_KEY);

// A game untouched for this long might be stalled with bot work pending. Bounded
// on updated_at, which is every write to the row: a drive in flight has just
// taken the lease, so the row reads fresh and a second drive is not dispatched
// on top of it.
const STALE_MS = 10_000;
// Ignore games nobody has MOVED in for ages - those are abandoned, not stalled;
// don't keep bumping them forever. Bounded on last_commit_at, which moves only
// when `version` moves, i.e. only on a kernel commit (migration
// 20260918230000). It cannot be updated_at: the drive this scan dispatches
// takes and releases the bot lease, both of which UPDATE games, and
// update_games_updated_at stamps updated_at on any update - so the heartbeat
// refreshed its own guard and the guard never fired. Hosted game 24a407 was
// still being driven 67 days after its last move.
//
// The cron command carries this same window as its gate (interval '1 hour'), so
// an idle tick never reaches this function at all; the two are held equal by
// e2e/heartbeat_gate.test.ts.
const ABANDON_MS = 60 * 60 * 1000; // 1 hour
const MAX_GAMES = 100; // safety cap per scan

/** A reply with no body: the status is the whole answer. */
const reply = (status: number): Response => new Response(null, { status, headers: corsHeaders });

/** The URL that drives one game: this function again, the game in the query. */
const driveUrl = (gameId: string): string => `${SELF_URL}?game=${encodeURIComponent(gameId)}`;

const runInBackground = (p: Promise<unknown>): void => {
    // deno-lint-ignore no-explicit-any
    const er = (globalThis as any).EdgeRuntime;
    if (er && typeof er.waitUntil === 'function') er.waitUntil(p);
};

serve(async (req: Request): Promise<Response> => {
    if (req.method === 'OPTIONS') return new Response('ok', { headers: corsHeaders });

    // ---- DRIVE: advance one game (fresh request => fresh CPU budget) ----
    const gameId = new URL(req.url).searchParams.get('game');
    if (gameId) {
        runInBackground(
            lockedBotLoop(gameId).catch(e =>
                console.error(`[heartbeat] drive error for ${gameId}:`, e)),
        );
        return reply(202);
    }

    // ---- SCAN: the stalled games the kernel says have bot work ----
    // The partial index idx_games_bot_scan covers the needs_bots + updated_at
    // range; last_commit_at filters the abandoned ones out of it.
    const now = Date.now();
    const { data, error } = await supabaseClient
        .from('games')
        .select('id')
        .eq('needs_bots', true)
        .lt('updated_at', new Date(now - STALE_MS).toISOString())
        .gt('last_commit_at', new Date(now - ABANDON_MS).toISOString())
        .limit(MAX_GAMES);

    if (error) {
        console.error('[heartbeat] scan query failed:', error);
        return reply(500);
    }
    const ids = (data ?? []).map((g: { id: string }) => g.id);

    // One fresh drive request per game.
    await Promise.all(ids.map((id: string) =>
        fetch(driveUrl(id), {
            method: 'POST',
            headers: { 'Authorization': `Bearer ${SERVICE_KEY}` },
        }).catch(e => console.error(`[heartbeat] dispatch failed for ${id}:`, e)),
    ));

    console.log(`[heartbeat] dispatched=${ids.length}`);
    return reply(204);
});
