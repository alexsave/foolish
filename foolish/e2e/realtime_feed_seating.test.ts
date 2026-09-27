// E2E for WHO joins the per-seat animation channel: the real ServerProvider and
// the real RealtimeAnimationFeed rendered together in jsdom, with only the
// supabase client, auth and route stubbed. The game bytes the stubs serve are
// real envelopes the kernel writes (table_envelope) for C-built lobby rows.
//
// `gu-{game}-{user}` is a private channel that Realtime admits only for a member
// of the game. A spectator is not one, so a spectator's join is refused every
// time, and the feed used to retry it every 500ms for as long as the game stayed
// open. The feed now joins only while the user holds a seat, which also means a
// user who takes a seat from the page (auto-join of an open lobby) must see that
// seat land in client state from the join response, or they would never join.
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

const ME = '60771e41-70e6-4e61-b077-00000000000a';
const HOST = '60771e41-70e6-4e61-b077-00000000000b';
const FULL = 'f011aa';
const OPEN = '0be11a';

const human = (id: string, name: string): FixtureSeat => ({ id, name });
const bot = (i: number): FixtureSeat => ({ id: `60771e41-70e6-4e61-b077-0000000001${String(i).padStart(2, '0')}`, name: `%Bot${i}`, brain: 'random' });

/** The envelope the kernel serves `viewer` (-1: a spectator) of a lobby seating `seats`, at `version`. */
function lobbyEnvelope(id: string, seats: FixtureSeat[], viewer: number, version: number): Uint8Array {
    const fx = fixture().title(id).seats(seats).build();
    const table = fixtureTable();
    assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK, 'the lobby loads');
    const env = table.envelope(id, viewer, version);
    if (typeof env === 'number') throw new Error(`envelope refused (${env})`);
    return env;
}

const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');

// What the stubbed backend holds, per test.
let routeGame = FULL;
let spectatorViews: Record<string, string> = {};
let joinResponse: Uint8Array | null = null;
const joins: string[] = [];
const invokes: { fn: string; type?: string }[] = [];

// A PostgREST-ish chain: every filter returns the chain, and the terminal calls
// resolve with the table's row for the game named in .eq('game_id', ...).
function table(name: string) {
    let gameId: string | null = null;
    const result = () => {
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

const supabaseMock = {
    from: (name: string) => table(name),
    rpc: async () => ({ data: null, error: null }),
    realtime: { setAuth: async () => {} },
    getChannels: () => [],
    removeChannel: async () => 'ok',
    functions: {
        invoke: async (fn: string, opts: { body: any }) => {
            invokes.push({ fn, type: opts?.body?.type });
            if (fn === 'meta' && opts.body?.type === 'join' && joinResponse) {
                return { data: new Blob([joinResponse as Uint8Array<ArrayBuffer>]), error: null };
            }
            return { data: null, error: null };
        },
    },
    channel: (topic: string) => {
        const ch: any = {
            topic,
            on: () => ch,
            subscribe: (cb?: (s: string) => void) => {
                if (topic.startsWith('gu-')) joins.push(topic);
                if (cb) queueMicrotask(() => cb('SUBSCRIBED'));
                return ch;
            },
        };
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

test('a spectator never joins the per-seat gu- channel', async () => {
    // A full lobby: the page loads it as a spectator and cannot auto-join.
    const full = [human(HOST, 'Host'), ...Array.from({ length: MAX_PLAYERS - 1 }, (_, i) => bot(i))];
    routeGame = FULL;
    spectatorViews = { [FULL]: hex(lobbyEnvelope(FULL, full, -1, 3)) };
    joinResponse = null;
    joins.length = 0; invokes.length = 0;

    const unmount = await render();
    // Long enough for several 500ms retries of a refused join.
    await sleep(1600);
    assert.ok(!invokes.some((i) => i.type === 'join'), 'the full lobby was not auto-joined (fixture sanity)');
    assert.deepEqual(joins, [], 'no gu- join for a user without a seat');
    await unmount();
});

test('taking a seat from the page joins the gu- channel for that seat', async () => {
    // An open lobby: the page auto-joins it, and the join response is the
    // joiner's own seated view.
    routeGame = OPEN;
    spectatorViews = { [OPEN]: hex(lobbyEnvelope(OPEN, [human(HOST, 'Host')], -1, 3)) };
    joinResponse = lobbyEnvelope(OPEN, [human(HOST, 'Host'), human(ME, 'Me')], 1, 4);
    joins.length = 0; invokes.length = 0;

    const unmount = await render();
    for (let i = 0; i < 40 && joins.length === 0; i++) await sleep(50);
    assert.ok(invokes.some((i) => i.fn === 'meta' && i.type === 'join'), 'the open lobby was auto-joined');
    assert.deepEqual(joins, [`gu-${OPEN}-${ME}`], 'joined exactly the seat\'s own gu- topic');
    await unmount();
});
