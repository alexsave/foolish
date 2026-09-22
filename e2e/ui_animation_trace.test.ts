/* =============================================================================
 * The live game's moves, played through the real page, held to the frames they drew
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md Phase 6b moves the web's optimistic play, its
 * move gates and the animation pipeline's board edits into the kernel. None of
 * that may change what a player sees or how a card moves, so this file plays
 * each case through the deployed page tree in jsdom - the real ServerProvider,
 * AnimationProvider, RealtimeAnimationFeed, GameDisplay and AnimationOverlay -
 * against a server that is the C Table itself, and holds every frame the page
 * drew, at the virtual millisecond it drew it, to the frames recorded from the
 * code before the phase (e2e/fixtures/ui_anim/*.txt). The goldens of the cases
 * that showed the bugs the invariants below found were re-recorded with their
 * fix, and every other frame is still the code before the phase's.
 *
 * The cases are the plan's browser list - an attack, a cover, a pass, a pickup,
 * a good, a throw-in while a move is pending, a rejected move - plus the races
 * the July flicker fix is about (a pickup that sweeps my pending card, a
 * concurrent throw-in, a stale push after a newer one), a pass the next
 * defender cannot hold, a pickup a closing good overtakes, a resync while a card
 * is pending, the rematch reset and a hand rearrange.
 *
 * What makes a frame comparable across runs:
 *   - time is virtual: setTimeout, setInterval and Date.now run on a clock the
 *     test advances, and every timer fires inside act(), so a frame is taken at
 *     an exact virtual time and never at a moment a slow machine happened to hit;
 *   - the server answers and pushes only when the case says so, so a race is
 *     played in the same order every run;
 *   - Math.random is seeded per case;
 *   - jsdom has no layout, so getBoundingClientRect answers a rect derived from
 *     the element's own identity (tag, data attributes, position among its
 *     siblings): a flight that looks up a different element starts or ends
 *     somewhere else, and the overlay's inline position says so.
 *
 * Three things hold on every case whatever the golden says: no board the store
 * holds shows a card twice (on the table, either side of a battle, and in my
 * hand); no frame of the page draws a card twice (a flight and the place it left
 * or lands on, say), and a card a case tracks is drawn exactly once on every
 * frame; and once every push has been delivered the store's board is the one
 * the server holds for me - the page settles on the truth, a lost push after a
 * refusal included.
 *
 * A frame is the page's HTML and the store it was drawn from; the golden holds
 * each distinct frame's time and hash. UPDATE_UI_ANIM=1 rewrites the goldens; UI_ANIM_DUMP=<dir> writes every
 * frame's HTML for a diff when a case fails.
 * ========================================================================== */

import { test, mock, after } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { JSDOM } from 'jsdom';
import { fixture, fixtureTable, PLAYING, GAME_OVER, IN, OUT, type FixtureSeat, type TableFixture } from './helpers/table_fixture.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { clientTable } from '../sdk/ts/table/client_table.ts';
import * as V from '../sdk/ts/gen/view_layout.bots.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; console.debug = () => {}; }

const GOLDEN_DIR = new URL('./fixtures/ui_anim/', import.meta.url);
const UPDATE = process.env.UPDATE_UI_ANIM === '1';
const DUMP = process.env.UI_ANIM_DUMP;

// ---- jsdom -------------------------------------------------------------------------
const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>', { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
for (const k of ['window', 'document', 'HTMLElement', 'HTMLCanvasElement', 'Node', 'Element', 'MouseEvent', 'KeyboardEvent',
    'getComputedStyle', 'requestAnimationFrame', 'cancelAnimationFrame', 'localStorage', 'sessionStorage', 'Image']) {
    try { g[k] = (dom.window as any)[k]; } catch { /* a getter already there */ }
}
try { Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true }); } catch { /* ok */ }
g.IS_REACT_ACT_ENVIRONMENT = true;
g.ResizeObserver ??= class { observe() {} unobserve() {} disconnect() {} };
dom.window.matchMedia ??= ((q: string) => ({ matches: false, media: q, addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {} })) as any;
g.matchMedia = dom.window.matchMedia;
(dom.window.HTMLCanvasElement.prototype as any).getContext = () => null;

// A rect per element identity: its tag, its data attributes, and its index among its siblings, up the tree.
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

// ---- the server: the C Table, answering when the case says so ---------------------
const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const b64 = (b: Uint8Array) => Buffer.from(b).toString('base64');

interface Push { gid: string; version: number; bytes: Uint8Array; seq: string }

class Server {
    state: Uint8Array;
    roster: Uint8Array;
    version: number;
    readonly outbox = new Map<string, Push[]>();   // user id -> pushes not yet delivered
    /** A push to me was lost: the page cannot settle on the server's board. */
    lost = false;

    lose(userId: string): void { this.take(userId); this.lost = true; }
    private n = 0;

    constructor(readonly gid: string, board: TableFixture, version = 10) {
        this.state = board.state;
        this.roster = board.roster;
        this.version = version;
    }

    private load(): ReturnType<typeof fixtureTable> {
        const t = fixtureTable();
        assert.equal(t.load(this.state, this.roster), L.TABLE_OK, 'the server table loads');
        t.setDealSeed(null);
        return t;
    }

    seatOf(userId: string): number { return this.load().seatOf(userId); }

    envelope(userId: string): Uint8Array {
        const t = this.load();
        const env = t.envelope(this.gid, t.seatOf(userId), this.version);
        assert.ok(env instanceof Uint8Array, `envelope: ${env}`);
        return env;
    }

    /** One table operation by `userId`; commits and queues every human seat's push. Returns the table result and reject code. */
    op(userId: string, run: (t: ReturnType<typeof fixtureTable>) => number): { rc: number; reject: number } {
        const t = this.load();
        const rc = run(t);
        assert.ok(rc >= 0, `the operation is not refused outright (${rc})`);
        if (rc === L.TABLE_REJECTED || rc === L.TABLE_MOOT || rc === L.TABLE_STALE_ROUND) return { rc, reject: t.reject() };
        const p = t.commit(this.gid, this.version + 1, 1_700_000_000_000);
        assert.ok(typeof p !== 'number', `commit products: ${p}`);
        const seats = t.seats();
        const pushes: [string, Uint8Array][] = [];
        if (p.nEvents > 0) {
            for (let s = 0; s < seats.length; s++) {
                if (seats[s].brain) continue;
                const b = t.push(this.gid, s);
                assert.ok(b instanceof Uint8Array, `push: ${b}`);
                pushes.push([seats[s].id, b]);
            }
        }
        this.state = p.state;
        this.roster = p.roster;
        this.version += 1;
        for (const [id, bytes] of pushes) {
            const q = this.outbox.get(id) ?? [];
            q.push({ gid: this.gid, version: this.version, bytes, seq: `${this.gid}-${++this.n}` });
            this.outbox.set(id, q);
        }
        return { rc, reject: 0 };
    }

    act(userId: string, wire: Uint8Array): { rc: number; reject: number } {
        return this.op(userId, (t) => t.act(userId, wire, null, 0));
    }

    /** The next undelivered push for `userId`. */
    take(userId: string): Push {
        const q = this.outbox.get(userId) ?? [];
        const p = q.shift();
        assert.ok(p, `a push is waiting for ${userId}`);
        return p;
    }
}

// ---- the network ---------------------------------------------------------------------
interface Pending { kind: 'action' | 'meta'; body: unknown; resolve: (r: { data: unknown; error: unknown }) => void }
let server: Server | null = null;
let pending: Pending[] = [];
const channels = new Map<string, { handlers: { event: string; cb: (p: unknown) => void }[] }>();

function query(table: string) {
    const filters: Record<string, unknown> = {};
    const answer = (single: boolean) => {
        let data: unknown = null;
        const gid = filters.game_id as string | undefined;
        if (table === 'player_views') {
            data = single ? (server && gid === server.gid && server.seatOf(ME) >= 0 ? { view: hex(server.envelope(ME)) } : null) : [];
        } else if (table === 'spectator_views') data = null;
        else if (table === 'chat_messages') data = [];
        else if (table === 'user_elo_ratings' || table === 'bots') data = [];
        return { data, error: null };
    };
    const chain: any = {
        select: () => chain, order: () => chain, limit: () => chain, insert: () => chain,
        eq: (k: string, v: unknown) => { filters[k] = v; return chain; },
        in: () => chain,
        maybeSingle: () => Promise.resolve(answer(true)),
        single: () => Promise.resolve(answer(true)),
        then: (ok: (r: unknown) => unknown, bad?: (e: unknown) => unknown) => Promise.resolve(answer(false)).then(ok, bad),
    };
    return chain;
}

function channel(topic: string) {
    const entry = { handlers: [] as { event: string; cb: (p: unknown) => void }[] };
    channels.set(topic, entry);
    const ch: any = {
        topic,
        on: (_type: string, filter: { event?: string }, cb: (p: unknown) => void) => { entry.handlers.push({ event: filter?.event ?? '', cb }); return ch; },
        subscribe: (cb?: (status: string, err?: unknown) => void) => { cb?.('SUBSCRIBED'); return ch; },
        unsubscribe: () => Promise.resolve(),
    };
    return ch;
}

const supabaseMock = {
    from: query,
    channel,
    getChannels: () => [],
    removeChannel: () => Promise.resolve(),
    realtime: { setAuth: () => Promise.resolve() },
    functions: {
        invoke: (name: string, opts: { body?: unknown }) => {
            const body = opts?.body;
            // The bot bump rides the action endpoint as JSON: nothing to answer.
            if (name === 'action' && !(body instanceof Blob)) return Promise.resolve({ data: null, error: null });
            return new Promise((resolve) => { pending.push({ kind: name === 'action' ? 'action' : 'meta', body, resolve }); });
        },
    },
    auth: {
        getSession: () => Promise.resolve({ data: { session: null } }),
        onAuthStateChange: () => ({ data: { subscription: { unsubscribe() {} } } }),
    },
};

let routeGameId = '';
mock.module('../src/backend/Connector.ts', { defaultExport: supabaseMock });
mock.module('next/navigation', { namedExports: {
    useParams: () => (routeGameId ? { game_id: routeGameId } : {}),
    useRouter: () => ({ push() {}, replace() {}, back() {} }),
    usePathname: () => (routeGameId ? `/${routeGameId}` : '/'),
} });

// ---- virtual time ----------------------------------------------------------------------
const realRandom = Math.random;
const realSetTimeout = globalThis.setTimeout, realClearTimeout = globalThis.clearTimeout;
const realSetInterval = globalThis.setInterval, realClearInterval = globalThis.clearInterval;
const realDateNow = Date.now;
// The animation pipeline is a requestAnimationFrame loop over performance.now()
// now (docs/C_GAME_SHAPE_MIGRATION.md Phase 9), so both are part of the clock
// this file controls. A real rAF would fire on the machine's paint schedule and
// a real performance.now would read the wall, and a frame taken then is a frame
// a slow machine happened to hit - which is exactly what this harness exists
// not to record.
const realRaf = globalThis.requestAnimationFrame, realCancelRaf = globalThis.cancelAnimationFrame;
const realPerfNow = globalThis.performance.now.bind(globalThis.performance);
// A 60 Hz display, as a virtual interval. Every landing the kernel plans is
// therefore observed at the first frame at or after it, which is what a browser
// does too: nothing is painted between two frames.
const FRAME_MS = 16;
interface Timer { id: number; due: number; fn: (...a: unknown[]) => void; args: unknown[]; every: number }
let clock = 0;
let timerSeq = 0;
const timers = new Map<number, Timer>();
function installClock(): void {
    clock = 0;
    timers.clear();
    g.setTimeout = (fn: (...a: unknown[]) => void, ms?: number, ...args: unknown[]) => {
        const id = ++timerSeq;
        timers.set(id, { id, due: clock + Math.max(0, Number(ms) || 0), fn, args, every: 0 });
        return id;
    };
    g.clearTimeout = (id: number) => { timers.delete(id); };
    g.setInterval = (fn: (...a: unknown[]) => void, ms?: number, ...args: unknown[]) => {
        const id = ++timerSeq;
        const every = Math.max(1, Number(ms) || 0);
        timers.set(id, { id, due: clock + every, fn, args, every });
        return id;
    };
    g.clearInterval = (id: number) => { timers.delete(id); };
    g.requestAnimationFrame = (fn: (t: number) => void) => g.setTimeout(() => fn(clock), FRAME_MS);
    g.cancelAnimationFrame = (id: number) => { timers.delete(id); };
    Date.now = () => 1_700_000_000_000 + clock;
    globalThis.performance.now = () => clock;
}
function removeClock(): void {
    g.setTimeout = realSetTimeout; g.clearTimeout = realClearTimeout;
    g.setInterval = realSetInterval; g.clearInterval = realClearInterval;
    g.requestAnimationFrame = realRaf; g.cancelAnimationFrame = realCancelRaf;
    Date.now = realDateNow;
    globalThis.performance.now = realPerfNow;
}
after(() => { Math.random = realRandom; removeClock(); });

function seedRandom(seed: number): void {
    let s = seed >>> 0;
    Math.random = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
}

// ---- the page ------------------------------------------------------------------------------
const ME = 'u-me-0000', ANNA = 'u-anna-0001', BORIS = 'u-boris-001';
const seat = (id: string, name: string): FixtureSeat => ({ id, name });

interface Probe {
    anim: any;
    actions: any;
    /** The store the page renders from: the board on screen and the hand order it draws. */
    store: string;
}
const probe: Probe = { anim: null, actions: null, store: '' };

interface Frame { t: number; label: string; html: string }

// The cards a board shows me, as keys: both sides of every battle, then my hand.
const shown = (view: any): string[] => [
    ...view.battles.flatMap((b: any) => (b.defense.suit === V.CARD_NONE_SUIT && b.defense.value === V.CARD_NONE_VALUE ? [b.attack] : [b.attack, b.defense])),
    ...view.myHand,
].map((c: any) => `${c.suit}/${c.value}`);

// A board's table, in the kernel's own notation ("7d"), both sides of every battle.
const tableNotation = (view: any): string[] =>
    view.battles.flatMap((b: any) => (b.defense.suit === V.CARD_NONE_SUIT && b.defense.value === V.CARD_NONE_VALUE ? [b.attack] : [b.attack, b.defense]))
        .map((c: any) => `${'23456789TJQKA'[c.value - 1] ?? '?'}${'shcd'[c.suit] ?? '?'}`);

// What a board says about the game, for comparing the page's with the server's.
const settled = (view: any) => ({
    status: view.status, firstAttacker: view.firstAttacker, defender: view.defender,
    battles: view.battles, hand: [...view.myHand].map((c: any) => `${c.suit}/${c.value}`).sort(),
    seats: view.seats.map((x: any) => [x.status, x.handCount]),
});

// The card faces the page shows, as their printed corner ("8\u2665"): every CardFace on
// the table, in a hand, on the flipped slot or in flight, unless it or a parent is
// hidden. A card may be in one place at a time - a flight carries the card, so the
// place it left and the place it is going to must not also show it.
const CARD_FACE_TEXT = /^(10|[2-9JQKA])([\u2660\u2665\u2663\u2666])/u;
function visibleFaces(host: HTMLElement): string[] {
    const out: string[] = [];
    for (const el of Array.from(host.querySelectorAll<HTMLElement>('div'))) {
        if (!el.style.fontFamily.startsWith('Georgia')) continue;
        let hidden = false;
        for (let e: HTMLElement | null = el; e && e !== host; e = e.parentElement) {
            if (e.style.visibility === 'hidden' || e.style.opacity === '0' || e.style.display === 'none') { hidden = true; break; }
        }
        if (hidden) continue;
        const m = CARD_FACE_TEXT.exec(el.textContent ?? '');
        if (m) out.push(m[1] + m[2]);
    }
    return out;
}
// The kernel's notation ("6s") as the page prints it ("6\u2660").
const printed = (card: string) => `${card[0] === 'T' ? '10' : card[0]}${'\u2660\u2665\u2663\u2666'['shcd'.indexOf(card[1])]}`;

class Stage {
    frames: Frame[] = [];
    /** Boards the store held that showed a card twice. */
    doubled: string[] = [];
    /** Frames whose page showed a card twice: in flight and where it left or lands, say. */
    shownTwice: string[] = [];
    /** Cards that must show exactly once on every frame from `tracking` on, and the frames that did not. */
    tracked: string[] = [];
    lostOrDoubled: string[] = [];
    /** The seat the shield stood on at each frame, repeats collapsed. A pass is a
     *  card going out AND the shield going on, so the shield's own out-home-out is
     *  the stutter class the pass cases are about: this list is longer than the
     *  moves that moved it. */
    shield: number[] = [];
    /** Every distinct board the store held, in the two terms a pass is about: the
     *  shield's seat and the cards on the table. A pass's card standing on the
     *  table under my own shield is a board no game ever reaches, and it is what a
     *  cached shield produced. */
    boards: { t: number; defender: number; table: string[] }[] = [];
    track(...cards: string[]): void { this.tracked = cards.map(printed); }
    host!: HTMLElement;
    root: any;
    act!: (fn: () => unknown) => Promise<void>;

    async mount(gid: string): Promise<void> {
        const React = (await import('react')).default;
        const { act } = await import('react');
        const { createRoot } = await import('react-dom/client');
        const h = React.createElement;
        const { LocalizationProvider } = await import('../src/contexts/LocalizationContext.tsx');
        const { ThemeProvider } = await import('../src/contexts/ThemeContext.tsx');
        const { StyleProvider } = await import('../src/contexts/StyleContext.tsx');
        const { TextureProvider } = await import('../src/components/TexturedSurface.tsx');
        const { AuthContext } = await import('../src/contexts/AuthContext.tsx');
        const { ProtectedRoute } = await import('../src/components/ProtectedRoute.tsx');
        const { GameView } = await import('../src/components/GameView.tsx');
        const { useAnimation } = await import('../src/contexts/AnimationContext.tsx');
        const { useServer, useServerActions } = await import('../src/contexts/ServerContext.tsx');
        const { ensureBotsAsync } = await import('../sdk/ts/wasm/bots.ts');
        await ensureBotsAsync();
        this.act = async (fn) => { await act(async () => { await fn(); }); };
        routeGameId = gid;
        const Probe = () => {
            probe.anim = useAnimation();
            probe.actions = useServerActions();
            const { view, localHandOrder } = useServer();
            probe.store = JSON.stringify({ view, localHandOrder });
            return null;
        };
        const auth = {
            user_id: ME, username: 'Me', loading: false,
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
        this.capture('mounted');
    }

    /** Lets promises and React work finish without moving the clock. */
    async settle(): Promise<void> {
        for (let i = 0; i < 8; i++) {
            await this.act(() => new Promise<void>((r) => setImmediate(r)));
        }
    }

    capture(label: string): void {
        // The page, and the store it was drawn from: a board the store holds is part of the frame
        // even where the DOM does not show it (a hand order before the arrangement memory reads it).
        const html = this.host.innerHTML + '\n<!-- store -->\n' + probe.store;
        const view = probe.store ? JSON.parse(probe.store).view : null;
        if (view) {
            const cards = shown(view);
            const twice = cards.filter((c, i) => cards.indexOf(c) !== i);
            if (twice.length > 0) this.doubled.push(`${clock}ms (${label}): ${twice.join(' ')}`);
            if (typeof view.defender === 'number' && this.shield[this.shield.length - 1] !== view.defender) {
                this.shield.push(view.defender);
            }
            const table = tableNotation(view);
            const board = this.boards[this.boards.length - 1];
            if (!board || board.defender !== view.defender || board.table.join(' ') !== table.join(' ')) {
                this.boards.push({ t: clock, defender: view.defender, table });
            }
        }
        const faces = visibleFaces(this.host);
        const twiceOnPage = faces.filter((c, i) => faces.indexOf(c) !== i);
        if (twiceOnPage.length > 0) this.shownTwice.push(`${clock}ms (${label}): ${twiceOnPage.join(' ')}`);
        for (const c of this.tracked) {
            const n = faces.filter((f) => f === c).length;
            if (n !== 1) this.lostOrDoubled.push(`${clock}ms (${label}): ${c} shown ${n} times`);
        }
        const last = this.frames[this.frames.length - 1];
        if (!last || last.html !== html) this.frames.push({ t: clock, label, html });
    }

    /** Moves the clock `ms` forward, firing every timer due on the way, each inside act. */
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
            this.capture(`timer@${clock}`);
        }
        clock = end;
    }

    /** Runs a step of the case at the current virtual time. */
    async step(label: string, fn: () => unknown): Promise<void> {
        await this.act(fn);
        await this.settle();
        this.capture(label);
    }

    async unmount(): Promise<void> {
        await this.act(() => { this.root.unmount(); });
        this.host.remove();
    }
}

// ---- what a case does ------------------------------------------------------------------
// The kernel's card notation (c/src/card.c read_card): "2" is value 1, "a" is 13; suits in SUIT_* order.
const cards = (text: string) => text.split(' ').map((t) => {
    const suit = 'shcd'.indexOf(t[1]);
    const value = '23456789TJQKA'.indexOf(t[0]) + 1;
    assert.ok(suit >= 0 && value >= 1, `card ${t}`);
    return { suit, value };
});

/** A tap's promise, whose rejection (a refused move) is the page's to handle. */
const tap = (p: Promise<unknown>) => { p.catch(() => {}); };

/** Answers the oldest waiting request from the page. */
async function answer(stage: Stage, label: string): Promise<void> {
    const s = server!;
    const req = pending.shift();
    assert.ok(req, `${label}: a request is waiting`);
    await stage.step(label, async () => {
        if (req.kind === 'action') {
            const body = new Uint8Array(await (req.body as Blob).arrayBuffer());
            const t = fixtureTable();
            const decoded = t.requestDecode(body);
            assert.ok(typeof decoded !== 'number', `${label}: the request decodes`);
            const r = s.act(ME, decoded.wire);            const response = fixtureTable().actionResponse(r.rc, r.reject, s.version);
            req.resolve({ data: response.buffer.slice(response.byteOffset, response.byteOffset + response.byteLength), error: null });
        } else {
            const body = req.body as { type: string; card_indices?: number[] };
            if (body.type === 'continue') s.op(ME, (t) => t.continueGame(ME));
            else if (body.type === 'rearrange-hand') s.op(ME, (t) => t.rearrangeHand(ME, body.card_indices!));
            else assert.fail(`${label}: an unexpected meta ${body.type}`);
            const env = s.envelope(ME);
            req.resolve({ data: env.buffer.slice(env.byteOffset, env.byteOffset + env.byteLength), error: null });
        }
    });
}

/** Delivers the next push for me on my gu- channel, as realtime would. */
async function deliver(stage: Stage, label: string, push?: Push): Promise<void> {
    const s = server!;
    const p = push ?? s.take(ME);
    const ch = channels.get(`gu-${s.gid}-${ME}`);
    assert.ok(ch, `${label}: the page joined its gu- channel`);
    const handler = ch.handlers.find((x) => x.event === 'animation_events');
    assert.ok(handler, `${label}: the page listens for animation events`);
    await stage.step(label, () => handler.cb({ payload: { t: 'as3', s: p.seq, v: p.version, b: b64(p.bytes) } }));
}

const traceText = (frames: Frame[]): string =>
    frames.map((f) => `${f.t} ${createHash('sha256').update(f.html).digest('hex').slice(0, 24)} ${f.label}`).join('\n') + '\n';

function dumpFrames(name: string, frames: Frame[]): void {
    if (!DUMP) return;
    const dir = join(DUMP, name);
    mkdirSync(dir, { recursive: true });
    frames.forEach((f, i) => writeFileSync(join(dir, `${String(i).padStart(3, '0')}_${f.t}.html`), f.html));
    writeFileSync(join(dir, 'trace.txt'), traceText(frames));
}

function holdToGolden(name: string, frames: Frame[]): void {
    const text = traceText(frames);
    assert.ok(frames.length >= 2, `${name}: the page drew frames (${frames.length})`);
    const file = new URL(`${name}.txt`, GOLDEN_DIR);
    if (UPDATE) {
        mkdirSync(GOLDEN_DIR, { recursive: true });
        writeFileSync(file, text);
        return;
    }
    assert.ok(existsSync(file), `${name}: no golden at ${file.pathname}; record it with UPDATE_UI_ANIM=1`);
    const want = readFileSync(file, 'utf8');
    if (text !== want) {
        const got = text.split('\n'), exp = want.split('\n');
        let i = 0;
        while (i < got.length && got[i] === exp[i]) i++;
        assert.fail(`${name}: frame ${i} differs from the golden:\n  got:  ${got.slice(i, i + 3).join(' | ')}\n  want: ${exp.slice(i, i + 3).join(' | ')}`);
    }
}

async function play(name: string, seed: number, gid: string, board: TableFixture, script: (s: Stage, srv: Server) => Promise<void>): Promise<void> {
    server = new Server(gid, board);
    pending = [];
    channels.clear();
    seedRandom(seed);
    installClock();
    const stage = new Stage();
    try {
        await stage.mount(gid);
        await script(stage, server);
        await stage.advance(6000);
        assert.equal(pending.length, 0, `${name}: every request was answered`);
        assert.deepEqual(stage.doubled, [], `${name}: a board showed a card twice`);
        assert.deepEqual(stage.shownTwice, [], `${name}: the page showed a card twice`);
        assert.deepEqual(stage.lostOrDoubled, [], `${name}: a tracked card was not on the page exactly once`);
        if ((server.outbox.get(ME) ?? []).length === 0) {
            const mine = clientTable().adoptEnvelope(server.envelope(ME))!;
            assert.deepEqual(settled(JSON.parse(probe.store).view), settled(mine), `${name}: the page settles on the server's board`);
        }
    } finally {
        await stage.unmount();
        removeClock();
        server = null;
        dumpFrames(name, stage.frames);
    }
    holdToGolden(name, stage.frames);
}

const two = (meSeat: 0 | 1) => {
    const ids = meSeat === 0 ? [seat(ME, 'Me'), seat(ANNA, 'Anna')] : [seat(ANNA, 'Anna'), seat(ME, 'Me')];
    return fixture().title('Two of us').seats(ids).status(PLAYING).deterministic().trump('Kc').deck('7s 8s 9s Ts');
};
const three = () => fixture().title('Three of us').seats([seat(ANNA, 'Anna'), seat(ME, 'Me'), seat(BORIS, 'Boris')])
    .status(PLAYING).deterministic().trump('Kc').deck('7s 8s 9s Ts');
const threeMeFirst = () => fixture().title('Three of us').seats([seat(ME, 'Me'), seat(ANNA, 'Anna'), seat(BORIS, 'Boris')])
    .status(PLAYING).deterministic().trump('Kc').deck('7s 8s 9s Ts');

// The cards in flight on the overlay: where each is drawn, its scale, and whether it is a refusal's red return.
const flights = (host: HTMLElement) => Array.from(host.querySelectorAll<HTMLElement>('div'))
    .filter((d) => d.style.position === 'fixed' && d.style.zIndex === '10000')
    .flatMap((o) => Array.from(o.children) as HTMLElement[])
    .map((c) => ({
        left: parseFloat(c.style.left), top: parseFloat(c.style.top),
        scale: parseFloat(/scale\(([0-9.]+)\)/.exec(c.style.transform)?.[1] ?? 'NaN'),
        red: c.innerHTML.includes('rgb(255, 150, 150)'),
    }));

// The page's own controls: a tap on a hand card (DragContext's tap is a press and a
// release inside 150 ms) and the Attack button.
const handCard = (host: HTMLElement, card: string) => {
    const c = cards(card)[0];
    const el = host.querySelector<HTMLElement>(`[data-location="hand"][data-card="${c.suit}-${c.value}"]`);
    assert.ok(el, `${card} is in my hand`);
    return el;
};
const selectedInHand = (host: HTMLElement): string[] =>
    Array.from(host.querySelectorAll<HTMLElement>('[data-location="hand"][data-card]'))
        .filter((el) => el.style.border.includes('rgb(255, 0, 0)') || el.style.border.includes(' red'))
        .map((el) => el.getAttribute('data-card')!);
const attackButton = (host: HTMLElement): HTMLElement | null =>
    Array.from(host.querySelectorAll<HTMLElement>('.btn-action-text')).find((el) => el.textContent === 'Attack') ?? null;
async function tapCard(s: Stage, card: string): Promise<void> {
    await s.step(`tap ${card}`, () => { handCard(s.host, card).dispatchEvent(new dom.window.MouseEvent('mousedown', { bubbles: true })); });
    await s.advance(40);
    await s.step(`release ${card}`, () => { dom.window.document.dispatchEvent(new dom.window.MouseEvent('mouseup', { bubbles: true })); });
}

// ---- the cases ------------------------------------------------------------------------------

test('attack: my card flies to the table, the server confirms it', async () => {
    const board = two(0).hand(0, '6s 6h Qd Ad 9c Tc').hand(1, '7h 8h 9h Th Jh Qh').attacker(0).defender(1).build();
    await play('attack', 101, 'an-attack', board, async (s) => {
        s.track('6s');
        await s.step('tap attack 6s', () => tap(probe.anim.attack(cards('6s'))));
        await s.advance(120);
        await answer(s, 'server applies');
        await s.advance(130);
        await deliver(s, 'push: my attack');
    });
});

test('cover: my card covers the attack, the server confirms it', async () => {
    const board = two(1).hand(0, '9c Tc Jd Qd').hand(1, '8h 7d Ad Js Qs Ks').table('6h').attacker(0).defender(1).build();
    await play('cover', 102, 'a-cover', board, async (s) => {
        s.track('8h', '6h');
        await s.step('tap cover 8h on 6h', () => tap(probe.anim.cover(cards('8h'), cards('6h'))));
        await s.advance(150);
        await answer(s, 'server applies');
        await s.advance(100);
        await deliver(s, 'push: my cover');
    });
});

test('a LAST DEFENCE rests before the sweep takes the table', async () => {
    // THE 1500ms THE OWNER ASKED FOR TWICE, and the one gap in a sequence that is
    // not ANIM_GAP_MS. `8h` is my whole hand, so covering with it empties it and
    // the bout closes on the cover (game.c handle_cover's `def->hand_count == 0`
    // branch - the "last defense" the owner named): the push carries the cover,
    // the discard that sweeps the table and the refills behind it, and the
    // kernel's plan puts ANIM_BOUT_END_HOLD_MS between the cover LANDING and the
    // sweep OPENING (anim_plan.h ANIM_BEAT_HOLDS, anim_build_plan's beat layout).
    //
    // HERE BECAUSE NOTHING ELSE IN THIS FILE REACHES IT. Every other case is a
    // stream with no bout-ending cover in it - a good closes the bout in the
    // `good` case, a pickup sweeps in `pickup` - so before this one the hold had
    // no frame times gating it at all. Measured in a browser before it existed,
    // the sweep took the table away 36ms after the card that won it landed
    // (docs/WEB_ANIM_PARITY.md section 3).
    const board = two(1).hand(0, '9c Tc Jd Qd').hand(1, '8h').table('6h')
        .attacker(0).defender(1).build();
    await play('cover_ends_bout', 130, 'a-bout-end', board, async (s) => {
        await s.step('tap cover 8h on 6h', () => tap(probe.anim.cover(cards('8h'), cards('6h'))));
        await s.advance(150);
        await answer(s, 'server applies');
        await s.advance(100);
        await deliver(s, 'push: the last defence closes the bout');
    });
});

test('the defender covers both attacks at once, and both cards fly together', async () => {
    // ONE MOVE IS ONE MOVEMENT. The kernel spends one COVER event per card, so
    // Anna covering two attacks in a single action reaches me as TWO cover
    // events by one seat - and anim_build_plan opens both at the same
    // millisecond (AnimPlanStep.beat_n), where before this they crawled across
    // the table one 525ms flight after the other. The page draws the beat, not
    // the step: useAnimationRun merges the run the same way the plan merged the
    // clock, and each card keeps the pile the kernel named it for.
    //
    // Anna's move, not mine, on purpose: a multi-card cover of MY OWN is
    // predicted as one event before any push exists, so it never reaches the
    // merge. Every cover that ARRIVES does.
    const board = two(0).hand(0, '9c Tc Jd Qd').hand(1, '8h 9h Ks').table('6h', '7h')
        .attacker(0).defender(1).build();
    await play('cover_both_at_once', 131, 'a-double-cover', board, async (s, srv) => {
        await s.step('Anna covers both on the server', () => {
            srv.act(ANNA, encodeAction({ kind: 'cover', cards: cards('8h 9h'), attack_cards: cards('6h 7h') }));
        });
        await s.advance(100);
        await deliver(s, 'push: Anna covers both attacks');
    });
});

test('the `out` that ends the game costs the sequence no time of its own', async () => {
    // AN OUT IS A NOTICE - no cards, no flight, no time - and it used to burn a
    // whole silent half second in the middle of a sequence (anim_plan.h's beats
    // section; anim_step_duration_ms answers 0 for it). The deck is empty, so my
    // last defence sweeps the table, the refill hands out nothing, I go out and
    // the game ends: cover, discard, refill, OUT, defender_move, transition. The
    // out lands in the gap the refill already had and the beat behind it opens
    // where it would have with no out in the stream at all - the frames below
    // are half a second shorter than they were.
    const board = two(1).hand(0, '9c Tc Jd Qd').hand(1, '8h').table('6h').deck('')
        .attacker(0).defender(1).build();
    await play('out_costs_nothing', 140, 'an-out', board, async (s) => {
        await s.step('tap cover 8h on 6h', () => tap(probe.anim.cover(cards('8h'), cards('6h'))));
        await s.advance(150);
        await answer(s, 'server applies');
        await s.advance(100);
        await deliver(s, 'push: the last defence ends the game');
    });
});

test('pass: I hand the attack on with a card of its rank', async () => {
    const board = three().hand(0, '9c Tc Jd Qd').hand(1, '7d 8d Ad').hand(2, '6s Js Qs Ks As').table('7h').attacker(0).defender(1).build();
    await play('pass', 103, 'a-pass', board, async (s) => {
        s.track('7d', '7h');
        await s.step('tap pass 7d', () => tap(probe.anim.pass(cards('7d'))));
        await s.advance(600);
        await answer(s, 'server applies');
        await s.advance(50);
        await deliver(s, 'push: my pass');
    });
});

test('pickup: I take the table', async () => {
    const board = two(1).hand(0, '9c Tc Jd').hand(1, 'Js Qs Ks').table('6h/8h', '6d').attacker(0).defender(1).build();
    await play('pickup', 104, 'a-pickup', board, async (s) => {
        s.track('6h', '8h', '6d');
        await s.step('tap pickup', () => tap(probe.anim.pickup()));
        await s.advance(200);
        await answer(s, 'server applies');
        await s.advance(100);
        await deliver(s, 'push: my pickup');
    });
});

test('good: my good closes the bout', async () => {
    const board = threeMeFirst().hand(0, '9c Tc Jd').hand(1, 'Js Qs Ks').hand(2, 'Ad Qd 6d').table('7h/9h')
        .attacker(0).defender(1).good(2).goodTimestamp().build();
    await play('good', 105, 'a-good', board, async (s) => {
        await s.step('tap good', () => tap(probe.anim.good()));
        await s.advance(90);
        await answer(s, 'server applies');
        await s.advance(60);
        await deliver(s, 'push: the bout closes');
    });
});

test('throw-in while my move is pending: another attacker lands first', async () => {
    const board = threeMeFirst().hand(0, '7d Tc Jd').hand(1, 'Js Qs Ks 6s As').hand(2, '9d Qd 6d').table('7h/9h')
        .attacker(0).defender(1).build();
    await play('throw_in_pending', 106, 'a-throw-in', board, async (s, srv) => {
        await s.step('tap attack 7d', () => tap(probe.anim.attack(cards('7d'))));
        await s.advance(60);
        await s.step('Boris throws in 9d on the server', () => { srv.act(BORIS, encodeAction({ kind: 'attack', cards: cards('9d') })); });
        await s.advance(90);
        await deliver(s, 'push: Boris\'s throw-in');
        await s.advance(100);
        await answer(s, 'server applies mine');
        await s.advance(150);
        await deliver(s, 'push: my throw-in');
    });
});

test('a rejected move, the refusal first: the defender took the table before my throw-in reached the server', async () => {
    const board = threeMeFirst().hand(0, '6s Tc Jd').hand(1, 'Js Qs Ks As').hand(2, '9d Qd 6d').table('6h')
        .attacker(0).defender(1).build();
    await play('rejected_refusal_first', 107, 'a-reject-1', board, async (s, srv) => {
        s.track('6s');
        await s.step('Anna picks up on the server', () => { srv.act(ANNA, encodeAction({ kind: 'pickup' })); });
        await s.step('tap attack 6s', () => tap(probe.anim.attack(cards('6s'))));
        await s.advance(150);
        await answer(s, 'server rejects mine');
        await s.advance(250);
        await deliver(s, 'push: Anna\'s pickup');
    });
});

test('a rejected move whose push never arrives: the card goes home and stays there', async () => {
    const board = threeMeFirst().hand(0, '6s Tc Jd').hand(1, 'Js Qs Ks As').hand(2, '9d Qd 6d').table('6h')
        .attacker(0).defender(1).build();
    await play('rejected_push_lost', 118, 'a-reject-lost', board, async (s, srv) => {
        s.track('6s');
        await s.step('Anna picks up on the server', () => { srv.act(ANNA, encodeAction({ kind: 'pickup' })); });
        srv.lose(ME);   // the pickup's push to me is lost
        await s.step('tap attack 6s', () => tap(probe.anim.attack(cards('6s'))));
        await s.advance(25);
        const landing = flights(s.host);
        assert.equal(landing.length, 1, 'the six flies to the table');
        await s.advance(125);
        await answer(s, 'server rejects mine');
        // The six lands at 500 and the return flight opens one kernel GAP later
        // (anim_plan.h ANIM_GAP_MS), on the first animation frame at or after it.
        await s.advance(378);
        // The flight has landed on a table that will never show the six: it flies home from where it landed...
        assert.deepEqual(flights(s.host), [{ ...landing[0], scale: 1.8, red: true }], 'the return flight starts where and as the six landed');
        await s.advance(25);
        // ... to its own place in my hand, which kept it (hidden) all along.
        const home = handCard(s.host, '6s').getBoundingClientRect();
        // A flight is hung by its CENTRE: `left`/`top` ARE the point the overlay
        // measured, and FlightCard's own `translate(-50%, -50%)` takes off the
        // half card. This used to subtract a half card here too, from a 70x90
        // card that does not exist - CardFace draws 50x70 - so the expectation
        // and the code were wrong by the same 10px in the same direction.
        assert.deepEqual(flights(s.host), [{ left: home.left + home.width / 2, top: home.top + home.height / 2, scale: 1.8, red: true }],
            'and lands on its own place in my hand');
        await s.advance(2500);
        const view = JSON.parse(probe.store).view;
        assert.ok(view.myHand.some((c: any) => c.suit === 0 && c.value === 5), 'the refused card is back in my hand');
        assert.equal(view.battles.length, 0, 'and the page has caught up with the pickup it never heard about: a refusal reconciles');
    });
});

test('a rejected move refused after my card has landed, its push never arriving: the card goes home and stays there', async () => {
    const board = threeMeFirst().hand(0, '6s Tc Jd').hand(1, 'Js Qs Ks As').hand(2, '9d Qd 6d').table('6h')
        .attacker(0).defender(1).build();
    await play('rejected_late_push_lost', 119, 'a-reject-late', board, async (s, srv) => {
        s.track('6s');
        await s.step('Anna picks up on the server', () => { srv.act(ANNA, encodeAction({ kind: 'pickup' })); });
        srv.lose(ME);   // the pickup's push to me is lost
        await s.step('tap attack 6s', () => tap(probe.anim.attack(cards('6s'))));
        await s.advance(900);
        await answer(s, 'server rejects mine');
        await s.advance(2500);
        const view = JSON.parse(probe.store).view;
        assert.ok(view.myHand.some((c: any) => c.suit === 0 && c.value === 5), 'the refused card is back in my hand');
        assert.equal(view.battles.length, 0, 'and the page has caught up with the pickup it never heard about: a refusal reconciles');
    });
});

test('a refused card comes home unselected: the next pick is a pick of its own, and Attack is offered for it', async () => {
    const board = two(0).hand(0, '6s 6c Qd Ad').hand(1, '7h 8h 9h Th').table('6h').attacker(0).defender(1).build();
    await play('refused_selection', 120, 'a-refused-pick', board, async (s, srv) => {
        await tapCard(s, '6s');
        assert.deepEqual(selectedInHand(s.host), ['0-5'], 'the tap selects the six');
        const attack = attackButton(s.host);
        assert.ok(attack, 'Attack is offered for the six');
        await s.step('click Attack', () => { attack!.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true })); });
        await s.advance(200);
        // The server refuses the throw-in as aimed at a round that closed first.
        const req = pending.shift();
        assert.ok(req && req.kind === 'action', 'the attack was sent');
        await s.step('server refuses mine: stale round', async () => {
            const decoded = fixtureTable().requestDecode(new Uint8Array(await (req!.body as Blob).arrayBuffer()));
            assert.ok(typeof decoded !== 'number', 'the request decodes');
            const r = srv.op(ME, (t) => t.act(ME, decoded.wire, 0, 1));
            assert.equal(r.rc, L.TABLE_STALE_ROUND, 'the server refuses it as stale');
            const response = fixtureTable().actionResponse(r.rc, r.reject, srv.version);
            req!.resolve({ data: response.buffer.slice(response.byteOffset, response.byteOffset + response.byteLength), error: null });
        });
        await s.advance(1500);
        assert.deepEqual(selectedInHand(s.host), [], 'the refused six came home unselected');
        await tapCard(s, '6c');
        assert.deepEqual(selectedInHand(s.host), ['2-5'], 'the next tap selects only its own card');
        assert.ok(attackButton(s.host), 'and Attack is offered for it');
    });
});

test('a rejected move, the push first: the pickup lands before the refusal', async () => {
    const board = threeMeFirst().hand(0, '6s Tc Jd').hand(1, 'Js Qs Ks As').hand(2, '9d Qd 6d').table('6h')
        .attacker(0).defender(1).build();
    await play('rejected_push_first', 108, 'a-reject-2', board, async (s, srv) => {
        s.track('6s');
        await s.step('Anna picks up on the server', () => { srv.act(ANNA, encodeAction({ kind: 'pickup' })); });
        await s.step('tap attack 6s', () => tap(probe.anim.attack(cards('6s'))));
        await s.advance(150);
        await deliver(s, 'push: Anna\'s pickup');
        await s.advance(250);
        await answer(s, 'server rejects mine');
    });
});

test('flicker B: the defender picks my card up at once, and the newer push arrives first', async () => {
    const board = two(0).hand(0, '6s 6h Qd Ad 9c Tc').hand(1, '7h 8h 9h Th Jh Qh').attacker(0).defender(1).build();
    await play('flicker_pickup_newer_first', 109, 'flicker-b', board, async (s, srv) => {
        await s.step('tap attack 6s', () => tap(probe.anim.attack(cards('6s'))));
        await s.advance(100);
        await answer(s, 'server applies mine');
        await s.step('Anna picks up on the server', () => { srv.act(ANNA, encodeAction({ kind: 'pickup' })); });
        const mine = srv.take(ME), theirs = srv.take(ME);
        await s.advance(150);
        await deliver(s, 'push: Anna\'s pickup', theirs);
        await s.advance(50);
        await deliver(s, 'push: my attack, stale', mine);
    });
});

test('flicker B, in order: my attack\'s push, then the pickup that sweeps it', async () => {
    const board = two(0).hand(0, '6s 6h Qd Ad 9c Tc').hand(1, '7h 8h 9h Th Jh Qh').attacker(0).defender(1).build();
    await play('flicker_pickup_in_order', 110, 'flicker-b2', board, async (s, srv) => {
        await s.step('tap attack 6s', () => tap(probe.anim.attack(cards('6s'))));
        await s.advance(100);
        await answer(s, 'server applies mine');
        await s.step('Anna picks up on the server', () => { srv.act(ANNA, encodeAction({ kind: 'pickup' })); });
        await s.advance(700);
        await deliver(s, 'push: my attack');
        await s.advance(20);
        await deliver(s, 'push: Anna\'s pickup');
    });
});

test('flicker A: a concurrent throw-in lands after my optimistic board, without my card', async () => {
    const board = threeMeFirst().hand(0, '7d Tc Jd').hand(1, 'Js Qs Ks 6s As').hand(2, '9d Qd 6d').table('7h/9h')
        .attacker(0).defender(1).build();
    await play('flicker_concurrent', 111, 'flicker-a', board, async (s, srv) => {
        await s.step('tap attack 7d', () => tap(probe.anim.attack(cards('7d'))));
        await s.step('Boris throws in 9d on the server', () => { srv.act(BORIS, encodeAction({ kind: 'attack', cards: cards('9d') })); });
        await s.advance(520);
        await deliver(s, 'push: Boris\'s throw-in');
        await s.advance(30);
        await answer(s, 'server applies mine');
        await s.advance(200);
        await deliver(s, 'push: my throw-in');
    });
});

test('a pass the next defender cannot hold: a throw-in overtakes it', async () => {
    const board = three().hand(0, '7c Tc Jd Qd').hand(1, '7d 8d Ad').hand(2, 'Js Qs').table('7h').attacker(0).defender(1).build();
    await play('pass_overtaken', 112, 'a-pass-overtaken', board, async (s, srv) => {
        await s.step('tap pass 7d', () => tap(probe.anim.pass(cards('7d'))));
        await s.step('Anna throws in 7c on the server', () => { srv.act(ANNA, encodeAction({ kind: 'attack', cards: cards('7c') })); });
        await s.advance(200);
        await deliver(s, 'push: Anna\'s throw-in');
        await s.advance(100);
        await answer(s, 'server rejects mine');
    });
});

test('a throw-in lands while my pass is pending, and the pass still stands', async () => {
    const board = three().hand(0, '7c Tc Jd Qd').hand(1, '7d 8d Ad').hand(2, 'Js Qs Ks As').table('7h').attacker(0).defender(1).build();
    await play('pass_pending_throw_in', 117, 'a-pass-pending', board, async (s, srv) => {
        await s.step('tap pass 7d', () => tap(probe.anim.pass(cards('7d'))));
        await s.step('Anna throws in 7c on the server', () => { srv.act(ANNA, encodeAction({ kind: 'attack', cards: cards('7c') })); });
        await s.advance(200);
        await deliver(s, 'push: Anna\'s throw-in');
        await s.advance(100);
        await answer(s, 'server applies mine');
        await s.advance(700);
        await deliver(s, 'push: my pass');
    });
});

// A PASS IS A CARD GOING OUT AND THE SHIELD GOING ON, so a pass has the
// out-home-out stutter twice over: the card can fly home and out again, and so
// can the shield. The two cases below are the shield's, and they are the ones
// the pass's pending state was for. Where a board my pass has not been
// confirmed on is asked of the KERNEL (client_optimistic_apply), a board that
// already shows the pass's cards comes back as the server wrote it and one that
// does not gets the shield handed on, so there is no board on which the page can
// show my pass's card on the table with the shield still on me.
test('a pass whose confirmation is late: the shield moves on once and stays', async () => {
    const board = three().hand(0, '7c Tc Jd Qd').hand(1, '7d 8d Ad').hand(2, 'Js Qs Ks As').table('7h').attacker(0).defender(1).build();
    await play('pass_confirmation_late', 121, 'a-pass-late', board, async (s, srv) => {
        s.track('7d');
        await s.step('tap pass 7d', () => tap(probe.anim.pass(cards('7d'))));
        // The prediction LANDS first, which is what makes this case different from
        // the throw-in one below it: the shield is already on Boris on screen when
        // the push composed without my pass arrives over it.
        await s.advance(700);
        await s.step('Anna throws in 7c on the server, before my pass reaches it', () => { srv.act(ANNA, encodeAction({ kind: 'attack', cards: cards('7c') })); });
        await s.advance(50);
        await deliver(s, 'push: Anna\'s throw-in, composed without my pass');
        await s.advance(700);
        await answer(s, 'server applies mine');
        await s.advance(100);
        await deliver(s, 'push: my pass, late');
        await s.advance(1200);
        assert.deepEqual(s.shield, [1, 2], 'the shield moved on when my pass landed and never came back');
        assert.deepEqual(s.boards.filter((b) => b.defender === 1 && b.table.includes('7d')), [],
            'and no board showed my pass\'s card on the table under my own shield');
    });
});

test('a pass refused after its flight landed: the shield goes home once', async () => {
    const board = three().hand(0, '7c Tc Jd Qd').hand(1, '7d 8d Ad').hand(2, 'Js Qs').table('7h').attacker(0).defender(1).build();
    await play('pass_refused_after_landing', 122, 'a-pass-refused', board, async (s, srv) => {
        s.track('7d');
        await s.step('tap pass 7d', () => tap(probe.anim.pass(cards('7d'))));
        await s.advance(700);
        // Anna fills the table, so the seat my pass hands the shield to cannot
        // hold it: the server refuses a move whose flight has already landed.
        await s.step('Anna throws in 7c on the server', () => { srv.act(ANNA, encodeAction({ kind: 'attack', cards: cards('7c') })); });
        await s.advance(50);
        await answer(s, 'server rejects mine');
        await s.advance(1500);
        await deliver(s, 'push: Anna\'s throw-in');
        await s.advance(1200);
        assert.deepEqual(s.shield, [1, 2, 1], 'out with the pass, home with the refusal, and no further');
        assert.deepEqual(s.boards.filter((b) => b.defender === 1 && b.table.includes('7d')), [],
            'and no board showed my pass\'s card on the table under my own shield');
    });
});

test('a pickup a closing good overtakes', async () => {
    const board = three().hand(0, '9c Tc').hand(1, 'Js Qs Ks').hand(2, 'Ad Qd 6d').table('7h/9h')
        .attacker(0).defender(1).good(0).goodTimestamp().build();
    await play('pickup_overtaken_by_good', 113, 'a-pickup-good', board, async (s, srv) => {
        await s.step('tap pickup', () => tap(probe.anim.pickup()));
        await s.step('Boris says good on the server', () => { srv.act(BORIS, encodeAction({ kind: 'good' })); });
        await s.advance(200);
        await deliver(s, 'push: the bout closes');
        await s.advance(100);
        await answer(s, 'server answers mine');
    });
});

test('a resync while my card is pending keeps it on the table', async () => {
    const board = two(0).hand(0, '6s 6h Qd Ad 9c Tc').hand(1, '7h 8h 9h Th Jh Qh').attacker(0).defender(1).build();
    await play('resync_pending', 114, 'a-resync', board, async (s) => {
        await s.step('tap attack 6s', () => tap(probe.anim.attack(cards('6s'))));
        await s.advance(600);
        await s.step('the page reloads the game', () => tap(probe.actions.loadGame('a-resync')));
        await s.advance(300);
        await answer(s, 'server applies');
        await s.advance(100);
        await deliver(s, 'push: my attack');
    });
});

test('the rematch: continue from the win screen', async () => {
    const board = fixture().title('Finished').seats([seat(ME, 'Me'), seat(ANNA, 'Anna')]).status(GAME_OVER).powerSuit(1)
        .hand(1, '6s 7s').seatStatus(0, OUT).seatStatus(1, IN).eliminated(0).discard(34).build();
    await play('continue', 115, 'a-continue', board, async (s) => {
        await s.step('tap continue', () => tap(probe.actions.continueGame('a-continue')));
        await s.advance(200);
        await answer(s, 'server resets the table');
        await s.advance(100);
        await deliver(s, 'push: back to the lobby');
    });
});

test('a hand rearrange', async () => {
    const board = two(0).hand(0, '6s 6h Qd Ad 9c Tc').hand(1, '7h 8h 9h Th Jh Qh').attacker(0).defender(1).build();
    await play('rearrange_hand', 116, 'a-rearrange', board, async (s) => {
        await s.step('rearrange', () => tap(probe.actions.rearrangeHand('a-rearrange', [5, 1, 2, 3, 4, 0])));
        await s.advance(200);
        await s.step('a stale rearrange', () => tap(probe.actions.rearrangeHand('a-rearrange', [0, 1, 9])));
        await answer(s, 'server applies');
    });
});

// ---- the tutorial ------------------------------------------------------------------
// The tutorial replays a frozen game from the learner's seat and waits for the
// learner's own moves. Its first seconds are held to two invariants on EVERY React
// commit (a Profiler reports each one, so a board shown for a single frame is
// caught, not only the frames a timer leaves behind):
//   - the deal lands on an empty hand: no commit shows a card in the learner's
//     hand while the stock still shows the whole deck;
//   - the move hint waits for the board it points at: no commit shows the hint
//     before the deal it follows has started to fly.
test('the tutorial: the deal lands on an empty hand, and the move hint waits for the deal', async () => {
    const React = (await import('react')).default;
    const { act } = await import('react');
    const { createRoot } = await import('react-dom/client');
    const h = React.createElement;
    const { LocalizationProvider } = await import('../src/contexts/LocalizationContext.tsx');
    const { ThemeProvider } = await import('../src/contexts/ThemeContext.tsx');
    const { StyleProvider } = await import('../src/contexts/StyleContext.tsx');
    const { TextureProvider } = await import('../src/components/TexturedSurface.tsx');
    const { Tutorial } = await import('../src/components/Tutorial.tsx');
    const { ensureBotsAsync } = await import('../sdk/ts/wasm/bots.ts');
    await ensureBotsAsync();
    routeGameId = '';
    seedRandom(211);
    installClock();

    const host = dom.window.document.createElement('div');
    dom.window.document.body.appendChild(host);
    interface Commit { t: number; hand: number; fullStock: boolean; hinted: boolean; flying: number }
    const commits: Commit[] = [];
    const record = () => {
        const html = host.innerHTML;
        const overlay = Array.from(host.querySelectorAll('div')).find((d) => d.style.position === 'fixed' && d.style.zIndex === '10000');
        const state = host.querySelector('[data-testid="tut-state"]');
        commits.push({
            t: clock,
            hand: host.querySelectorAll('[data-hand-container] [data-location="hand"][data-card]').length,
            fullStock: html.includes('>36<'),
            hinted: !!state && state.getAttribute('data-action') !== '' || html.includes('tut-move') || html.includes('rgb(47, 207, 99)'),
            flying: overlay ? overlay.children.length : 0,
        });
    };
    const root = createRoot(host);
    const doAct = async (fn: () => unknown) => { await act(async () => { await fn(); }); };
    const settle = async () => { for (let i = 0; i < 8; i++) await doAct(() => new Promise<void>((r) => setImmediate(r))); };
    try {
        await doAct(() => root.render(h(React.Profiler, { id: 'tutorial', onRender: record },
            h(LocalizationProvider, null, h(ThemeProvider, null, h(StyleProvider, null, h(TextureProvider, null, h(Tutorial))))))));
        await settle();
        const start = host.querySelector('[data-testid="tut-start"]');
        assert.ok(start, 'the intro card offers a start');
        await doAct(() => { start!.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true })); });
        await settle();
        // Past the deal and the lead's first prompt: fire every timer due, one at a time.
        for (let end = clock + 6000; ;) {
            let next: Timer | null = null;
            for (const t of timers.values()) if (t.due <= end && (!next || t.due < next.due || (t.due === next.due && t.id < next.id))) next = t;
            if (!next) break;
            clock = next.due;
            if (next.every > 0) next.due += next.every; else timers.delete(next.id);
            const fire = next;
            await doAct(() => { fire.fn(...fire.args); });
            await settle();
        }
    } finally {
        await doAct(() => { root.unmount(); });
        host.remove();
        removeClock();
    }

    assert.ok(commits.some((c) => c.hand === 6), 'the learner is dealt a hand');
    assert.ok(commits.some((c) => c.hinted), 'the learner is prompted for the lead');
    const early = commits.filter((c) => c.hand > 0 && c.fullStock);
    assert.deepEqual(early.map((c) => `${c.t}ms: ${c.hand} cards`), [], 'no commit shows a card in the learner\'s hand before the deal leaves the stock');
    const firstFlight = commits.findIndex((c) => c.flying > 0);
    const firstHint = commits.findIndex((c) => c.hinted);
    assert.ok(firstFlight >= 0, 'the deal flies');
    assert.ok(firstHint > firstFlight, `the hint first shows at commit ${firstHint} (${commits[firstHint]?.t}ms), after the deal starts to fly at commit ${firstFlight} (${commits[firstFlight]?.t}ms)`);
});
