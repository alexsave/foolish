// Drive the REAL edge entry points (server/impls/supabase/functions/<name>/index.ts)
// the way the platform does: a Request with a signed bearer token in, a Response
// out. Nothing between the Request and the handlers is test code - the auth is
// the real getAuthenticatedUser/verifyJwtLocal (against a JWKS injected through
// its own test hook instead of a network fetch), the body parsing is wrap400's,
// the dispatch is the index.ts file's.
//
// Three pieces of the platform are stood in for, each as narrowly as possible:
//   - serve(): the deno std server is e2e/adapters/server.ts, which records the
//     handler an index.ts registers at import time instead of listening.
//   - EdgeRuntime.waitUntil: the post-response work (create's persist, the bot
//     loop) is COLLECTED, so a test can `settle()` and see a finished world
//     instead of racing it.
//   - bot pacing: the bot loop sleeps until the kernel says a bot may act -
//     the last commit's animation on its viewers' screens, then the pace
//     (table_bot_wait_ms, c/src/bot_drive.h bot_wait_ms). A sleep in that band
//     resolves on the next tick instead AND moves Date.now on to the moment it
//     was due, because the kernel's wait is a deadline on the clock, not a
//     duration: a skipped sleep that left the clock behind would be asked the
//     same wait again. The sequence of cycles is unchanged. The band stops
//     below pg's 10s idle timer so the pool is not touched.
import './../harness.ts';
import { servedHandlers, ServedHandler } from '../adapters/server.ts';
import { __setJwksForTest } from '../../server/impls/supabase/functions/_shared/adapter/auth.ts';
import { clientTable, type AskRefusal } from '../../sdk/ts/table/client_table.ts';
import { askBody } from './ask.ts';
import { refusalMessage } from '../../server/impls/supabase/functions/_shared/adapter/table_io.ts';
import * as V from '../../sdk/ts/gen/view_layout.bots.ts';

// ---- post-response work ----------------------------------------------------
const pending: Promise<unknown>[] = [];
const er = (globalThis as { EdgeRuntime?: { waitUntil?: (p: Promise<unknown>) => void } }).EdgeRuntime!;
er.waitUntil = (p: Promise<unknown>) => { pending.push(p); };

/** Wait for every waitUntil promise (and any it scheduled) to settle. */
export async function settle(): Promise<void> {
    while (pending.length > 0) {
        const batch = pending.splice(0, pending.length);
        await Promise.allSettled(batch);
    }
}

// ---- bot pacing --------------------------------------------------------------
const realSetTimeout = globalThis.setTimeout;
const realDateNow = Date.now;
let skippedMs = 0;
Date.now = () => realDateNow() + skippedMs;
(globalThis as { setTimeout: unknown }).setTimeout = ((fn: (...a: unknown[]) => void, ms?: number, ...args: unknown[]) => {
    const wait = ms ?? 0;
    if (wait < 250 || wait > 9000) return realSetTimeout(fn, ms, ...args);
    const due = Date.now() + wait;
    return realSetTimeout(() => {
        const now = Date.now();
        if (due > now) skippedMs += due - now;
        fn(...args);
    }, 0);
}) as unknown as typeof setTimeout;

// ---- auth ------------------------------------------------------------------
const enc = new TextEncoder();
const b64url = (bytes: Uint8Array): string => {
    let bin = '';
    for (const b of bytes) bin += String.fromCharCode(b);
    return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
};

let signingKey: CryptoKey | null = null;
const KID = 'e2e-edge';

async function key(): Promise<CryptoKey> {
    if (signingKey) return signingKey;
    const kp = await crypto.subtle.generateKey({ name: 'ECDSA', namedCurve: 'P-256' }, true, ['sign', 'verify']);
    const jwk = await crypto.subtle.exportKey('jwk', kp.publicKey) as JsonWebKey & { kid?: string };
    jwk.kid = KID;
    __setJwksForTest({ keys: [jwk as never] });
    signingKey = kp.privateKey;
    return signingKey;
}

/** A bearer token the real verifier accepts for `sub` = userId. */
export async function tokenFor(userId: string, username = `u-${userId.slice(0, 4)}`): Promise<string> {
    const k = await key();
    const h = b64url(enc.encode(JSON.stringify({ alg: 'ES256', kid: KID, typ: 'JWT' })));
    const p = b64url(enc.encode(JSON.stringify({
        sub: userId, aud: 'authenticated', role: 'authenticated', user_metadata: { username },
    })));
    const sig = new Uint8Array(await crypto.subtle.sign({ name: 'ECDSA', hash: 'SHA-256' }, k, enc.encode(`${h}.${p}`)));
    return `${h}.${p}.${b64url(sig)}`;
}

// ---- handlers ----------------------------------------------------------------
const handlers = new Map<string, ServedHandler>();

/** The handler functions/<name>/index.ts registers with serve(). */
export type EdgeName = 'action' | 'meta' | 'create' | 'bot-heartbeat' | 'delete-account';

export async function edge(name: EdgeName): Promise<ServedHandler> {
    const got = handlers.get(name);
    if (got) return got;
    const before = servedHandlers.length;
    await import(`../../server/impls/supabase/functions/${name}/index.ts`);
    if (servedHandlers.length !== before + 1) {
        throw new Error(`edge(${name}): expected index.ts to serve exactly one handler, saw ${servedHandlers.length - before}`);
    }
    const h = servedHandlers[before];
    handlers.set(name, h);
    return h;
}

/**
 * `refusal`: the body of a non-2xx answer as the kernel reads it (c/src/ask.h
 * AskRefusal), or null when it is not one. `reason`: that refusal in words, for
 * a suite that matches loosely on why - the wire carries codes and no prose, so
 * the words are this helper's, and for a kernel refusal they are the server's
 * own log sentence (table_io.ts refusalMessage).
 */
export interface EdgeResponse { status: number; type: string; bytes: Uint8Array; json: any; refusal: AskRefusal | null; reason: string }

function reasonOf(r: AskRefusal | null): string {
    if (!r) return '';
    switch (r.why) {
        case V.ASK_WHY_AUTH: return 'Invalid token, or no authorization header';
        case V.ASK_WHY_MALFORMED: return 'Malformed request';
        case V.ASK_WHY_REFUSED: return refusalMessage(r.code, r.detail, '', '');
        case V.ASK_WHY_NOT_FOUND: return 'Game not found';
        case V.ASK_WHY_NO_BOT: return 'Bot not available';
        case V.ASK_WHY_INTERNAL: return 'Internal error';
        case V.ASK_WHY_METHOD: return 'Method not allowed';
        default: return `why ${r.why}`;
    }
}

async function read(res: Response): Promise<EdgeResponse> {
    const type = res.headers.get('content-type') ?? '';
    const bytes = new Uint8Array(await res.arrayBuffer());
    let json: any = null;
    if (type.includes('application/json')) {
        try { json = JSON.parse(new TextDecoder().decode(bytes)); } catch { /* not json */ }
    }
    const refusal = res.status >= 400 && bytes.length > 0 ? clientTable().readRefusal(bytes) : null;
    return { status: res.status, type, bytes, json, refusal, reason: reasonOf(refusal) };
}

const URL_BASE = 'http://edge.local/functions/v1';

/**
 * POST a JSON body to an edge function as `token` (null = no Authorization).
 * No endpoint reads JSON any more: this is what a client built before the
 * requests left JSON still sends, for the suites that hold what it gets back.
 */
export async function postJson(name: EdgeName, token: string | null, body: unknown): Promise<EdgeResponse> {
    const h = await edge(name);
    const headers: Record<string, string> = { 'content-type': 'application/json' };
    if (token) headers.authorization = `Bearer ${token}`;
    return read(await h(new Request(`${URL_BASE}/${name}`, { method: 'POST', headers, body: JSON.stringify(body) })));
}

/** POST bytes to an edge function as `token`: an ask to `meta`, an action request to `action`. */
export async function postBytes(name: EdgeName, token: string | null, body: Uint8Array): Promise<EdgeResponse> {
    const h = await edge(name);
    const headers: Record<string, string> = { 'content-type': 'application/octet-stream' };
    if (token) headers.authorization = `Bearer ${token}`;
    return read(await h(new Request(`${URL_BASE}/${name}`, { method: 'POST', headers, body: body as unknown as BodyInit })));
}

/** POST a request that is not a move, stated with the old body's names (helpers/ask.ts), as the ask a client sends to `meta`. */
export const postAsk = (token: string | null, body: Record<string, unknown>): Promise<EdgeResponse> =>
    postBytes('meta', token, askBody(body));

/** POST with no body at all (create, delete-account, the heartbeat). `query`: the URL's, e.g. '?game=abc'. */
export async function postEmpty(name: EdgeName, token: string | null, query = ''): Promise<EdgeResponse> {
    const h = await edge(name);
    const headers: Record<string, string> = {};
    if (token) headers.authorization = `Bearer ${token}`;
    return read(await h(new Request(`${URL_BASE}/${name}${query}`, { method: 'POST', headers })));
}

/** POST a packed binary body to the action function as `token`. */
export const postPacked = (token: string | null, body: Uint8Array): Promise<EdgeResponse> => postBytes('action', token, body);
