// The Supabase host's shared shell: the service-role client, the batched
// realtime broadcast, the post-response bot loop, and wrap400 (CORS, auth, the
// JSON body, the error response). Everything here is I/O. The game itself is
// the C Table's, reached through table_io.ts.
import { corsHeaders, handleCors } from './cors.ts';
import { createClient } from 'jsr:@supabase/supabase-js';
import type { User } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import { getAuthenticatedUser } from './auth.ts';

// A lazy import that resolves ONCE. The bot loop's module graph (the bot drive)
// stays off every cold start that never drives a bot; and re-RESOLVING a
// specifier per call walks the resolver again under the e2e loader.
const lazy = <T>(load: () => Promise<T>): (() => Promise<T>) => {
    let mod: Promise<T> | undefined;
    return () => (mod ??= load());
};
const botActionsMod = lazy(() => import('./bot_actions.ts'));

export const supabaseClient = createClient(
    Deno.env.get('SUPABASE_URL') || '',
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || ''
);

// One batched REST broadcast for many topics in a single POST.
//
// This is exactly what realtime-js's httpSend() does internally - a POST of
// { messages: [{ topic, event, payload, private }] } to /realtime/v1/api/broadcast
// - generalized to many topics so ALL recipients of one game update go out in a
// SINGLE round-trip. The server never holds a websocket; clients keep receiving
// over their existing subscriptions. `topic` is the bare channel name; all our
// channels are private, hence private: true.
const REALTIME_BROADCAST_URL = `${Deno.env.get('SUPABASE_URL') || ''}/realtime/v1/api/broadcast`;
const REALTIME_BROADCAST_KEY = Deno.env.get('SUPABASE_SERVICE_ROLE_KEY') || '';

// deno-lint-ignore no-explicit-any
export interface BroadcastMessage { topic: string; event: string; payload: any; }

export const broadcastMessages = async (messages: BroadcastMessage[], reqId: string = 'unknown'): Promise<void> => {
    if (messages.length === 0) return;
    const start = Date.now();
    // One retry on failure: the whole path is fire-and-forget (a dropped
    // broadcast only surfaces as a missed animation until the next event's
    // versioned state supersedes it). Duplicate delivery is safe: clients dedup
    // by sequence id and drop stale versions.
    for (let attempt = 1; attempt <= 2; attempt++) {
        try {
            const response = await fetch(REALTIME_BROADCAST_URL, {
                method: 'POST',
                headers: {
                    apikey: REALTIME_BROADCAST_KEY,
                    Authorization: `Bearer ${REALTIME_BROADCAST_KEY}`,
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify({
                    messages: messages.map(m => ({ topic: m.topic, event: m.event, payload: m.payload, private: true })),
                }),
            });
            // Realtime returns 202 Accepted on success.
            if (response.status === 202) {
                await response.body?.cancel();
                break;
            }
            const text = await response.text().catch(() => response.statusText);
            console.error(`[${reqId}][BROADCAST] REST broadcast failed (attempt ${attempt}): ${response.status} ${text}`);
        } catch (err) {
            console.error(`[${reqId}][BROADCAST] REST broadcast error (attempt ${attempt}):`, err);
        }
    }
    console.log(`[${reqId}][BROADCAST] batched ${messages.length} message(s) in ${Date.now() - start}ms`);
};

// Fire-and-forget bot drive AFTER the HTTP response. CRITICAL: without
// EdgeRuntime.waitUntil the runtime reaps the isolate ~15s after the response -
// mid-loop - so the loop's `finally` never releases the bot lease. waitUntil
// keeps the worker alive until the loop settles.
export const scheduleBotLoop = (game_id: string, reqId: string): void => {
    const botLoop = botActionsMod()
        .then(m => m.lockedBotLoop(game_id))
        .catch(err => console.error(`[${reqId}] bot loop error:`, err));
    // deno-lint-ignore no-explicit-any
    const er = (globalThis as any).EdgeRuntime;
    if (er && typeof er.waitUntil === 'function') {
        er.waitUntil(botLoop);
    }
};

/** What wrap400 hands a JSON endpoint's handler. */
export interface RequestContext {
    user: User;
    /** The caller's display name (user_metadata.username). */
    userName: string;
    // deno-lint-ignore no-explicit-any
    body: any;
    reqId: string;
}

/** A handler's answer: the response body (a packed envelope) and the game whose bots to wake, if any. */
export interface HandlerResult {
    body: Uint8Array;
    runBots?: string | null;
}

export const wrap400 = (
    execute: (ctx: RequestContext) => Promise<HandlerResult>,
    // Packed-request escape hatch: when set and the request body is
    // application/octet-stream, the whole request is delegated here after CORS
    // and auth (the `action` function's binary path).
    binary: ((req: Request, user: User, reqId: string) => Promise<Response>) | null = null,
) => {
    const handler = async (req: Request): Promise<Response> => {
        const reqId = crypto.randomUUID().split('-')[0];
        const started = Date.now();
        try {
            const corsResponse = handleCors(req);
            if (corsResponse) return corsResponse;

            const user: User = await getAuthenticatedUser(req);

            if (binary && (req.headers.get('content-type') || '').includes('application/octet-stream')) {
                return await binary(req, user, reqId);
            }

            let body = {};
            try { body = await req.json(); } catch { /* an empty or non-JSON body reads as {} */ }

            const result = await execute({ user, userName: user.user_metadata.username, body, reqId });
            if (result.runBots) scheduleBotLoop(result.runBots, reqId);

            console.log(`[${reqId}][WRAP400] ${req.method} ${new URL(req.url).pathname} in ${Date.now() - started}ms`);
            return new Response(result.body as unknown as BodyInit, {
                headers: { ...corsHeaders, 'Content-Type': 'application/octet-stream' },
            });
        } catch (e: unknown) {
            const err = e as Error;
            console.error(`[${reqId}][WRAP400] error after ${Date.now() - started}ms:`, { name: err.name, message: err.message, stack: err.stack });
            return new Response(
                JSON.stringify({ error: err.message }),
                { status: 400, headers: { ...corsHeaders, 'Content-Type': 'application/json' } }
            );
        }
    };

    serve(handler);
    return handler;
};
