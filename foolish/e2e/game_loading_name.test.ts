/* =============================================================================
 * While a game loads, the page draws its name and nothing else
 * =============================================================================
 * The game page used to fill the gap before its view arrived with a grey,
 * pulsing skeleton of a lobby: a title bar, an id line, a QR box, two seats. It
 * was a guess at a screen, and it looked like one. What the page actually knows
 * in that gap is the game's NAME - the dashboard listed the table, or `create`
 * just answered with it - so that is what it draws, in the lobby's own title
 * slot, and the rest of the screen is the background.
 *
 * This file mounts the deployed tree (the real ServerProvider under the real
 * ProtectedRoute, the real Dashboard, the real GameView) in jsdom and walks the
 * journeys a person takes into a game, with the game page's read of its view
 * HELD so the loading screen can be looked at:
 *   - from the dashboard into a lobby it listed: the name, alone, and it is the
 *     element the lobby then draws its title in;
 *   - straight onto a game's URL, where nothing is known: nothing is drawn;
 *   - from the dashboard into a game that is being played: nothing, because the
 *     board has no title line for a name to hand over to;
 *   - "create new game": nothing until `create` answers, then the name.
 *
 * The dashboard's ServerProvider and the game page's are two mounts, as they are
 * on the site (ProtectedRoute is per route), so the name really does cross a
 * provider that has been torn down. Only the network and the router are stubbed.
 * ========================================================================== */

import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';
import { fixture, fixtureTable, PLAYING, type FixtureSeat } from './helpers/table_fixture.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>', { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
for (const k of ['window', 'document', 'HTMLElement', 'HTMLCanvasElement', 'Node', 'Element', 'MouseEvent', 'KeyboardEvent',
    'getComputedStyle', 'requestAnimationFrame', 'cancelAnimationFrame', 'localStorage', 'sessionStorage', 'Image']) {
    try { g[k] = (dom.window as any)[k]; } catch { /* a getter already there */ }
}
try { Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true }); } catch { /* ok */ }
g.IS_REACT_ACT_ENVIRONMENT = true;
g.self ??= dom.window;
g.ResizeObserver ??= class { observe() {} unobserve() {} disconnect() {} };
dom.window.matchMedia ??= ((q: string) => ({
    matches: /prefers-reduced-motion/.test(q), media: q,
    addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {},
})) as any;
g.matchMedia = dom.window.matchMedia;
(dom.window.HTMLCanvasElement.prototype as any).getContext = () => null;

// ---- the network: supabase, answering from these rows, when the gate lets it ------
/** A door a request waits at until the test opens it. */
class Gate {
    private open_!: () => void;
    readonly passed = new Promise<void>((r) => { this.open_ = r; });
    open(): void { this.open_(); }
}
let dashboard: string[] = [];                 // hex views the dashboard lists
const playerViews = new Map<string, string>(); // game id -> hex view of the signed-in player
let viewGate: Gate | null = null;              // holds the game page's read of its view
let createGate: Gate | null = null;            // holds `create`'s answer
let createAnswer: Uint8Array | null = null;

function query(table: string) {
    const filters: Record<string, unknown> = {};
    const one = async () => {
        const gid = filters.game_id as string | undefined;
        if (table !== 'player_views' && table !== 'spectator_views') return { data: null, error: null };
        await viewGate?.passed;
        return { data: table === 'player_views' && gid && playerViews.has(gid) ? { view: playerViews.get(gid) } : null, error: null };
    };
    const many = async () => ({ data: table === 'player_views' ? dashboard.map((view) => ({ view })) : [], error: null });
    const chain: any = {
        select: () => chain, order: () => chain, limit: () => chain, insert: () => chain,
        eq: (k: string, v: unknown) => { filters[k] = v; return chain; },
        in: () => chain,
        maybeSingle: one,
        single: one,
        then: (ok: (r: unknown) => unknown, bad?: (e: unknown) => unknown) => many().then(ok, bad),
    };
    return chain;
}
const channel: any = { on: () => channel, subscribe: () => channel, unsubscribe: () => Promise.resolve() };
const supabaseMock = {
    from: query,
    channel: () => channel,
    getChannels: () => [],
    removeChannel: () => Promise.resolve(),
    realtime: { setAuth: () => Promise.resolve() },
    functions: { invoke: async (name: string) => {
        if (name !== 'create') return { data: null, error: null };
        await createGate?.passed;
        const b = createAnswer!;
        return { data: b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength), error: null };
    } },
    auth: {
        getSession: () => Promise.resolve({ data: { session: null } }),
        onAuthStateChange: () => ({ data: { subscription: { unsubscribe() {} } } }),
    },
};

let routeGameId = '';
const pushed: string[] = [];
mock.module('../src/common/Connector.ts', { defaultExport: supabaseMock });
mock.module('next/navigation', { namedExports: {
    useParams: () => (routeGameId ? { game_id: routeGameId } : {}),
    useRouter: () => ({ push(to: string) { pushed.push(to); }, replace() {}, back() {} }),
    usePathname: () => (routeGameId ? `/${routeGameId}` : '/dashboard'),
} });

// ---- the server's bytes ---------------------------------------------------------------
const ME = 'u-me-0000';
const SEATS: FixtureSeat[] = [{ id: ME, name: 'Me' }, { id: 'u-anna-0001', name: 'Anna' }];

function envelopeBytes(gameId: string, board: { state: Uint8Array; roster: Uint8Array }): Uint8Array {
    const t = fixtureTable();
    assert.equal(t.load(board.state, board.roster), L.TABLE_OK, 'the fixture loads');
    const env = t.envelope(gameId, 0, 7);
    assert.ok(env instanceof Uint8Array, `envelope: ${env}`);
    return env;
}
const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const lobbyOf = (gameId: string, title: string) => envelopeBytes(gameId, fixture().title(title).seats(SEATS).build());
const boutOf = (gameId: string, title: string) => envelopeBytes(gameId, fixture().title(title).seats(SEATS).status(PLAYING)
    .trump('Qh').deck('7s 8s 9s Ts').hand(0, '6c 7c').hand(1, '6d 7d').build());

// ---- the page -----------------------------------------------------------------------
interface Page { host: HTMLElement; wait: (ms: number) => Promise<void>; close: () => Promise<void> }

/** Mounts one route - the dashboard, or the game `gameId` - under its own ProtectedRoute, as the site does. */
async function open(gameId: string): Promise<Page> {
    const React = (await import('react')).default;
    const h = React.createElement;
    const { act } = await import('react');
    const { createRoot } = await import('react-dom/client');
    const { LocalizationProvider } = await import('../src/contexts/LocalizationContext.tsx');
    const { ThemeProvider } = await import('../src/contexts/ThemeContext.tsx');
    const { StyleProvider } = await import('../src/contexts/StyleContext.tsx');
    const { TextureProvider } = await import('../src/components/TexturedSurface.tsx');
    const { AuthContext } = await import('../src/contexts/AuthContext.tsx');
    const { ProtectedRoute } = await import('../src/components/ProtectedRoute.tsx');
    const { GameView } = await import('../src/components/GameView.tsx');
    const { Dashboard } = await import('../src/components/Dashboard.tsx');
    const { ensureBotsAsync } = await import('../sdk/ts/wasm/bots.ts');
    await ensureBotsAsync();
    routeGameId = gameId;
    const auth = {
        user_id: ME, username: 'Me', loading: false,
        signIn: async () => ({}), signUp: async () => ({}), signOut: async () => {}, updatePassword: async () => {},
        redirectAfterLogin: null, setRedirectAfterLogin: () => {}, clearRedirectAfterLogin: () => {},
    };
    const host = dom.window.document.createElement('div');
    dom.window.document.body.appendChild(host);
    const root = createRoot(host);
    const wait = async (ms: number) => { for (let i = 0; i < 6; i++) await act(async () => { await new Promise((r) => setTimeout(r, ms / 6)); }); };
    await act(async () => {
        root.render(h(LocalizationProvider, null, h(ThemeProvider, null, h(StyleProvider, null, h(TextureProvider, null,
            h(AuthContext.Provider, { value: auth as any }, h(ProtectedRoute, null, gameId ? h(GameView) : h(Dashboard))))))));
    });
    await wait(120);
    return { host, wait, close: async () => { await act(async () => { root.unmount(); }); host.remove(); } };
}

/** The loading screen `host` is showing, or a failure naming what it shows instead. */
function loadingScreen(host: HTMLElement): HTMLElement {
    const el = host.querySelector<HTMLElement>('.lobby[aria-busy="true"]');
    assert.ok(el, `the page is on its loading screen (it drew: ${host.innerHTML.slice(0, 200)})`);
    return el;
}

/** What a loading screen draws over the app background: every element in it that is not a background layer. */
const drawn = (screen: HTMLElement): Element[] =>
    Array.from(screen.querySelectorAll('*')).filter((el) => !/^bg-/.test(el.className) && el.tagName !== 'STYLE');

/** Holds that `screen` draws `title` in the lobby's title slot, and not one thing more. */
function assertNameOnly(screen: HTMLElement, title: string, where: string): void {
    const all = drawn(screen);
    assert.equal(all.length, 1, `${where}: one element is drawn, the name (drew: ${all.map((e) => e.outerHTML).join(' ').slice(0, 300)})`);
    const name = all[0] as HTMLInputElement;
    assert.equal(name.tagName, 'INPUT', `${where}: the name is the element the lobby draws its title in`);
    assert.equal(name.className, 'lobby__name-input', `${where}: in the lobby's title slot, unedited`);
    assert.equal(name.value, title, `${where}: the game's name`);
    assert.equal(name.readOnly, true, `${where}: there is no game to rename yet`);
    assert.equal(screen.textContent, '', `${where}: no other text`);
}

function assertBare(screen: HTMLElement, where: string): void {
    const all = drawn(screen);
    assert.deepEqual(all.map((e) => e.outerHTML), [], `${where}: the background alone`);
    assert.equal(screen.textContent, '', `${where}: no text`);
}

test('from the dashboard into a lobby: the name alone while the view loads, and the lobby takes it over in place', async () => {
    const view = lobbyOf('kitchen1', 'Kitchen table');
    dashboard = [hex(view)];
    playerViews.set('kitchen1', hex(view));
    const dash = await open('');
    assert.match(dash.host.textContent ?? '', /Kitchen table/, 'the dashboard lists the lobby');
    await dash.close();

    viewGate = new Gate();
    const page = await open('kitchen1');
    try {
        assertNameOnly(loadingScreen(page.host), 'Kitchen table', 'loading');
        viewGate.open();
        await page.wait(120);
        assert.equal(page.host.querySelector('[aria-busy="true"]'), null, 'the view landed: the loading screen is gone');
        const title = page.host.querySelector<HTMLInputElement>('.lobby > input.lobby__name-input');
        assert.equal(title?.value, 'Kitchen table', 'the lobby draws the same name in the same slot');
        assert.ok(page.host.querySelector('.lobby__qr-container'), 'and the rest of the lobby is the real one');
    } finally { viewGate.open(); viewGate = null; await page.close(); }
});

test('straight onto a game URL: nothing is known, so nothing is drawn', async () => {
    playerViews.set('direct01', hex(lobbyOf('direct01', 'Never listed')));
    viewGate = new Gate();
    const page = await open('direct01');
    try {
        assertBare(loadingScreen(page.host), 'loading');
        viewGate.open();
        await page.wait(120);
        assert.equal(page.host.querySelector<HTMLInputElement>('.lobby > input.lobby__name-input')?.value, 'Never listed');
    } finally { viewGate.open(); viewGate = null; await page.close(); }
});

test('from the dashboard into a game being played: no name, the board has no title for it to become', async () => {
    const view = boutOf('playing1', 'Mid game');
    dashboard = [hex(view)];
    playerViews.set('playing1', hex(view));
    const dash = await open('');
    assert.match(dash.host.textContent ?? '', /Mid game/, 'the dashboard lists the game');
    await dash.close();

    viewGate = new Gate();
    const page = await open('playing1');
    try {
        assertBare(loadingScreen(page.host), 'loading');
    } finally { viewGate.open(); viewGate = null; await page.close(); }
});

test('create new game: the overlay is bare until `create` answers, then it is the new game\'s name', async () => {
    dashboard = [];
    createAnswer = lobbyOf('fresh001', 'Brand new');
    createGate = new Gate();
    const dash = await open('');
    try {
        const button = Array.from(dash.host.querySelectorAll('button')).find((b) => /create/i.test(b.textContent ?? ''));
        assert.ok(button, 'the dashboard has its create button');
        button.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
        await dash.wait(30);
        assertBare(loadingScreen(dash.host), 'before create answers');
        createGate.open();
        await dash.wait(60);
        assert.deepEqual(pushed, ['/fresh001'], 'the page is sent to the new game');
        assertNameOnly(loadingScreen(dash.host), 'Brand new', 'after create answers');
    } finally { createGate.open(); createGate = null; await dash.close(); }

    // And the game page it lands on, a new provider, still has the name.
    playerViews.set('fresh001', hex(createAnswer));
    viewGate = new Gate();
    const page = await open('fresh001');
    try {
        assertNameOnly(loadingScreen(page.host), 'Brand new', 'the game page');
    } finally { viewGate.open(); viewGate = null; await page.close(); }
});
