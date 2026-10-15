// E2E for the live game's cast stream producer
// (src/state/RealtimeAnimationFeed.tsx), rendered for real in jsdom with only the
// backend seam (e2e/helpers/fake_backend.ts), auth, route and server actions
// stubbed.
//
// The server admits a seat's own stream only once the user is a member of the
// game. Opening a game you are about to join, or one whose create is still
// persisting, is exactly that window: the first subscriptions are refused and
// retried (by the kernel's client). Casts are not replayed, so every cast sent
// while the stream was refused - the joiner's own join animation among them - is
// gone. The feed must refetch authoritative state once it is finally admitted,
// the same as after a reconnect. When the very first subscription is admitted
// nothing was missed, and it must not spend a fetch.
//
// Needs --experimental-test-module-mocks (see the test:e2e script).

import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';
import { fakeBackend } from './helpers/fake_backend.ts';

const dom = new JSDOM('<!DOCTYPE html><html><body><div id="root"></div></body></html>', { url: 'http://localhost/' });
const g = globalThis as any;
g.window = dom.window;
g.document = dom.window.document;
try { Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true }); } catch { /* already a getter */ }
g.HTMLElement = dom.window.HTMLElement;
g.IS_REACT_ACT_ENVIRONMENT = true;

const GAME_ID = 'ab12cd';
const USER_ID = '60771e41-70e6-4e61-b077-000000000002';

const fb = fakeBackend({ user: { id: USER_ID, username: 'U' } });
const loadGameCalls: string[] = [];

mock.module('next/navigation', { namedExports: { useParams: () => ({ game_id: GAME_ID }), usePathname: () => `/${GAME_ID}` } });
mock.module('../src/contexts/AuthContext.tsx', { namedExports: { useAuth: () => ({ user_id: USER_ID }) } });
mock.module('../src/contexts/ServerContext.tsx', { namedExports: {
    // Seated: these tests are about a seat's join lifecycle, not who may join
    // (e2e/realtime_feed_seating.test.ts covers that against the real provider).
    useServer: () => ({
        loadGame: async (id: string) => { loadGameCalls.push(id); return { game_id: id }; },
        views: { [GAME_ID]: { mySeat: 0 } },
    }),
} });
mock.module('../src/common/backend.ts', { namedExports: fb.module });

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/** Wait (bounded by attempts, not the clock) until `done()` holds. */
async function until(done: () => boolean, what: string): Promise<void> {
    for (let i = 0; i < 60 && !done(); i++) await sleep(50);
    assert.ok(done(), `timed out waiting for: ${what}`);
}

async function mount() {
    const React = (await import('react')).default;
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { RealtimeAnimationFeed } = await import('../src/state/RealtimeAnimationFeed.tsx');
    const root = createRoot(dom.window.document.getElementById('root')!);
    await act(async () => { root.render(React.createElement(RealtimeAnimationFeed)); });
    return { unmount: () => act(async () => { root.unmount(); }) };
}

function reset() {
    fb.holdLog.length = 0;
    loadGameCalls.length = 0;
}

test('feed: admitted on the first subscription, it republishes casts and spends no refetch', async () => {
    reset();
    const { animationFeed } = await import('../src/state/animationFeed.ts');
    const published: any[] = [];
    const off = animationFeed.subscribe((m) => published.push(m));
    const view = await mount();
    await until(() => fb.held.get(`game:${GAME_ID}`) === 1, 'the hold');
    fb.push.joined(GAME_ID, true);
    const cast = new Uint8Array([1, 2, 3]);
    fb.push.cast(GAME_ID, cast);
    fb.push.cast('other1', new Uint8Array([9]));
    assert.deepEqual(published, [{ cast, game_id: GAME_ID }], 'its own game\'s cast republished with its game id, and nothing else');
    await sleep(50);
    assert.deepEqual(loadGameCalls, [], 'nothing was missed, so nothing is refetched');
    off();
    await view.unmount();
    assert.equal(fb.held.get(`game:${GAME_ID}`), undefined, 'let go on unmount');
});

test('feed: refused until the user is a member, it refetches once admitted', async () => {
    reset();
    const view = await mount();
    await until(() => fb.held.get(`game:${GAME_ID}`) === 1, 'the hold');
    fb.push.refused(GAME_ID);
    fb.push.refused(GAME_ID);
    fb.push.joined(GAME_ID, true);
    await until(() => loadGameCalls.length > 0, 'the refetch after admission');
    await sleep(100);
    assert.deepEqual(loadGameCalls, [GAME_ID], 'exactly one refetch of this game after the refused window');
    await view.unmount();
});

test('feed: rejoined after a reconnect, it refetches', async () => {
    reset();
    const view = await mount();
    await until(() => fb.held.get(`game:${GAME_ID}`) === 1, 'the hold');
    fb.push.joined(GAME_ID, true);
    await sleep(50);
    assert.deepEqual(loadGameCalls, [], 'no refetch on the first admission');
    fb.push.joined(GAME_ID, true);
    await until(() => loadGameCalls.length > 0, 'the refetch after the rejoin');
    assert.deepEqual(loadGameCalls, [GAME_ID]);
    await view.unmount();
});
