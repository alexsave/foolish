// The Supabase host's shared shell: the service-role client, the batched
// realtime broadcast, the post-response bot loop, and wrap400 (CORS, auth, the
// request's bytes, the refusal). Everything here is I/O. The game itself is
// the C Table's, reached through table_io.ts, and every body in or out is
// bytes the kernel reads or wrote.
import { corsHeaders, handleCors } from './cors.ts';
import { createClient } from 'jsr:@supabase/supabase-js';
import type { User } from 'jsr:@supabase/supabase-js';
import "jsr:@supabase/functions-js/edge-runtime.d.ts"
import { serve } from "https://deno.land/std@0.168.0/http/server.ts"
import * as L from '@sdk/ts/gen/game_layout.bots.ts';
import { serverTable } from '@sdk/ts/table/server_table.ts';
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

/** One realtime message: `cast` is the kernel's bytes (evwire.h, the cast: the version and the as3 push) as base64. */
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
                // The one JSON in the server, and it is Supabase's: the broadcast
                // endpoint accepts nothing else, and a `payload` must be an
                // object. Ours is one key holding the cast, base64.
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
    const botLoop = botActionsMod()
        .then(m => m.lockedBotLoop(game_id))
        .catch(err => console.error(`[${reqId}] bot loop error:`, err));
    // deno-lint-ignore no-explicit-any
    const er = (globalThis as any).EdgeRuntime;
    if (er && typeof er.waitUntil === 'function') {
        er.waitUntil(botLoop);
    }
};

// ---- refusals ----------------------------------------------------------------

const OCTETS = 'application/octet-stream';

/** A request the endpoint cannot read: not bytes, or bytes the kernel refuses. */
export class MalformedRequest extends Error {
    constructor(message = 'malformed request') { super(message); this.name = 'MalformedRequest'; }
}

/** add-bot: no bot row is free to seat, or the one asked for is not. */
export class NoBotAvailable extends Error {
    constructor(message: string) { super(message); this.name = 'NoBotAvailable'; }
}

/** The caller did not authenticate. */
export class Unauthenticated extends Error {
    constructor(message: string) { super(message); this.name = 'Unauthenticated'; }
}

/** getAuthenticatedUser, with its failure typed so the refusal can say ASK_WHY_AUTH. */
export async function authenticate(req: Request): Promise<User> {
    try {
        return await getAuthenticatedUser(req);
    } catch (e) {
        throw new Unauthenticated((e as Error).message);
    }
}

/** A reply that has no answer: the HTTP status, and a body the kernel wrote (ask.h AskRefusal). */
export async function refusalResponse(why: number, status: number, code = 0, detail = 0): Promise<Response> {
    let body: Uint8Array;
    try {
        body = (await serverTable()).refusal(why, code, detail);
    } catch (e) {
        // The kernel itself did not load: the status still says it failed.
        console.error('[REFUSAL] the kernel could not write the refusal:', e);
        body = new Uint8Array(0);
    }
    return new Response(body as unknown as BodyInit, {
        status, headers: { ...corsHeaders, 'Content-Type': OCTETS },
    });
}

// By name, so this module need not import the ones that define the errors.
const WHY_BY_ERROR = new Map<string | undefined, number>([
    ['Unauthenticated', L.ASK_WHY_AUTH],
    ['MalformedRequest', L.ASK_WHY_MALFORMED],
    ['MalformedActionRequest', L.ASK_WHY_MALFORMED],
    ['GameNotFound', L.ASK_WHY_NOT_FOUND],
    ['NoBotAvailable', L.ASK_WHY_NO_BOT],
]);

/** The refusal for whatever a handler threw: a 400 for what the caller got wrong or the kernel refused (table_io.ts TableRefusal), a 500 for what the server broke. */
export function refusalFor(e: unknown): Promise<Response> {
    const r = e as { name?: string; code?: unknown; detail?: unknown };
    if (r?.name === 'TableRefusal' && typeof r.code === 'number') {
        return refusalResponse(L.ASK_WHY_REFUSED, 400, r.code, typeof r.detail === 'number' ? r.detail : 0);
    }
    const why = WHY_BY_ERROR.get(r?.name);
    return why === undefined ? refusalResponse(L.ASK_WHY_INTERNAL, 500) : refusalResponse(why, 400);
}

/**
 * The request's body as bytes. A body that does not say it is bytes gets a 415
 * before it is read: a client that still sends application/json must not have
 * its JSON handed to the kernel as a malformed ask.
 */
export async function requestBytes(req: Request): Promise<Uint8Array | Response> {
    if (!(req.headers.get('content-type') || '').includes(OCTETS)) {
        return refusalResponse(L.ASK_WHY_MALFORMED, 415);
    }
    return new Uint8Array(await req.arrayBuffer());
}

// ---- the request shell -----------------------------------------------------------

/** What wrap400 hands an endpoint's handler. */
export interface RequestContext {
    user: User;
    /** The caller's display name (user_metadata.username). */
    userName: string;
    /** The request body: bytes the kernel reads (an ask, or an action request). */
    body: Uint8Array;
    reqId: string;
}

/** A handler's answer: the response body (bytes the kernel wrote) and the game whose bots to wake, if any. */
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

            const user: User = await authenticate(req);

            const body = await requestBytes(req);
            if (body instanceof Response) return body;

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
