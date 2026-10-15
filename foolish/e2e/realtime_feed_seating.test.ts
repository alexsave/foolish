// E2E for WHO holds which cast stream: the real ServerProvider and the real
// RealtimeAnimationFeed rendered together in jsdom, with only the backend seam
// (src/common/backend.ts), auth and route stubbed. The game bytes the stub
// serves are real envelopes the kernel writes (table_envelope) for C-built
// lobby rows.
//
// A game's cast stream is one subscription the server answers by membership:
// a member hears their own seat's casts, anyone else the spectators'. The
// feed holds it only while the user holds a seat (a spectator's is
// ServerContext's), so a user who takes a seat from the page (auto-join of an
// open lobby) must see that seat land in client state from the join
// response, or the feed would never hold their seat's stream.
//
// Needs --experimental-test-module-mocks (see the test:e2e script).

import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';
import { MAX_PLAYERS } from '../sdk/ts/gen/view_layout.bots.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { fixture, fixtureTable, type FixtureSeat } from './helpers/table_fixture.ts';
import { askKindName } from './helpers/ask.ts';
import { fakeBackend } from './helpers/fake_backend.ts';

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

// What the stubbed backend holds, per test.
let routeGame = FULL;
const fb = fakeBackend({ user: { id: ME, username: 'ME' } });
let joinResponse: Uint8Array | null = null;
fb.onMeta = async (ask) => {
    if (askKindName(ask) === 'join' && joinResponse) return joinResponse;
    throw new Error('refused');
};
const feedHolds = () => fb.holdLog.filter((h) => h.startsWith('game:'));
const asked = (kind: string) => fb.calls.some((c) => c.fn === 'meta' && askKindName(c.arg as Uint8Array) === kind);

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

test('a spectator holds the game\'s stream once, from the page, never from the feed', async () => {
    // A full lobby: the page loads it as a spectator and cannot auto-join.
    const full = [human(HOST, 'Host'), ...Array.from({ length: MAX_PLAYERS - 1 }, (_, i) => bot(i))];
    routeGame = FULL;
    fb.views = { [FULL]: lobbyEnvelope(FULL, full, -1, 3) };
    joinResponse = null;
    fb.calls.length = 0; fb.holdLog.length = 0;

    const unmount = await render();
    await sleep(600);
    assert.ok(!asked('join'), 'the full lobby was not auto-joined (fixture sanity)');
    assert.deepEqual(feedHolds(), [`game:${FULL}`], 'one hold of the game: the spectator\'s, from ServerContext');
    await unmount();
    assert.equal(fb.held.get(`game:${FULL}`), undefined, 'and it is let go on unmount');
});

test('taking a seat from the page holds the seat\'s stream from the feed', async () => {
    // An open lobby: the page auto-joins it, and the join response is the
    // joiner's own seated view.
    routeGame = OPEN;
    fb.views = { [OPEN]: lobbyEnvelope(OPEN, [human(HOST, 'Host')], -1, 3) };
    joinResponse = lobbyEnvelope(OPEN, [human(HOST, 'Host'), human(ME, 'Me')], 1, 4);
    fb.calls.length = 0; fb.holdLog.length = 0;

    const unmount = await render();
    for (let i = 0; i < 40 && feedHolds().length === 0; i++) await sleep(50);
    assert.ok(asked('join'), 'the open lobby was auto-joined');
    assert.deepEqual(feedHolds(), [`game:${OPEN}`], 'the seat\'s own stream, held once');
    assert.equal(fb.held.get(`game:${OPEN}`), 1, 'and still held');
    await unmount();
});
