// E2E for the page's stream of a game across a join and an exit: the real
// ServerProvider and RealtimeAnimationFeed rendered in jsdom, against the
// backend seam stubbed (e2e/helpers/fake_backend.ts).
//
// A game is one stream, whoever holds it: the feed for a seated player, the page
// for a spectator. Across a join and an exit the page must hold it exactly
// once - never twice (a leak that grows with every exit and re-join), never
// zero (a page that stops moving) - and it must keep carrying the game's casts.
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

// ---- the backend --------------------------------------------------------------------
const GAME = 'feed01';
const fb = fakeBackend({ user: { id: ME, username: 'ME' } });
let metaAnswer: Uint8Array | null = null;
fb.onMeta = async () => { if (!metaAnswer) throw new Error('refused'); return metaAnswer; };
const holds = () => fb.held.get(`game:${GAME}`) ?? 0;

mock.module('next/navigation', { namedExports: {
    useParams: () => ({ game_id: GAME }),
    usePathname: () => `/${GAME}`,
    useRouter: () => ({ push: () => {}, replace: () => {} }),
} });
mock.module('../src/contexts/AuthContext.tsx', { namedExports: { useAuth: () => ({ user_id: ME }) } });
mock.module('../src/common/backend.ts', { namedExports: fb.module });

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

test('a spectator who takes a seat and gives it up holds the game\'s stream once, and it keeps moving', async () => {
    const React = (await import('react')).default;
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { ServerProvider, useServerActions } = await import('../src/contexts/ServerContext.tsx');
    const { RealtimeAnimationFeed } = await import('../src/state/RealtimeAnimationFeed.tsx');
    const { animationFeed } = await import('../src/state/animationFeed.ts');

    const watching = [human(HOST, 'Host'), ...Array.from({ length: MAX_PLAYERS - 2 }, (_, i) => bot(i))];
    const seated = [...watching, human(ME, 'Me')];
    const mine = (v: number) => envelope(GAME, seated, seated.length - 1, v);
    const theirs = (v: number) => envelope(GAME, watching, -1, v);
    const heard: string[] = [];
    animationFeed.subscribe((m: any) => { if (m.cast) heard.push(m.game_id); });
    const carries = (what: string) => {
        heard.length = 0;
        fb.push.cast(GAME, new Uint8Array([1]));
        assert.deepEqual(heard, [GAME], `${what}: a cast of the game reaches the animation feed, once`);
    };

    let actions: any = null;
    const Probe = () => { actions = useServerActions(); return null; };
    fb.views = { [GAME]: mine(3) };
    const root = createRoot(dom.window.document.getElementById('root')!);
    await act(async () => {
        root.render(React.createElement(ServerProvider, null, React.createElement(RealtimeAnimationFeed), React.createElement(Probe)));
    });
    await act(async () => { await sleep(300); });
    assert.equal(holds(), 1, 'seated: the game held once (the feed\'s)');
    carries('seated');

    // I give the seat up: watching, on one stream that carries the game.
    fb.views = { [GAME]: theirs(4) };
    metaAnswer = theirs(4);
    await act(async () => { await actions.exitGame(GAME, undefined, ME); await sleep(300); });
    assert.equal(holds(), 1, 'watching: still once (the page\'s, the feed let go)');
    carries('watching');

    // I take the seat again: the join answers with my seated view.
    fb.views = { [GAME]: mine(5) };
    metaAnswer = mine(5);
    await act(async () => { await actions.joinGame(GAME); await sleep(300); });
    assert.equal(holds(), 1, 'seated again: once, not the spectator\'s beside my own');
    carries('seated again');

    // And once more round.
    metaAnswer = theirs(6);
    await act(async () => { await actions.exitGame(GAME, undefined, ME); await sleep(300); });
    assert.equal(holds(), 1, 'watching again: once');
    metaAnswer = mine(7);
    await act(async () => { await actions.joinGame(GAME); await sleep(300); });
    assert.equal(holds(), 1, 'seated again: once');

    await act(async () => { root.unmount(); });
    assert.equal(holds(), 0, 'and nothing is held once the page is gone');
});
