// The Supabase host's shared shell: the service-role client, the batched
// realtime broadcast, the post-response bot loop, and wrap400 (CORS, auth, the
// request's bytes, the refusal). Everything here is I/O; every body in or out
// is bytes the kernel reads or wrote.
import { corsHeaders, handleCors } from './cors.ts';
import { createClient } from 'jsr:@supabase/supabase-js';
import type { User } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import * as L from '@sdk/ts/gen/game_layout.bots.ts';
import { serverTable } from '@sdk/ts/table/server_table.ts';
import { getAuthenticatedUser } from './auth.ts';

const OCTETS = 'application/octet-stream';

// A lazy import that resolves ONCE. The bot loop's module graph (the bot drive)
// stays off every cold start that never drives a bot; and re-RESOLVING a
// specifier per call walks the resolver again under the e2e loader.
let botActionsMod: Promise<typeof import('./bot_actions.ts')> | undefined;

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

/** One realtime message: `cast` is the kernel's cast (evwire.h), base64. */
export interface BroadcastMessage { topic: string; event: string; cast: string; }

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
                // Supabase's envelope, not ours: a `payload` must be a JSON object.
                body: JSON.stringify({
                    messages: messages.map(m => ({ topic: m.topic, event: m.event, payload: { b: m.cast }, private: true })),
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
    const botLoop = (botActionsMod ??= import('./bot_actions.ts'))
        .then(m => m.lockedBotLoop(game_id))
        .catch(err => console.error(`[${reqId}] bot loop error:`, err));
    // deno-lint-ignore no-explicit-any
    const er = (globalThis as any).EdgeRuntime;
    if (er && typeof er.waitUntil === 'function') {
        er.waitUntil(botLoop);
    }
};

/** An error whose refusal says `why` (an ASK_WHY_*). One thrown without a `why` is the server's own fault. */
export const refused = (why: number, message: string): Error => Object.assign(new Error(message), { why });

/** The reply for whatever was thrown: a body the kernel wrote (ask.h AskRefusal) from the error's `why`, `code` and `detail`. */
export async function refusalFor(e: unknown, status?: number): Promise<Response> {
    const r = (e ?? {}) as { why?: number; code?: number; detail?: number };
    const table = await serverTable();
    const body = r.why === undefined ? table.refusal(L.ASK_WHY_INTERNAL) : table.refusal(r.why, r.code ?? 0, r.detail ?? 0);
    return new Response(body as unknown as BodyInit, {
        status: status ?? (r.why === undefined ? 500 : 400), headers: { ...corsHeaders, 'Content-Type': OCTETS },
    });
}

/** What wrap400 hands an endpoint's handler. */
export interface RequestContext {
    user: User;
    /** The caller's display name (user_metadata.username). */
    userName: string;
    body: Uint8Array;
    reqId: string;
}

/** A handler's answer: the response body and the game whose bots to wake, if any. */
export interface HandlerResult {
    body: Uint8Array;
    runBots?: string | null;
}

export const wrap400 = (execute: (ctx: RequestContext) => Promise<HandlerResult>) => {
    const handler = async (req: Request): Promise<Response> => {
        const reqId = crypto.randomUUID().split('-')[0];
        const started = Date.now();
        try {
            const corsResponse = handleCors(req);
            if (corsResponse) return corsResponse;

            const user: User = await getAuthenticatedUser(req);
            // A client that still sends JSON must not have it read as an ask.
            if (!(req.headers.get('content-type') || '').includes(OCTETS)) return refusalFor(refused(L.ASK_WHY_MALFORMED, ''), 415);
            const body = new Uint8Array(await req.arrayBuffer());
            const result = await execute({ user, userName: user.user_metadata.username, body, reqId });
            if (result.runBots) scheduleBotLoop(result.runBots, reqId);

            console.log(`[${reqId}][WRAP400] ${req.method} ${new URL(req.url).pathname} in ${Date.now() - started}ms`);
            return new Response(result.body as unknown as BodyInit, {
                headers: { ...corsHeaders, 'Content-Type': OCTETS },
            });
        } catch (e: unknown) {
            const err = e as Error;
            console.error(`[${reqId}][WRAP400] error after ${Date.now() - started}ms:`, { name: err.name, message: err.message, stack: err.stack });
            return refusalFor(e);
        }
    };

    serve(handler);
    return handler;
};
