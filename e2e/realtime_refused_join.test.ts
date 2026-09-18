// E2E for the page's private Realtime channels when a join is REFUSED: the real
// ServerProvider and RealtimeAnimationFeed rendered together in jsdom, against a
// supabase client stub that behaves as realtime-js and Realtime do on a refusal.
//
// What a refusal costs, measured on the local stack (Realtime v2.129): Realtime
// decides a private join in milliseconds but replies with the refusal about five
// seconds later, and answers nothing else on that websocket meanwhile. Every
// channel the page holds shares that one socket, so each refused join delays
// every other join and every broadcast on it by up to five seconds. A signed-in
// spectator's page joined chat:{game} (a members-only topic) beside game-{game},
// and 18 of 40 game- pushes arrived 1 to 5 s late (median 890 ms, against 8 ms
// without the chat: join). realtime-js rejoins an errored channel by itself
// (1 s, 2 s, 5 s, then every 10 s) for as long as the channel object lives, and
// hands the SAME errored object back from supabase.channel(topic), whose
// subscribe() then does nothing: a page that retries a refused channel without
// removing it leaves realtime-js's endless rejoin as the only retry.
//
// So the page must (1) not join a private channel the policy refuses it (a
// spectator's chat:), and (2) remove a refused channel before anything else, so
// that no join is ever issued by realtime-js behind the page's back.
//
// Needs --experimental-test-module-mocks (see the test:e2e script).

import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';
import { MAX_PLAYERS } from '../server/api/core/constants.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixture, fixtureTable, type FixtureSeat } from './helpers/table_fixture.ts';

const dom = new JSDOM('<!DOCTYPE html><html><body><div id="root"></div></body></html>', { url: 'http://localhost/' });
const g = globalThis as any;
g.window = dom.window;
g.document = dom.window.document;
try { Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true }); } catch { /* already a getter */ }
g.HTMLElement = dom.window.HTMLElement;
g.IS_REACT_ACT_ENVIRONMENT = true;
if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.error = () => {}; console.debug = () => {}; }

const ME = '60771e41-70e6-4e61-b077-00000000001a';
const HOST = '60771e41-70e6-4e61-b077-00000000001b';
const human = (id: string, name: string): FixtureSeat => ({ id, name });
const bot = (i: number): FixtureSeat => ({ id: `60771e41-70e6-4e61-b077-0000000002${String(i).padStart(2, '0')}`, name: `%Bot${i}`, brain: 'random' });

function envelope(id: string, seats: FixtureSeat[], viewer: number, version: number): Uint8Array {
    const fx = fixture().title(id).seats(seats).build();
    const table = fixtureTable();
    assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK, 'the lobby loads');
    const env = table.envelope(id, viewer, version);
    if (typeof env === 'number') throw new Error(`envelope refused (${env})`);
    return env;
}
const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');

// ---- the backend rows ------------------------------------------------------------
let routeGame = '';
let playerViews: Record<string, string> = {};
let spectatorViews: Record<string, string> = {};

function table(name: string) {
    let gameId: string | null = null;
    const result = () => {
        if (name === 'player_views' && gameId && playerViews[gameId]) return { data: { view: playerViews[gameId] }, error: null };
        if (name === 'spectator_views' && gameId && spectatorViews[gameId]) return { data: { view: spectatorViews[gameId] }, error: null };
        if (name === 'chat_messages') return { data: [], error: null };
        return { data: null, error: null };
    };
    const chain: any = {
        select: () => chain, order: () => chain, limit: () => chain, in: () => chain, neq: () => chain,
        eq: (col: string, v: string) => { if (col === 'game_id') gameId = v; return chain; },
        maybeSingle: async () => result(),
        single: async () => result(),
        then: (ok: (r: any) => void, err?: (e: any) => void) => Promise.resolve(result()).then(ok, err),
    };
    return chain;
}

// ---- realtime, as realtime-js and Realtime behave on a refusal ----------------------
/** How many more joins of a topic Realtime refuses (Infinity: always). */
let refuse: Record<string, number> = {};
interface Join { topic: string; bySelf: boolean; admitted: boolean }
const joinLog: Join[] = [];
const REJOIN_MS = [1000, 2000, 5000, 10000];   // realtime-js's own rejoin backoff

class FakeChannel {
    state: 'closed' | 'joining' | 'joined' | 'errored' = 'closed';
    removed = false;
    private cb: ((s: string, e?: unknown) => void) | null = null;
    private rejoinTimer: ReturnType<typeof setTimeout> | null = null;
    private tries = 0;
    constructor(readonly topic: string, readonly isPrivate: boolean) {}
    on() { return this; }
    subscribe(cb?: (s: string, e?: unknown) => void) {
        // realtime-js: only a closed channel subscribes; an errored one is left to its rejoin timer.
        if (this.state !== 'closed') return this;
        this.cb = cb ?? null;
        this.join(false);
        return this;
    }
    private join(bySelf: boolean) {
        this.state = 'joining';
        const left = refuse[this.topic] ?? 0;
        const admitted = !this.isPrivate || left <= 0;
        if (!admitted) refuse[this.topic] = left - 1;
        joinLog.push({ topic: this.topic, bySelf, admitted });
        queueMicrotask(() => {
            if (this.removed) return;
            if (admitted) { this.state = 'joined'; this.tries = 0; this.cb?.('SUBSCRIBED'); return; }
            this.state = 'errored';
            this.cb?.('CHANNEL_ERROR', new Error(`Unauthorized: You do not have permissions to read from this Channel topic: ${this.topic}`));
            if (this.removed) return;
            const wait = REJOIN_MS[Math.min(this.tries++, REJOIN_MS.length - 1)];
            this.rejoinTimer = setTimeout(() => { if (!this.removed) this.join(true); }, wait);
        });
    }
    remove() {
        this.removed = true;
        if (this.rejoinTimer) clearTimeout(this.rejoinTimer);
        const was = this.state;
        this.state = 'closed';
        if (was !== 'closed') queueMicrotask(() => this.cb?.('CLOSED'));
    }
}
let channels: FakeChannel[] = [];

const supabaseMock = {
    from: (name: string) => table(name),
    rpc: async () => ({ data: null, error: null }),
    realtime: { setAuth: async () => {} },
    getChannels: () => channels.filter((c) => !c.removed),
    removeChannel: async (ch: FakeChannel) => { ch.remove(); return 'ok'; },
    functions: { invoke: async () => ({ data: null, error: null }) },
    channel: (topic: string, opts?: { config?: { private?: boolean } }) => {
        // realtime-js hands back the live channel of that topic if there is one.
        const live = channels.find((c) => c.topic === topic && !c.removed);
        if (live) return live;
        const ch = new FakeChannel(topic, opts?.config?.private === true);
        channels.push(ch);
        return ch;
    },
};

mock.module('next/navigation', { namedExports: {
    useParams: () => ({ game_id: routeGame }),
    useRouter: () => ({ push: () => {}, replace: () => {} }),
} });
mock.module('../src/contexts/AuthContext.tsx', { namedExports: { useAuth: () => ({ user_id: ME }) } });
mock.module('../src/backend/Connector.ts', { defaultExport: supabaseMock });

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function render() {
    const React = (await import('react')).default;
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { ServerProvider } = await import('../src/contexts/ServerContext.tsx');
    const { RealtimeAnimationFeed } = await import('../src/state/RealtimeAnimationFeed.tsx');
    const root = createRoot(dom.window.document.getElementById('root')!);
    await act(async () => {
        root.render(React.createElement(ServerProvider, null, React.createElement(RealtimeAnimationFeed)));
    });
    return async () => { await act(async () => { root.unmount(); }); };
}

function reset() {
    joinLog.length = 0;
    for (const c of channels) c.remove();
    channels = [];
    refuse = {};
    playerViews = {};
    spectatorViews = {};
}

test('a signed-in spectator joins no private channel Realtime refuses', async () => {
    reset();
    const GAME = 'c0ffee';
    const full = [human(HOST, 'Host'), ...Array.from({ length: MAX_PLAYERS - 1 }, (_, i) => bot(i))];
    routeGame = GAME;
    spectatorViews = { [GAME]: hex(envelope(GAME, full, -1, 3)) };
    // The policies: game- admits any signed-in user; chat: and gu- admit only a member.
    refuse = { [`chat:${GAME}`]: Infinity, [`gu-${GAME}-${ME}`]: Infinity };

    const unmount = await render();
    await sleep(1500);
    const refused = joinLog.filter((j) => !j.admitted).map((j) => j.topic);
    assert.ok(joinLog.some((j) => j.topic === `game-${GAME}` && j.admitted), 'the spectator stream was joined (fixture sanity)');
    assert.deepEqual(refused, [], 'no refused join: each one stalls every channel on the socket for five seconds');
    await unmount();
});

test('a seat whose membership Realtime does not see yet: each refused channel is removed, and only the page rejoins', async () => {
    reset();
    const GAME = 'bead00';
    routeGame = GAME;
    playerViews = { [GAME]: hex(envelope(GAME, [human(HOST, 'Host'), human(ME, 'Me')], 1, 4)) };
    // The first two joins of each members-only topic are refused, then the membership row is visible.
    refuse = { [`chat:${GAME}`]: 2, [`gu-${GAME}-${ME}`]: 2 };

    const unmount = await render();
    for (let i = 0; i < 80; i++) {
        if (channels.some((c) => c.topic === `chat:${GAME}` && c.state === 'joined') && channels.some((c) => c.topic === `gu-${GAME}-${ME}` && c.state === 'joined')) break;
        await sleep(50);
    }
    // Past the moment realtime-js's own first rejoin would have fired on a channel left errored.
    await sleep(1200);
    const bySelf = joinLog.filter((j) => j.bySelf).map((j) => j.topic);
    assert.deepEqual(bySelf, [], 'no join was issued by realtime-js on a refused channel the page did not remove');
    for (const topic of [`chat:${GAME}`, `gu-${GAME}-${ME}`]) {
        assert.ok(channels.some((c) => c.topic === topic && !c.removed && c.state === 'joined'), `${topic} is joined once the membership is visible`);
        assert.equal(channels.filter((c) => c.topic === topic && c.state === 'errored' && !c.removed).length, 0, `no errored ${topic} channel is left on the socket`);
    }
    await unmount();
});
