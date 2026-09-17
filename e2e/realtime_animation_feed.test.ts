// E2E for the live game's animation channel producer
// (src/state/RealtimeAnimationFeed.tsx), rendered for real in jsdom with only the
// supabase client, auth, route and server actions stubbed.
//
// The web joins `gu-{game}-{user}` as a PRIVATE channel, and Realtime admits the
// join only once the user is a member of the game (the player_hands EXISTS in the
// channel policy, see e2e/realtime_channel_auth.test.ts). Opening a game you are
// about to join, or one whose create is still persisting in the background, is
// exactly that window: the first joins are refused and retried. Realtime has no
// catch-up, so every broadcast sent while the join was refused - the joiner's own
// join animation among them, and the join response does not apply state for the
// joiner - is gone. The feed must refetch authoritative state once it is finally
// admitted, the same as after a reconnect. When the very first join is admitted
// nothing was missed, and it must not spend a fetch.
//
// Needs --experimental-test-module-mocks (see the test:e2e script).

import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';

const dom = new JSDOM('<!DOCTYPE html><html><body><div id="root"></div></body></html>', { url: 'http://localhost/' });
const g = globalThis as any;
g.window = dom.window;
g.document = dom.window.document;
try { Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true }); } catch { /* already a getter */ }
g.HTMLElement = dom.window.HTMLElement;
g.IS_REACT_ACT_ENVIRONMENT = true;

const GAME_ID = 'ab12cd';
const USER_ID = '60771e41-70e6-4e61-b077-000000000002';

type Status = 'SUBSCRIBED' | 'CHANNEL_ERROR' | 'TIMED_OUT' | 'CLOSED';

// Each join attempt consumes the next scripted outcome; a SUBSCRIBED channel can
// later be dropped by the test through `drop`.
let script: Status[] = [];
const joins: { topic: string; private: boolean }[] = [];
let live: { handlers: Map<string, (p: any) => void>; statusCb: (s: Status) => void } | null = null;
const loadGameCalls: string[] = [];

const supabaseMock = {
    realtime: { setAuth: async () => {} },
    removeChannel: async () => 'ok',
    channel: (topic: string, opts: any) => {
        const handlers = new Map<string, (p: any) => void>();
        const ch = {
            on: (_type: string, filter: { event: string }, cb: (p: any) => void) => { handlers.set(filter.event, cb); return ch; },
            subscribe: (statusCb: (s: Status) => void) => {
                joins.push({ topic, private: opts?.config?.private === true });
                const outcome = script.shift() ?? 'CHANNEL_ERROR';
                live = { handlers, statusCb };
                queueMicrotask(() => statusCb(outcome));
                return ch;
            },
        };
        return ch;
    },
};

mock.module('next/navigation', { namedExports: { useParams: () => ({ game_id: GAME_ID }) } });
mock.module('../src/contexts/AuthContext.tsx', { namedExports: { useAuth: () => ({ user_id: USER_ID }) } });
mock.module('../src/contexts/ServerContext.tsx', { namedExports: {
    // Seated: these tests are about a seat's join lifecycle, not who may join
    // (e2e/realtime_feed_seating.test.ts covers that against the real provider).
    useServer: () => ({
        loadGame: async (id: string) => { loadGameCalls.push(id); return { game_id: id }; },
        games: { [GAME_ID]: { self: { player_id: USER_ID } } },
    }),
} });
mock.module('../src/backend/Connector.ts', { defaultExport: supabaseMock });

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

function reset(outcomes: Status[]) {
    script = outcomes;
    joins.length = 0;
    loadGameCalls.length = 0;
    live = null;
}

test('feed: admitted on the first join, it republishes broadcasts and spends no refetch', async () => {
    reset(['SUBSCRIBED']);
    const { animationFeed } = await import('../src/state/animationFeed.ts');
    const published: any[] = [];
    const off = animationFeed.subscribe((m) => published.push(m));
    const view = await mount();
    await until(() => joins.length === 1 && live !== null, 'the first join');
    await sleep(100);

    assert.deepEqual(joins, [{ topic: `gu-${GAME_ID}-${USER_ID}`, private: true }], 'joins its own private gu- topic');
    live!.handlers.get('animation_events')!({ payload: { t: 'as2', s: 'seq', v: 7, b: '' } });
    assert.deepEqual(published, [{ t: 'as2', s: 'seq', v: 7, b: '', game_id: GAME_ID }], 'broadcast republished with its game id');
    assert.deepEqual(loadGameCalls, [], 'nothing was missed, so nothing is refetched');

    off();
    await view.unmount();
});

test('feed: refused until the user is a member, it refetches once admitted', async () => {
    reset(['CHANNEL_ERROR', 'CHANNEL_ERROR', 'SUBSCRIBED']);
    const view = await mount();
    await until(() => joins.length === 3, 'two refused joins and a third that is admitted');
    await until(() => loadGameCalls.length > 0, 'the refetch after admission');
    await sleep(100);

    assert.deepEqual(loadGameCalls, [GAME_ID], 'exactly one refetch of this game after the refused window');
    await view.unmount();
});

test('feed: dropped after connecting, it refetches when re-admitted', async () => {
    reset(['SUBSCRIBED', 'SUBSCRIBED']);
    const view = await mount();
    await until(() => joins.length === 1, 'the first join');
    await sleep(50);
    assert.deepEqual(loadGameCalls, [], 'no refetch on the first admission');

    live!.statusCb('CHANNEL_ERROR');
    await until(() => joins.length === 2, 'the rejoin');
    await until(() => loadGameCalls.length > 0, 'the refetch after the rejoin');
    assert.deepEqual(loadGameCalls, [GAME_ID]);
    await view.unmount();
});
