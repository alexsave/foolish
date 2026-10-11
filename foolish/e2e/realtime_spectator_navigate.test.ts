// E2E for the page's spectator stream across NAVIGATION: the real ServerProvider
// rendered in jsdom, watching one full table, then another, then a third, against
// a supabase client stub that names channels the way realtime-js does.
//
// The page left a game's streams from an effect cleanup that sat behind an early
// return ("the user has not changed"), so the cleanup existed for the first game
// left and for none after it: the second game's game- stream stayed subscribed
// while the page showed the third, and kept publishing into the animation feed.
//
// Needs --experimental-test-module-mocks (see the test:e2e script).

import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';
import { MAX_PLAYERS } from '../sdk/ts/gen/view_layout.bots.ts';
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

const ME = '60771e41-70e6-4e61-b077-00000000003a';
const HOST = '60771e41-70e6-4e61-b077-00000000003b';
const human = (id: string, name: string): FixtureSeat => ({ id, name });
const bot = (i: number): FixtureSeat => ({ id: `60771e41-70e6-4e61-b077-0000000004${String(i).padStart(2, '0')}`, name: `%Bot${i}`, brain: 'random' });

function envelope(id: string, seats: FixtureSeat[], viewer: number, version: number): Uint8Array {
    const fx = fixture().title(id).seats(seats).build();
    const table = fixtureTable();
    assert.equal(table.load(fx.state, fx.roster), L.TABLE_OK, 'the lobby loads');
    const env = table.envelope(id, viewer, version);
    if (typeof env === 'number') throw new Error(`envelope refused (${env})`);
    return env;
}
const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');

// ---- the backend --------------------------------------------------------------------
const GAMES = ['feed01', 'feed02', 'feed03'];
let page = GAMES[0];
const playerViews: Record<string, string> = {};
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

// ---- realtime, named as realtime-js names it -----------------------------------------
class FakeChannel {
    removed = false;
    events: string[] = [];
    constructor(readonly topic: string) {}
    on(_type: string, filter: { event?: string }) { this.events.push(filter?.event ?? ''); return this; }
    subscribe(cb?: (s: string) => void) { queueMicrotask(() => { if (!this.removed) cb?.('SUBSCRIBED'); }); return this; }
}
let channels: FakeChannel[] = [];
const live = (bare: string) => channels.filter((c) => !c.removed && c.topic === `realtime:${bare}`);

const supabaseMock = {
    from: (name: string) => table(name),
    rpc: async () => ({ data: null, error: null }),
    realtime: { setAuth: async () => {} },
    getChannels: () => channels.filter((c) => !c.removed),
    removeChannel: async (ch: FakeChannel) => { ch.removed = true; return 'ok'; },
    functions: {
        invoke: async () => ({ data: null, error: null }),
    },
    channel: (topic: string) => {
        // RealtimeClient.channel: the topic is prefixed, and a live channel of that topic is handed back.
        const full = `realtime:${topic}`;
        const existing = channels.find((c) => c.topic === full && !c.removed);
        if (existing) return existing;
        const ch = new FakeChannel(full);
        channels.push(ch);
        return ch;
    },
};

mock.module('next/navigation', { namedExports: {
    useParams: () => ({ game_id: page }),
    useRouter: () => ({ push: () => {}, replace: () => {} }),
} });
mock.module('../src/contexts/AuthContext.tsx', { namedExports: { useAuth: () => ({ user_id: ME }) } });
mock.module('../src/common/Connector.ts', { defaultExport: supabaseMock });

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

test('a spectator who goes from table to table holds the game- stream of the table on the page, and no other', async () => {
    const React = (await import('react')).default;
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { ServerProvider } = await import('../src/contexts/ServerContext.tsx');

    // Full tables I am not at: nothing to join, so each one is watched.
    const full = [human(HOST, 'Host'), ...Array.from({ length: MAX_PLAYERS - 1 }, (_, i) => bot(i))];
    spectatorViews = Object.fromEntries(GAMES.map((id) => [id, hex(envelope(id, full, -1, 3))]));

    const root = createRoot(dom.window.document.getElementById('root')!);
    const show = async (id: string) => {
        page = id;
        await act(async () => { root.render(React.createElement(ServerProvider, null, React.createElement('i', { key: id }))); });
        await act(async () => { await sleep(300); });
    };
    const watched = () => GAMES.filter((id) => live(`game-${id}`).length > 0);

    await show(GAMES[0]);
    assert.deepEqual(watched(), [GAMES[0]], 'the first table is watched (fixture sanity)');
    await show(GAMES[1]);
    assert.deepEqual(watched(), [GAMES[1]], 'on the second table: its stream, and the first table\'s is left');
    await show(GAMES[2]);
    assert.deepEqual(watched(), [GAMES[2]], 'on the third table: its stream, and the second table\'s is left too');
    await show(GAMES[0]);
    assert.deepEqual(watched(), [GAMES[0]], 'back on the first: one stream again, the one for the page');

    await act(async () => { root.unmount(); });
});
