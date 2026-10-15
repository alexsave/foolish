// live_page.ts - the deployed web page, played against the deployed server, on one clock.
//
// e2e/ui_animation_trace.test.ts mounts the real page tree in jsdom against a
// server that is the C Table in-process, answering when the case says so. That
// holds what the page DRAWS for a given order of events, and says nothing about
// which order the real server produces, or when. This helper closes that gap for
// the bugs that live in the timing between the two: the page (ServerProvider,
// AnimationProvider, RealtimeAnimationFeed, GameView) talks to the REAL edge
// entry points (functions/action and functions/meta index.ts, through
// e2e/helpers/edge.ts, JWT and all), which run the real CAS loop against this
// file's real Postgres and wake the real bot loop (scheduleBotLoop ->
// lockedBotLoop), whose realtime broadcasts are delivered to the page's own
// channel handler.
//
// ONE VIRTUAL CLOCK for both sides. setTimeout / setInterval / Date.now /
// performance.now / requestAnimationFrame are the helper's, so the bot loop's
// pacing sleep (table_bot_wait_ms) and the page's animation plan run on the
// same milliseconds, and a timeline is the same every run. What is NOT virtual is
// the I/O: a Postgres round trip happens in real time while the virtual clock
// stands still, and `quiesce` waits for every query and every request the page
// has in flight to finish before the clock moves again. So a request costs
// `latency.invokeMs` of virtual time (added on purpose, default 0) and nothing
// else, and a broadcast reaches the page `latency.realtimeMs` after the server
// sent it.
//
// Import this file FIRST in a test, before anything under src/: it installs the
// jsdom globals and the module mocks the page's imports resolve against.

import '../harness.ts';
import { mock } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { JSDOM } from 'jsdom';
import { broadcastLog, pgPool } from '../harness.ts';
import { postBytes, postEmpty, tokenFor } from './edge.ts';
import { askKindName } from './ask.ts';
import { castParts } from './cast.ts';
import { fakeBackend } from './fake_backend.ts';
import { netClient } from '../../sdk/ts/net/net_client.ts';
import { clientTable } from '../../sdk/ts/table/client_table.ts';
// The modules the server loads lazily, loaded now: a lazy import resolves in REAL
// time, and the virtual clock would otherwise move on while one is in flight.
import '../../server/impls/supabase/functions/_shared/adapter/bot_actions.ts';
import '../../server/impls/supabase/functions/_shared/adapter/finalize.ts';
import '../../server/impls/supabase/functions/_shared/adapter/packed_action.ts';
import { serverTable } from '../../sdk/ts/table/server_table.ts';

// The page and the server both narrate; a failure is an assertion, not a log line.
if (!process.env.E2E_VERBOSE) { console.error = () => {}; console.debug = () => {}; }

// ---- jsdom ---------------------------------------------------------------------------
const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>', { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
for (const k of ['window', 'document', 'HTMLElement', 'HTMLCanvasElement', 'Node', 'Element', 'MouseEvent', 'KeyboardEvent',
    'getComputedStyle', 'localStorage', 'sessionStorage', 'Image']) {
    try { g[k] = (dom.window as any)[k]; } catch { /* a getter already there */ }
}
try { Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true }); } catch { /* ok */ }
g.IS_REACT_ACT_ENVIRONMENT = true;
g.ResizeObserver ??= class { observe() {} unobserve() {} disconnect() {} };
dom.window.matchMedia ??= ((q: string) => ({ matches: false, media: q, addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {} })) as any;
g.matchMedia = dom.window.matchMedia;
(dom.window.HTMLCanvasElement.prototype as any).getContext = () => null;
// jsdom has no layout; a rect per element identity keeps flights well-formed (see ui_animation_trace.test.ts).
const identity = (el: Element): string => {
    const parts: string[] = [];
    for (let e: Element | null = el; e && e !== dom.window.document.body; e = e.parentElement) {
        const data = Array.from(e.attributes).filter((a) => a.name.startsWith('data-')).map((a) => `${a.name}=${a.value}`).join(',');
        const index = e.parentElement ? Array.prototype.indexOf.call(e.parentElement.children, e) : 0;
        parts.push(`${e.tagName}[${data}]#${index}`);
    }
    return parts.join('<');
};
(dom.window.Element.prototype as any).getBoundingClientRect = function rect(this: Element) {
    const h = createHash('sha256').update(identity(this)).digest();
    const x = h.readUInt16LE(0) % 1200, y = h.readUInt16LE(2) % 760, width = 40 + (h[4] % 40), height = 60 + (h[5] % 40);
    return { x, y, left: x, top: y, width, height, right: x + width, bottom: y + height, toJSON() { return this; } };
};
Object.defineProperty(dom.window, 'innerWidth', { value: 1280, configurable: true });
Object.defineProperty(dom.window, 'innerHeight', { value: 800, configurable: true });

// ---- virtual time ----------------------------------------------------------------------
const realSetImmediate = globalThis.setImmediate;
const realSetTimeout = globalThis.setTimeout, realClearTimeout = globalThis.clearTimeout;
const realSetInterval = globalThis.setInterval, realClearInterval = globalThis.clearInterval;
const realDateNow = Date.now;
const realPerfNow = globalThis.performance.now.bind(globalThis.performance);
const FRAME_MS = 16;
const EPOCH = 1_700_000_000_000;
interface Timer { id: number; due: number; fn: (...a: unknown[]) => void; args: unknown[]; every: number }
let clock = 0;
let timerSeq = 0;
const timers = new Map<number, Timer>();

/** The virtual time, in ms since the page was mounted. */
export const now = (): number => clock;

// The handles this clock gave out, so a clear can tell its own timers from real ones.
const virtualHandles = new WeakSet<object>();

// A clear is handed the handle of whichever clock armed the timer, and only that
// clock can cancel it. The pg pool is why this matters: a client released before
// mount() carries a REAL 10 s idle timer, and when the server checks that client
// out again under the virtual clock, pg-pool cancels the timer with the virtual
// clearTimeout. Swallowing a real handle there left the real timer armed, and
// ten real seconds after the release pg-pool's idle callback ended the client
// in the middle of whatever query it was running. A run that takes longer than
// that (the -O0 coverage lane) lost the first bot commit's connection after the
// row was written: the bot loop threw before its broadcast, and the page never
// saw the move.
const virtualId = (h: unknown): number | null => {
    if (h !== null && typeof h === 'object' && virtualHandles.has(h)) return (h as { id: number }).id;
    if (typeof h === 'number' && timers.has(h)) return h;   // a handle coerced through its Symbol.toPrimitive
    return null;
};

function installClock(): void {
    clock = 0;
    timers.clear();
    g.setTimeout = (fn: (...a: unknown[]) => void, ms?: number, ...args: unknown[]) => {
        const id = ++timerSeq;
        timers.set(id, { id, due: clock + Math.max(0, Number(ms) || 0), fn, args, every: 0 });
        // pg and node internals call .unref()/.ref()/.hasRef() on a timer handle.
        const handle = { id, unref() { return this; }, ref() { return this; }, hasRef() { return true; }, refresh() { return this; }, [Symbol.toPrimitive]: () => id };
        virtualHandles.add(handle);
        return handle as unknown as number;
    };
    g.clearTimeout = (h: unknown) => {
        const id = virtualId(h);
        if (id !== null) timers.delete(id); else realClearTimeout(h as Parameters<typeof clearTimeout>[0]);
    };
    g.setInterval = (fn: (...a: unknown[]) => void, ms?: number, ...args: unknown[]) => {
        const id = ++timerSeq;
        const every = Math.max(1, Number(ms) || 0);
        timers.set(id, { id, due: clock + every, fn, args, every });
        const handle = { id, unref() { return this; }, ref() { return this; }, hasRef() { return true; }, [Symbol.toPrimitive]: () => id };
        virtualHandles.add(handle);
        return handle as unknown as number;
    };
    g.clearInterval = (h: unknown) => {
        const id = virtualId(h);
        if (id !== null) timers.delete(id); else realClearInterval(h as Parameters<typeof clearInterval>[0]);
    };
    g.requestAnimationFrame = (fn: (t: number) => void) => g.setTimeout(() => fn(clock), FRAME_MS);
    g.cancelAnimationFrame = g.clearTimeout;
    Date.now = () => EPOCH + clock;
    globalThis.performance.now = () => clock;
}
function removeClock(): void {
    g.setTimeout = realSetTimeout; g.clearTimeout = realClearTimeout;
    g.setInterval = realSetInterval; g.clearInterval = realClearInterval;
    delete g.requestAnimationFrame; delete g.cancelAnimationFrame;
    Date.now = realDateNow;
    globalThis.performance.now = realPerfNow;
}

// ---- real I/O in flight -------------------------------------------------------------------
// Every Postgres query the server issues goes through this file's one pool
// (e2e/adapters/supabase.ts), so counting its calls is counting the server's I/O.
let ioInFlight = 0;
const track = <T>(p: Promise<T>): Promise<T> => {
    ioInFlight++;
    return p.finally(() => { ioInFlight--; });
};
const poolAny = pgPool as any;
const poolQuery = poolAny.query.bind(pgPool);
const poolConnect = poolAny.connect.bind(pgPool);
poolAny.query = (...a: unknown[]) => {
    const cb = typeof a[a.length - 1] === 'function';
    return cb ? poolQuery(...a) : track(poolQuery(...a));
};
poolAny.connect = (...a: unknown[]) => (a.length > 0 ? poolConnect(...a) : track(poolConnect()));

/** Wait (in REAL time, the virtual clock standing still) until no query and no request is in flight. */
async function quiesce(): Promise<void> {
    let calm = 0;
    for (let spins = 0; calm < 4; spins++) {
        await new Promise<void>((r) => realSetImmediate(r));
        calm = ioInFlight === 0 ? calm + 1 : 0;
        if (spins > 200_000) throw new Error('live_page: the server never went quiet');
    }
}

// ---- the network: the page's supabase client, routed to the real server --------------------
export interface Latency {
    /** Virtual ms a functions.invoke takes to reach the server, and again to come back. */
    invokeMs: number;
    /** Virtual ms from the server's broadcast to the page's channel handler. */
    realtimeMs: number;
}
const latency: Latency = { invokeMs: 0, realtimeMs: 0 };

let me = '';
let myToken = '';

/** Every request the page sent, and when it was answered (virtual ms). */
export interface Sent { at: number; answeredAt: number; name: string; kind: string; status: number; body: unknown; bytes: Uint8Array | null }
export const sent: Sent[] = [];

const later = (ms: number): Promise<void> => new Promise((r) => { g.setTimeout(r, ms); });

/** One request to the real edge entry point `name`: its answer's bytes, or a throw on a refusal. */
async function invoke(name: 'create' | 'action' | 'meta', bytes: Uint8Array | null): Promise<Uint8Array> {
    const kind = !bytes ? '' : name === 'meta' ? askKindName(bytes) : 'packed';
    const rec: Sent = { at: clock, answeredAt: -1, name, kind, status: 0, body: bytes, bytes };
    sent.push(rec);
    if (latency.invokeMs > 0) await later(latency.invokeMs);
    const res = bytes
        ? await track(postBytes(name as 'action' | 'meta', myToken, bytes))
        : await track(postEmpty(name as 'create', myToken));
    rec.status = res.status;
    if (latency.invokeMs > 0) await later(latency.invokeMs);
    rec.answeredAt = clock;
    if (res.status !== 200) throw new Error(res.reason || `HTTP ${res.status}`);
    return res.bytes;
}

/** A BYTEA column as the pool hands it back. */
const byteaOf = (v: unknown): Uint8Array =>
    typeof v === 'string' ? Uint8Array.from(Buffer.from(v.replace(/^\\x/, ''), 'hex')) : new Uint8Array(v as Buffer);

// The page's backend seam (src/common/backend.ts), routed to the real server.
const fb = fakeBackend();
fb.module.create = () => invoke('create', null);
fb.module.meta = (ask: Uint8Array) => invoke('meta', ask);
fb.module.action = async (gid: string, wire: Uint8Array, intent = -1) =>
    netClient().action(200, await invoke('action', clientTable().actionRequest(gid, wire, intent >= 0 ? intent : undefined)));
fb.module.view = async (gid: string) => {
    const mine = await pgPool.query('SELECT view FROM player_views WHERE game_id = $1 AND player_id = $2', [gid, me]);
    if (mine.rows[0]) return byteaOf(mine.rows[0].view);
    // A page that holds no seat reads the shared masked view.
    const theirs = await pgPool.query('SELECT view FROM spectator_views WHERE game_id = $1', [gid]);
    return theirs.rows[0] ? byteaOf(theirs.rows[0].view) : null;
};
fb.module.games = async () => {
    const { rows } = await pgPool.query('SELECT view FROM player_views WHERE player_id = $1 ORDER BY updated_at DESC', [me]);
    return rows.map((r: { view: unknown }) => byteaOf(r.view));
};

let routeGameId = '';
mock.module(new URL('../../src/common/backend.ts', import.meta.url).href, { namedExports: fb.module });
mock.module('next/navigation', { namedExports: {
    useParams: () => (routeGameId ? { game_id: routeGameId } : {}),
    useRouter: () => ({ push() {}, replace() {}, back() {} }),
    usePathname: () => (routeGameId ? `/${routeGameId}` : '/'),
} });

// Broadcasts: the adapter records each message the server's batched REST
// broadcast carries into broadcastLog; the page's stream of the game is handed
// each one addressed to it, `realtimeMs` later, exactly as the stream would.
/** Every broadcast the server sent to this page, and when (virtual ms). */
export interface Delivered { sentAt: number; deliveredAt: number; version: number; payload: any }
export const delivered: Delivered[] = [];
const logPush = broadcastLog.push.bind(broadcastLog);
(broadcastLog as any).push = (...items: any[]) => {
    for (const m of items) {
        // gu-<game>-<me>: my seat's casts of <game>, heard while the page holds the game.
        if (!me || typeof m.channel !== 'string' || !m.channel.startsWith('gu-') || !m.channel.endsWith(`-${me}`)) continue;
        const gid = m.channel.slice(3, m.channel.length - me.length - 1);
        if (m.event !== 'animation_events' || !fb.held.get(`game:${gid}`)) continue;
        const rec: Delivered = { sentAt: clock, deliveredAt: -1, version: castParts(m.payload).version, payload: m.payload };
        delivered.push(rec);
        g.setTimeout(() => { rec.deliveredAt = clock; fb.push.cast(gid, Uint8Array.from(Buffer.from(m.payload.b, 'base64'))); }, latency.realtimeMs);
    }
    return logPush(...items);
};

// ---- the page ------------------------------------------------------------------------------
export interface Probe {
    anim: any;
    actions: any;
    view: any;
}
export const probe: Probe = { anim: null, actions: null, view: null };

/** One painted frame, reduced to what the timing cases read. */
export interface PageFrame {
    t: number;
    /** The flight on screen: its type, acting seat and cards ("6s"), or null. */
    flying: { type: string; seat: number | undefined; cards: string[] } | null;
    /** The board the page is drawn from: the table ("6s" / "6s/7s" per pile) and the version. */
    table: string[];
    version: number;
    /** My hand, as the page orders it. */
    hand: string[];
}

export const notation = (c: { suit: number; value: number }): string =>
    `${'23456789TJQKA'[c.value - 1] ?? '?'}${'shcd'[c.suit] ?? '?'}`;
const isNone = (c: { suit: number; value: number }) => c.value < 1 || c.suit < 0 || c.suit > 3;

export class LivePage {
    frames: PageFrame[] = [];
    host!: HTMLElement;
    private root: any;
    private act!: (fn: () => unknown) => Promise<void>;

    /** Mount the page on `/${gameId}` signed in as `userId`. */
    async mount(gameId: string, userId: string, lat: Partial<Latency> = {}): Promise<void> {
        latency.invokeMs = lat.invokeMs ?? 0;
        latency.realtimeMs = lat.realtimeMs ?? 0;
        me = userId;
        myToken = await tokenFor(userId, 'Me');
        sent.length = 0;
        delivered.length = 0;
        fb.held.clear();
        installClock();
        const React = (await import('react')).default;
        const { act } = await import('react');
        const { createRoot } = await import('react-dom/client');
        const h = React.createElement;
        const { LocalizationProvider } = await import('../../src/contexts/LocalizationContext.tsx');
        const { ThemeProvider } = await import('../../src/contexts/ThemeContext.tsx');
        const { StyleProvider } = await import('../../src/contexts/StyleContext.tsx');
        const { TextureProvider } = await import('../../src/components/TexturedSurface.tsx');
        const { AuthContext } = await import('../../src/contexts/AuthContext.tsx');
        const { ProtectedRoute } = await import('../../src/components/ProtectedRoute.tsx');
        const { GameView } = await import('../../src/components/GameView.tsx');
        const { useAnimation } = await import('../../src/contexts/AnimationContext.tsx');
        const { useServer, useServerActions } = await import('../../src/contexts/ServerContext.tsx');
        const { ensureBotsAsync } = await import('../../sdk/ts/wasm/bots.ts');
        await ensureBotsAsync();
        await serverTable();
        for (const name of ['action', 'meta'] as const) await postEmpty(name, null);   // import each index.ts now
        this.act = async (fn) => { await act(async () => { await fn(); }); };
        routeGameId = gameId;
        const Probe = () => {
            probe.anim = useAnimation();
            probe.actions = useServerActions();
            probe.view = useServer().view;
            return null;
        };
        const auth = {
            user_id: userId, username: 'Me', loading: false,
            signIn: async () => ({}), signUp: async () => ({}), signOut: async () => {}, updatePassword: async () => {},
            redirectAfterLogin: null, setRedirectAfterLogin: () => {}, clearRedirectAfterLogin: () => {},
        };
        const tree = h(LocalizationProvider, null, h(ThemeProvider, null, h(StyleProvider, null, h(TextureProvider, null,
            h(AuthContext.Provider, { value: auth as any }, h(ProtectedRoute, null, h(React.Fragment, null, h(GameView), h(Probe))))))));
        this.host = dom.window.document.createElement('div');
        dom.window.document.body.appendChild(this.host);
        this.root = createRoot(this.host);
        await this.act(() => { this.root.render(tree); });
        await this.settle();
        this.capture();
    }

    /** Let the page's promises, React's work and the server's I/O finish, the clock standing still. */
    async settle(): Promise<void> {
        for (let i = 0; i < 3; i++) {
            await this.act(async () => { await quiesce(); });
        }
    }

    private capture(): void {
        const a = probe.anim?.currentAnimation;
        const v = probe.view;
        const frame: PageFrame = {
            t: clock,
            flying: a ? { type: a.type, seat: a.seat, cards: (a.cards ?? []).map(notation) } : null,
            table: v ? v.battles.map((b: any) => (isNone(b.defense) ? notation(b.attack) : `${notation(b.attack)}/${notation(b.defense)}`)) : [],
            version: v?.version ?? -1,
            hand: v ? v.myHand.map(notation) : [],
        };
        const last = this.frames[this.frames.length - 1];
        if (last && JSON.stringify({ ...last, t: 0 }) === JSON.stringify({ ...frame, t: 0 })) return;
        this.frames.push(frame);
    }

    /** Run `fn` at the current virtual time, as a user's input would. */
    async step(fn: () => unknown): Promise<void> {
        await this.act(fn);
        await this.settle();
        this.capture();
    }

    /** Move the clock `ms` forward, firing every timer due on the way (the server's and the page's). */
    async advance(ms: number): Promise<void> {
        const end = clock + ms;
        for (;;) {
            let next: Timer | null = null;
            for (const t of timers.values()) {
                if (t.due > end) continue;
                if (!next || t.due < next.due || (t.due === next.due && t.id < next.id)) next = t;
            }
            if (!next) break;
            clock = next.due;
            if (next.every > 0) next.due += next.every; else timers.delete(next.id);
            const fire = next;
            await this.act(() => { fire.fn(...fire.args); });
            await this.settle();
            this.capture();
        }
        clock = end;
    }

    /** Advance until `pred` holds on a captured frame, or `maxMs` passes. */
    async advanceUntil(pred: (f: PageFrame) => boolean, maxMs: number, stepMs = FRAME_MS): Promise<boolean> {
        const end = clock + maxMs;
        while (clock < end) {
            await this.advance(stepMs);
            const f = this.frames[this.frames.length - 1];
            if (f && pred(f)) return true;
        }
        return false;
    }

    async unmount(): Promise<void> {
        await this.act(() => { this.root.unmount(); });
        this.host.remove();
        // Let the server's loop run out on the virtual clock, then hand time back.
        for (let i = 0; i < 400 && timers.size > 0; i++) {
            const next = [...timers.values()].sort((a, b) => a.due - b.due)[0];
            clock = Math.max(clock, next.due);
            timers.delete(next.id);
            if (next.every > 0) continue;
            try { next.fn(...next.args); } catch { /* the page is gone */ }
            await quiesce();
        }
        removeClock();
        routeGameId = '';
        me = '';
    }
}

/** A printable timeline of the frames where the flight changed. */
export function flightTimeline(frames: readonly PageFrame[]): string[] {
    const out: string[] = [];
    let prev = '';
    for (const f of frames) {
        const k = f.flying ? `${f.flying.type}@${f.flying.seat} ${f.flying.cards.join(' ')}` : '-';
        if (k === prev) continue;
        prev = k;
        out.push(`${String(f.t).padStart(6)}ms  ${k.padEnd(28)} v${f.version} table[${f.table.join(' ')}]`);
    }
    return out;
}

export { assert };
