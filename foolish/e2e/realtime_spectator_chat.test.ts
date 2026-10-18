// E2E for which streams a page holds for a game, by seat: the real ServerProvider
// and RealtimeAnimationFeed rendered together in jsdom, against the backend
// seam stubbed (e2e/helpers/fake_backend.ts).
//
// A game's chat admits only its members. A spectator's page must not ask for it:
// the server refuses it, and the client would retry the refusal for as long as
// the page stays open. A seated player's page holds both the game and its chat.
//
// (This file was realtime_refused_join.test.ts, which also held the page's own
// retry loop for a refused Phoenix channel. That loop is gone: the retry of a
// refused subscription is the kernel's client's, held by
// foolish/server/test/unit_netclient.c stream_order_and_retries and unit_stream.c.)
//
// Needs --experimental-test-module-mocks (see the test:e2e script).

import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';
import { MAX_PLAYERS } from '../sdk/ts/gen/view_layout.bots.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixture, fixtureTable, type FixtureSeat } from './helpers/table_fixture.ts';
import { fakeBackend } from './helpers/fake_backend.ts';

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

let routeGame = '';
const fb = fakeBackend({ user: { id: ME, username: 'ME' } });

mock.module('next/navigation', { namedExports: {
    useParams: () => ({ game_id: routeGame }),
    usePathname: () => `/${routeGame}`,
    useRouter: () => ({ push: () => {}, replace: () => {} }),
} });
mock.module('../src/contexts/AuthContext.tsx', { namedExports: { useAuth: () => ({ user_id: ME }) } });
mock.module('../src/common/backend.ts', { namedExports: fb.module });

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

test('a signed-in spectator holds the game and not its chat', async () => {
    const GAME = 'c0ffee';
    const full = [human(HOST, 'Host'), ...Array.from({ length: MAX_PLAYERS - 1 }, (_, i) => bot(i))];
    routeGame = GAME;
    fb.views = { [GAME]: envelope(GAME, full, -1, 3) };
    fb.holdLog.length = 0;
    const unmount = await render();
    await sleep(600);
    assert.ok(fb.holdLog.includes(`game:${GAME}`), 'the spectators\' stream is held (fixture sanity)');
    assert.ok(!fb.holdLog.includes(`chat:${GAME}`), 'the members-only chat is never asked for');
    await unmount();
});

test('a seated player holds the game and its chat', async () => {
    const GAME = 'bead00';
    routeGame = GAME;
    fb.views = { [GAME]: envelope(GAME, [human(HOST, 'Host'), human(ME, 'Me')], 1, 4) };
    fb.holdLog.length = 0;
    const unmount = await render();
    for (let i = 0; i < 40 && !fb.held.get(`chat:${GAME}`); i++) await sleep(50);
    assert.equal(fb.held.get(`game:${GAME}`), 1, 'the seat\'s stream, once');
    assert.equal(fb.held.get(`chat:${GAME}`), 1, 'and the chat, once');
    await unmount();
    assert.equal(fb.held.size, 0, 'both let go with the page');
});
