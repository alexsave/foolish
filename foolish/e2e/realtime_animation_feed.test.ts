// E2E for the live game's cast stream producer
// (src/state/RealtimeAnimationFeed.tsx), rendered for real in jsdom with only the
// backend seam (e2e/helpers/fake_backend.ts), auth, route and server actions
// stubbed.
//
// The feed holds the seat's stream while the board shows a seat and republishes
// each of its game's casts. What a refused or reopened stream missed is the
// kernel's client's to refetch (c_base/test/unit_netclient.c
// accepted_again_refetches); the board it fetches reaches the page as a view.
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

mock.module('next/navigation', { namedExports: { useParams: () => ({ game_id: GAME_ID }), usePathname: () => `/${GAME_ID}` } });
mock.module('../src/contexts/AuthContext.tsx', { namedExports: { useAuth: () => ({ user_id: USER_ID }) } });
mock.module('../src/contexts/ServerContext.tsx', { namedExports: {
    // Seated: these tests are about a seat's join lifecycle, not who may join
    // (e2e/realtime_feed_seating.test.ts covers that against the real provider).
    useServer: () => ({ views: { [GAME_ID]: { mySeat: 0 } } }),
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
}

test('feed: it republishes its own game\'s casts, and lets the stream go on unmount', async () => {
    reset();
    const { animationFeed } = await import('../src/state/animationFeed.ts');
    const published: any[] = [];
    const off = animationFeed.subscribe((m) => published.push(m));
    const view = await mount();
    await until(() => fb.held.get(`game:${GAME_ID}`) === 1, 'the hold');
    const cast = new Uint8Array([1, 2, 3]);
    fb.push.cast(GAME_ID, cast);
    fb.push.cast('other1', new Uint8Array([9]));
    assert.deepEqual(published, [{ cast, game_id: GAME_ID }], 'its own game\'s cast republished with its game id, and nothing else');
    off();
    await view.unmount();
    assert.equal(fb.held.get(`game:${GAME_ID}`), undefined, 'let go on unmount');
});
