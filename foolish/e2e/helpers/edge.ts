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

export interface EdgeResponse { status: number; type: string; bytes: Uint8Array; json: any }

async function read(res: Response): Promise<EdgeResponse> {
    const type = res.headers.get('content-type') ?? '';
    const bytes = new Uint8Array(await res.arrayBuffer());
    let json: any = null;
    if (type.includes('application/json')) {
        try { json = JSON.parse(new TextDecoder().decode(bytes)); } catch { /* not json */ }
    }
    return { status: res.status, type, bytes, json };
}

const URL_BASE = 'http://edge.local/functions/v1';

/** POST a JSON body to an edge function as `token` (null = no Authorization). */
export async function postJson(name: EdgeName, token: string | null, body: unknown): Promise<EdgeResponse> {
    const h = await edge(name);
    const headers: Record<string, string> = { 'content-type': 'application/json' };
    if (token) headers.authorization = `Bearer ${token}`;
    return read(await h(new Request(`${URL_BASE}/${name}`, { method: 'POST', headers, body: JSON.stringify(body) })));
}

/** POST a packed binary body to the action function as `token`. */
export async function postPacked(token: string | null, body: Uint8Array): Promise<EdgeResponse> {
    const h = await edge('action');
    const headers: Record<string, string> = { 'content-type': 'application/octet-stream' };
    if (token) headers.authorization = `Bearer ${token}`;
    return read(await h(new Request(`${URL_BASE}/action`, { method: 'POST', headers, body: body as unknown as BodyInit })));
}
