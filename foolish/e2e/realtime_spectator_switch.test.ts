// E2E for the page's spectator stream across a join and an exit: the real
// ServerProvider and RealtimeAnimationFeed rendered in jsdom, against a supabase
// client stub that names channels the way realtime-js does.
//
// realtime-js prefixes every topic: supabase.channel('game-x') is a channel whose
// `topic` is 'realtime:game-x' (RealtimeClient.channel). A page that looks its own
// channels up by the bare topic never finds them: after a join the spectator's
// game- channel stays on the socket beside the new seat's gu- stream, and every
// exit and re-join adds another.
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
const GAME = 'feed01';
let playerViews: Record<string, string> = {};
let spectatorViews: Record<string, string> = {};
let metaAnswer: Uint8Array | null = null;

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
        invoke: async (_name: string, opts: { body?: any }) => {
            if (opts?.body?.type === 'bump' || !metaAnswer) return { data: null, error: null };
            const b = metaAnswer;
            return { data: b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength), error: null };
        },
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
    useParams: () => ({ game_id: GAME }),
    useRouter: () => ({ push: () => {}, replace: () => {} }),
} });
mock.module('../src/contexts/AuthContext.tsx', { namedExports: { useAuth: () => ({ user_id: ME }) } });
mock.module('../src/backend/Connector.ts', { defaultExport: supabaseMock });

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

test('a spectator who takes a seat and gives it up holds one live game- stream, and only while watching', async () => {
    const React = (await import('react')).default;
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { ServerProvider, useServerActions } = await import('../src/contexts/ServerContext.tsx');
    const { RealtimeAnimationFeed } = await import('../src/state/RealtimeAnimationFeed.tsx');

    const watching = [human(HOST, 'Host'), ...Array.from({ length: MAX_PLAYERS - 2 }, (_, i) => bot(i))];
    const seated = [...watching, human(ME, 'Me')];
    const mine = (v: number) => envelope(GAME, seated, seated.length - 1, v);
    const theirs = (v: number) => envelope(GAME, watching, -1, v);

    let actions: any = null;
    const Probe = () => { actions = useServerActions(); return null; };
    playerViews = { [GAME]: hex(mine(3)) };
    const root = createRoot(dom.window.document.getElementById('root')!);
    await act(async () => {
        root.render(React.createElement(ServerProvider, null, React.createElement(RealtimeAnimationFeed), React.createElement(Probe)));
    });
    await act(async () => { await sleep(300); });
    assert.equal(live(`gu-${GAME}-${ME}`).length, 1, 'seated: my own gu- stream (fixture sanity)');

    // I give the seat up: watching, on one stream that carries the game.
    playerViews = {};
    spectatorViews = { [GAME]: hex(theirs(4)) };
    metaAnswer = theirs(4);
    await act(async () => { await actions.exitGame(GAME, undefined, ME); await sleep(300); });
    let watchingStreams = live(`game-${GAME}`);
    assert.equal(watchingStreams.length, 1, 'watching: one game- stream');
    assert.ok(watchingStreams[0].events.includes('animation_events'), 'and it carries the game\'s animation events, so the page keeps moving');
    assert.deepEqual(live(`gu-${GAME}-${ME}`).map((c) => c.topic), [], 'and my seat\'s gu- stream is left');

    // I take the seat again: the join answers with my seated view.
    playerViews = { [GAME]: hex(mine(5)) };
    metaAnswer = mine(5);
    await act(async () => { await actions.joinGame(GAME); await sleep(300); });
    assert.deepEqual(live(`game-${GAME}`).map((c) => c.topic), [], 'seated: the spectator stream is left, not kept beside my own');

    // And once more round: still one stream while watching, none while seated.
    metaAnswer = theirs(6);
    await act(async () => { await actions.exitGame(GAME, undefined, ME); await sleep(300); });
    watchingStreams = live(`game-${GAME}`);
    assert.equal(watchingStreams.length, 1, 'watching again: one game- stream');
    assert.ok(watchingStreams[0].events.includes('animation_events'), 'carrying the animation events');
    metaAnswer = mine(7);
    await act(async () => { await actions.joinGame(GAME); await sleep(300); });
    assert.deepEqual(live(`game-${GAME}`).map((c) => c.topic), [], 'seated again: no spectator stream left on the socket');

    await act(async () => { root.unmount(); });
});
