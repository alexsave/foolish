/* =============================================================================
 * The web's screens, rendered for real, held to the DOM they rendered before
 * =============================================================================
 * docs/C_GAME_SHAPE_MIGRATION.md Phase 6a moves every component off the TS game
 * shape and onto the kernel's TableView snapshots. That change must not move a
 * pixel, so this file renders the deployed page tree - the real ServerProvider
 * reading envelopes from player_views / spectator_views, the real
 * AnimationProvider, GameProvider, DragProvider, Lobby, GameDisplay, WinScreen,
 * the Dashboard, and the Tutorial - in jsdom, and compares each screen's DOM,
 * attribute and class order included, with the golden recorded from the code
 * before the phase (e2e/fixtures/ui_dom/*.html).
 *
 * The envelopes are the server's own bytes: a board composed in the kernel
 * (e2e/helpers/table_fixture.ts), loaded into the C Table, played on where the
 * screen needs a real move (a pickup), and written by table_envelope for the
 * viewer. Only the network (supabase) and the router are stubbed.
 *
 * What is normalized, and why: nothing in the markup. Math.random is seeded per
 * screen (card backs pick a random tilt), and timers of 300 ms or more never
 * fire (the replay's and the tutorial's deal, the bot bump, chat bubbles), so
 * the same screen renders the same bytes run after run on any machine.
 *
 * UPDATE_UI_GOLDENS=1 rewrites the goldens; a diff there is a UI change to look
 * at in a browser, not a number to accept.
 * ========================================================================== */

import { test, mock, after } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { JSDOM } from 'jsdom';
import { fixture, fixtureTable, parseCardText, PLAYING, GAME_OVER, IN, OUT, READY, IDLE, type FixtureSeat } from './helpers/table_fixture.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }
// A replay's clock and the history's dates are drawn in local time: pin the zone so a
// golden reads the same on every machine.
process.env.TZ = 'UTC';

const GOLDEN_DIR = new URL('./fixtures/ui_dom/', import.meta.url);
const UPDATE = process.env.UPDATE_UI_GOLDENS === '1';

// ---- jsdom, as the React client renders into it ---------------------------------
const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>', { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
for (const k of ['window', 'document', 'HTMLElement', 'HTMLCanvasElement', 'Node', 'Element', 'MouseEvent', 'KeyboardEvent',
    'getComputedStyle', 'requestAnimationFrame', 'cancelAnimationFrame', 'localStorage', 'sessionStorage', 'Image']) {
    try { g[k] = (dom.window as any)[k]; } catch { /* a getter already there */ }
}
try { Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true }); } catch { /* ok */ }
g.IS_REACT_ACT_ENVIRONMENT = true;
g.self ??= dom.window;   // next/link reads it (the invalid-replay page links home)
g.ResizeObserver ??= class { observe() {} unobserve() {} disconnect() {} };
// REDUCE MOTION IS ON for these shots, and that is the whole point of them: a
// DOM snapshot is a screen AT REST, and a gesture caught halfway through is not
// a state anybody chose - it is whatever millisecond the machine happened to
// reach. The role marks turn like a coin now (src/components/RoleCoin.tsx), and
// with motion allowed `replay_bout3_revealed` recorded a shield at
// scaleX(0.45239896) and then failed on the next run at scaleX(0.44829558).
// Under Reduce Motion every gesture is an instant swap, exactly as it is on
// iMessage, so each screen here is the mark the board settles on. The MOTION is
// held frame by frame in e2e/ui_animation_trace.test.ts, which runs on a virtual
// clock and can say when each frame was drawn.
dom.window.matchMedia ??= ((q: string) => ({
    matches: /prefers-reduced-motion/.test(q), media: q,
    addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {},
})) as any;
g.matchMedia = dom.window.matchMedia;
// jsdom has no canvas: texture generators see null contexts and fall back.
(dom.window.HTMLCanvasElement.prototype as any).getContext = () => null;

// ---- the network: supabase, answering from the screen's rows -----------------------
interface Rows {
    playerViews: Map<string, string>;    // game id -> hex view of the signed-in player
    spectatorViews: Map<string, string>; // game id -> hex spectator view
    dashboard: string[];                 // hex views, newest first
    elo: { user_id: string; elo_rating: number; previous_elo: number }[];
    botElo: { id: string; elo_rating: number; previous_elo: number }[];
    bots: { id: string; nickname: string; strategy_key: string }[];
    snapshots: { id: string; player_ids: string[]; moves: string; extras: string | null; created_at: string }[];
}
let rows: Rows = emptyRows();
function emptyRows(): Rows {
    return { playerViews: new Map(), spectatorViews: new Map(), dashboard: [], elo: [], botElo: [], bots: [], snapshots: [] };
}

/** A PostgREST chain: every filter returns the chain; awaiting it (or a terminal) answers from `rows`. */
function query(table: string) {
    const filters: Record<string, unknown> = {};
    const answer = (single: boolean) => {
        let data: unknown = null;
        const gid = filters.game_id as string | undefined;
        if (table === 'player_views') data = single ? (gid && rows.playerViews.has(gid) ? { view: rows.playerViews.get(gid) } : null)
            : rows.dashboard.map((view) => ({ view }));
        else if (table === 'spectator_views') data = gid && rows.spectatorViews.has(gid) ? { view: rows.spectatorViews.get(gid) } : null;
        else if (table === 'chat_messages') data = [];
        else if (table === 'user_elo_ratings') data = rows.elo;
        else if (table === 'bots') data = filters.in ? rows.botElo : rows.bots;
        else if (table === 'game_snapshots') data = rows.snapshots;
        return { data, error: null };
    };
    const chain: any = {
        select: () => chain, order: () => chain, limit: () => chain, insert: () => chain,
        eq: (k: string, v: unknown) => { filters[k] = v; return chain; },
        in: (k: string, v: unknown) => { filters.in = [k, v]; return chain; },
        maybeSingle: () => Promise.resolve(answer(true)),
        single: () => Promise.resolve(answer(true)),
        then: (ok: (r: unknown) => unknown, bad?: (e: unknown) => unknown) => Promise.resolve(answer(false)).then(ok, bad),
    };
    return chain;
}
const invoked: string[] = [];
const channel: any = { on: () => channel, subscribe: () => channel, unsubscribe: () => Promise.resolve() };
const supabaseMock = {
    from: query,
    channel: () => channel,
    getChannels: () => [],
    removeChannel: () => Promise.resolve(),
    realtime: { setAuth: () => Promise.resolve() },
    // Every edge function the page calls, by name, so a test can see a move sent.
    functions: { invoke: (name: string) => { invoked.push(name); return Promise.resolve({ data: null, error: null }); } },
    auth: {
        getSession: () => Promise.resolve({ data: { session: null } }),
        onAuthStateChange: () => ({ data: { subscription: { unsubscribe() {} } } }),
    },
};

let routeGameId = '';
mock.module('../src/backend/Connector.ts', { defaultExport: supabaseMock });
mock.module('next/navigation', { namedExports: {
    useParams: () => (routeGameId ? { game_id: routeGameId } : {}),
    useRouter: () => ({ push() {}, replace() {}, back() {} }),
    usePathname: () => (routeGameId ? `/${routeGameId}` : '/'),
} });

// ---- deterministic randomness per screen ----------------------------------------------
const realRandom = Math.random;
function seedRandom(seed: number): void {
    let s = seed >>> 0;
    Math.random = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
}
after(() => { Math.random = realRandom; });

// ---- the server's bytes ---------------------------------------------------------------
const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');

function envelope(gameId: string, board: { state: Uint8Array; roster: Uint8Array }, viewer: number, version = 7,
    after?: (t: ReturnType<typeof fixtureTable>) => void): string {
    const t = fixtureTable();
    assert.equal(t.load(board.state, board.roster), L.TABLE_OK, 'the fixture loads');
    after?.(t);
    const env = t.envelope(gameId, viewer, version);
    assert.ok(env instanceof Uint8Array, `envelope for viewer ${viewer}: ${env}`);
    return hex(env);
}

// A bot seat is named as the server names it: `bots.nickname`, which carries the reserved
// '%' prefix (src/common/botName.ts). The page shows the name without it.
const seat = (id: string, name: string, brain?: string): FixtureSeat => (brain ? { id, name: `%${name}`, brain } : { id, name });
const ME = 'u-me-0000';

// ---- rendering ------------------------------------------------------------------------
type Interact = (host: HTMLElement, wait: (ms: number) => Promise<void>) => Promise<void>;

async function render(el: () => Promise<any>, interact?: Interact, settle = 120): Promise<string> {
    const React = (await import('react')).default;
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const host = dom.window.document.createElement('div');
    dom.window.document.body.appendChild(host);
    const root = createRoot(host);
    const tree = await el();
    const wait = async (ms: number) => { for (let i = 0; i < 6; i++) await act(async () => { await new Promise((r) => setTimeout(r, ms / 6)); }); };
    // A red assertion in `interact` still unmounts the page: a board left
    // mounted keeps its intervals alive, and the run never ends.
    try {
        await act(async () => { root.render(tree); });
        await wait(settle);
        if (interact) await interact(host, wait);
        return host.innerHTML;
    } finally {
        await act(async () => { root.unmount(); });
        host.remove();
        void React;
    }
}

async function gamePage(gameId: string, userId: string): Promise<any> {
    const React = (await import('react')).default;
    const h = React.createElement;
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
        user_id: userId, username: 'Me', loading: false,
        signIn: async () => ({}), signUp: async () => ({}), signOut: async () => {}, updatePassword: async () => {},
        redirectAfterLogin: null, setRedirectAfterLogin: () => {}, clearRedirectAfterLogin: () => {},
    };
    return h(LocalizationProvider, null, h(ThemeProvider, null, h(StyleProvider, null, h(TextureProvider, null,
        h(AuthContext.Provider, { value: auth as any }, h(ProtectedRoute, null, gameId ? h(GameView) : h(Dashboard)))))));
}

async function replayPage(code: string): Promise<any> {
    const React = (await import('react')).default;
    const h = React.createElement;
    const { LocalizationProvider } = await import('../src/contexts/LocalizationContext.tsx');
    const { ThemeProvider } = await import('../src/contexts/ThemeContext.tsx');
    const { StyleProvider } = await import('../src/contexts/StyleContext.tsx');
    const { TextureProvider } = await import('../src/components/TexturedSurface.tsx');
    const { AuthContext } = await import('../src/contexts/AuthContext.tsx');
    const { ReplayScreen } = await import('../src/components/ReplayScreen.tsx');
    const { ensureBotsAsync } = await import('../sdk/ts/wasm/bots.ts');
    await ensureBotsAsync();
    routeGameId = code;
    const auth = { user_id: ME, username: 'Me', loading: false, setRedirectAfterLogin: () => {} };
    return h(LocalizationProvider, null, h(ThemeProvider, null, h(StyleProvider, null, h(TextureProvider, null,
        h(AuthContext.Provider, { value: auth as any }, h(ReplayScreen, { code }))))));
}

/** Clicks the element `selector` finds under `host`. */
async function click(host: HTMLElement, selector: string, wait: (ms: number) => Promise<void>): Promise<void> {
    const el = host.querySelector(selector);
    assert.ok(el, `nothing matches ${selector}`);
    el.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
    await wait(30);
}

async function historyPage(): Promise<any> {
    const React = (await import('react')).default;
    const h = React.createElement;
    const { LocalizationProvider } = await import('../src/contexts/LocalizationContext.tsx');
    const { ThemeProvider } = await import('../src/contexts/ThemeContext.tsx');
    const { StyleProvider } = await import('../src/contexts/StyleContext.tsx');
    const { TextureProvider } = await import('../src/components/TexturedSurface.tsx');
    const { AuthContext } = await import('../src/contexts/AuthContext.tsx');
    const { MatchHistory } = await import('../src/components/MatchHistory.tsx');
    const { ensureBotsAsync } = await import('../sdk/ts/wasm/bots.ts');
    await ensureBotsAsync();
    routeGameId = '';
    const auth = { user_id: ME, username: 'Me', loading: false, setRedirectAfterLogin: () => {} };
    return h(LocalizationProvider, null, h(ThemeProvider, null, h(StyleProvider, null, h(TextureProvider, null,
        h(AuthContext.Provider, { value: auth as any }, h(MatchHistory))))));
}

async function tutorialPage(start: boolean): Promise<any> {
    const React = (await import('react')).default;
    const h = React.createElement;
    const { LocalizationProvider } = await import('../src/contexts/LocalizationContext.tsx');
    const { ThemeProvider } = await import('../src/contexts/ThemeContext.tsx');
    const { StyleProvider } = await import('../src/contexts/StyleContext.tsx');
    const { TextureProvider } = await import('../src/components/TexturedSurface.tsx');
    const { Tutorial } = await import('../src/components/Tutorial.tsx');
    const { ensureBotsAsync } = await import('../sdk/ts/wasm/bots.ts');
    await ensureBotsAsync();
    routeGameId = '';
    void start;
    return h(LocalizationProvider, null, h(ThemeProvider, null, h(StyleProvider, null, h(TextureProvider, null, h(Tutorial)))));
}

function holdToGolden(name: string, html: string): void {
    const file = new URL(`${name}.html`, GOLDEN_DIR);
    assert.ok(html.length > 200, `${name}: rendered something (${html.length} bytes): ${html.slice(0, 200)}`);
    if (UPDATE || !existsSync(file)) {
        if (!UPDATE) assert.fail(`${name}: no golden at ${file.pathname}; record it with UPDATE_UI_GOLDENS=1`);
        mkdirSync(GOLDEN_DIR, { recursive: true });
        writeFileSync(file, html + '\n');
        return;
    }
    const want = readFileSync(file, 'utf8').replace(/\n$/, '');
    if (html !== want) {
        let i = 0;
        while (i < html.length && html[i] === want[i]) i++;
        assert.fail(`${name}: the DOM differs from the golden at byte ${i}:\n  got:  ${html.slice(Math.max(0, i - 120), i + 160)}\n  want: ${want.slice(Math.max(0, i - 120), i + 160)}`);
    }
}

async function screen(name: string, seed: number, setup: () => void, page: () => Promise<any>, interact?: Interact): Promise<void> {
    rows = emptyRows();
    setup();
    seedRandom(seed);
    // The screen as it stands before any long timer: the replay's and the
    // tutorial's deal (400 / 450 ms), the bot bump (5 s), chat bubbles (8 s).
    // Dropping them keeps a slow machine from capturing a later state.
    const realSetTimeout = globalThis.setTimeout;
    g.setTimeout = (fn: (...a: unknown[]) => void, ms?: number, ...a: unknown[]) =>
        (ms ?? 0) >= 300 ? 0 : realSetTimeout(fn, ms, ...a);
    let html: string;
    try { html = await render(page, interact); } finally { g.setTimeout = realSetTimeout; }
    holdToGolden(name, html);
}

// ---- the screens ----------------------------------------------------------------------

const bots = [
    { id: 'b-cordite-01', nickname: '%St. Petersburg 1', strategy_key: 'cordite' },
    { id: 'b-robusta-02', nickname: '%Seoul 2', strategy_key: 'robusta' },
    { id: 'b-powder-003', nickname: '%Vienna 3', strategy_key: 'blackpowder' },
];

test('lobby, 1 seat: the creator alone', async () => {
    const gid = 'lob1';
    const board = fixture().title('Me\'s Game').seats([seat(ME, 'Me')]).build();
    await screen('lobby_1', 11, () => {
        rows.playerViews.set(gid, envelope(gid, board, 0));
        rows.bots = bots;
    }, () => gamePage(gid, ME));
});

test('lobby, 3 seats: a human, a ready bot, another human', async () => {
    const gid = 'lob3';
    const board = fixture().title('Дмитрий\'s Game')
        .seats([seat(ME, 'Me'), seat('b-cordite-01', 'St. Petersburg 1', 'cordite'), seat('u-dmitry-01', 'Дмитрий')]).build();
    await screen('lobby_3', 12, () => {
        rows.playerViews.set(gid, envelope(gid, board, 0));
        rows.bots = bots;
    }, () => gamePage(gid, ME));
});

test('lobby, 8 seats: full, with bots', async () => {
    const gid = 'lob8';
    const seats = [seat(ME, 'Me'), seat('u-anna-0001', 'Anna'), seat('b-cordite-01', 'St. Petersburg 1', 'cordite'),
        seat('u-boris-001', 'Boris'), seat('b-robusta-02', 'Seoul 2', 'robusta'), seat('u-vera-0001', 'Vera'),
        seat('b-powder-003', 'Vienna 3', 'blackpowder'), seat('u-gleb-0001', 'Gleb')];
    const board = fixture().title('Full table').seats(seats).seatStatus(1, READY).build();
    await screen('lobby_8', 13, () => {
        rows.playerViews.set(gid, envelope(gid, board, 0));
        rows.bots = bots;
    }, () => gamePage(gid, ME));
});

// A 2-seat bout: the attacker led 6h (covered by 8h) and threw in 6d.
function twoSeatBout() {
    return fixture().title('Two of us').seats([seat(ME, 'Me'), seat('u-anna-0001', 'Anna')]).status(PLAYING)
        .trump('Kc').deck('7s 8s 9s Ts Js Qs Ks As 7c 8c 9c Tc Jc Qc Ac 7d 8d 9d Td Jd')
        .hand(0, '6s 7h Qd Ad').hand(1, '9h Th Jh 6c')
        .table('6h/8h', '6d').attacker(0).defender(1).build();
}

test('2 seats, mid-bout, as the attacker', async () => {
    const gid = 'two1';
    const board = twoSeatBout();
    await screen('bout_2p_attacker', 21, () => rows.playerViews.set(gid, envelope(gid, board, 0)), () => gamePage(gid, ME));
});

test('2 seats, mid-bout, as the defender', async () => {
    const gid = 'two2';
    const board = twoSeatBout();
    await screen('bout_2p_defender', 22, () => rows.playerViews.set(gid, envelope(gid, board, 1)), () => gamePage(gid, 'u-anna-0001'));
});

test('4 seats, right after a pickup', async () => {
    const gid = 'four';
    const board = fixture().title('After the pickup')
        .seats([seat(ME, 'Me'), seat('u-anna-0001', 'Anna'), seat('b-cordite-01', 'St. Petersburg 1', 'cordite'), seat('u-boris-001', 'Boris')])
        .status(PLAYING).trump('Qh').deck('7s 8s 9s Ts Js Qs Ks As')
        .hand(0, '7c 8c 9c Tc Jc').hand(1, '6h 7h 8h 9h Th 6d').hand(2, 'Qc Kc Ac 7d 8d 9d').hand(3, 'Td Jd Qd Kd Ad Jh')
        .table('6c/Kh', '6s').attacker(0).defender(1).discard(4).build();
    await screen('after_pickup_4p', 31, () => {
        rows.playerViews.set(gid, envelope(gid, board, 0, 9, (t) => {
            const rc = t.act('u-anna-0001', encodeAction({ kind: 'pickup' }), null, 0);
            assert.ok(rc >= 0, `the pickup applies (${rc})`);
        }));
    }, () => gamePage(gid, ME));
});

function eightSeats() {
    const seats = [seat(ME, 'Me'), seat('u-anna-0001', 'Anna'), seat('b-cordite-01', 'St. Petersburg 1', 'cordite'),
        seat('u-boris-001', 'Boris'), seat('b-robusta-02', 'Seoul 2', 'robusta'), seat('u-vera-0001', 'Vera'),
        seat('b-powder-003', 'Vienna 3', 'blackpowder'), seat('u-gleb-0001', 'Gleb')];
    return fixture().title('Eight').seats(seats).status(PLAYING).trump('2d').deck('3s 4s 5s 6s')
        .hand(0, 'As Ks Qs Js Ts 9s').hand(1, 'Ah Kh Qh Jh Th 9h').hand(2, 'Ac Kc Qc Jc Tc 9c').hand(3, 'Ad Kd Qd Jd Td 9d')
        .hand(4, '8s 8h 8c 8d 7h 7c').hand(5, '6h 6c 6d 5h 5c 7s').hand(6, '4h 4c 4d 3h 3c 3d').hand(7, '2s 2c 7d')
        .table('2h/5d').attacker(6).defender(7).discard(0).build();
}

test('8 seats, playing', async () => {
    const gid = 'eight';
    const board = eightSeats();
    await screen('playing_8p', 41, () => rows.playerViews.set(gid, envelope(gid, board, 0)), () => gamePage(gid, ME));
});

test('a spectator of 8 seats', async () => {
    const gid = 'watch';
    const board = eightSeats();
    await screen('spectator_8p', 42, () => rows.spectatorViews.set(gid, envelope(gid, board, -1)), () => gamePage(gid, 'u-stranger'));
});

function saidGood() {
    return fixture().title('Good').seats([seat(ME, 'Me'), seat('u-anna-0001', 'Anna'), seat('u-boris-001', 'Boris')])
        .status(PLAYING).trump('Kc').deck('7s 8s 9s Ts Js Qs')
        .hand(0, '6s Ks Qd').hand(1, 'Ah Td').hand(2, 'Jd 9d Ad')
        .table('7h/9h', '6d/8d').attacker(0).defender(1).good(2).goodTimestamp().build();
}

test('a said good, as the attacker who has not said it', async () => {
    const gid = 'good0';
    const board = saidGood();
    await screen('good_not_said', 51, () => rows.playerViews.set(gid, envelope(gid, board, 0)), () => gamePage(gid, ME));
});

test('a said good, as the attacker who said it', async () => {
    const gid = 'good2';
    const board = saidGood();
    await screen('good_said', 52, () => rows.playerViews.set(gid, envelope(gid, board, 2)), () => gamePage(gid, 'u-boris-001'));
});

test('the win screen', async () => {
    const gid = 'done';
    const board = fixture().title('Finished').seats([seat(ME, 'Me'), seat('b-cordite-01', 'St. Petersburg 1', 'cordite'), seat('u-anna-0001', 'Anna')])
        .status(GAME_OVER).powerSuit(1).hand(1, '6s 7s').seatStatus(0, OUT).seatStatus(1, IN).seatStatus(2, OUT)
        .eliminated(2, 0).discard(34).build();
    await screen('win_screen', 61, () => {
        rows.playerViews.set(gid, envelope(gid, board, 0));
        rows.elo = [{ user_id: ME, elo_rating: 1216, previous_elo: 1200 }, { user_id: 'u-anna-0001', elo_rating: 1190, previous_elo: 1180 }];
        rows.botElo = [{ id: 'b-cordite-01', elo_rating: 1400, previous_elo: 1426 }];
    }, () => gamePage(gid, ME));
});

test('the dashboard', async () => {
    const lobby = fixture().title('Waiting room').seats([seat(ME, 'Me'), seat('b-cordite-01', 'St. Petersburg 1', 'cordite')]).build();
    await screen('dashboard', 71, () => {
        rows.dashboard = [envelope('dash1', twoSeatBout(), 0), envelope('dash2', lobby, 0), envelope('dash3', eightSeats(), 0)];
    }, () => gamePage('', ME));
});

// The match history reads each finished game's code: its seats, its fool, the
// order the others went out, and the moves its extras time (the names). Recorded
// on the code before Phase 7, which read them through the TS replay decoder.
test('the match history', async () => {
    const { seededCode } = await import('./helpers/seeded_codes.ts');
    const { replaySummary } = await import('../sdk/ts/wasm/bots.ts');
    const { encodeExtrasBytes } = await import('../server/api/common/replay/extras.ts');
    // BOTH ERAS OF BOT NAME, on purpose, because the history screen is where they
    // coexist. A blob stores the name the bot had when the game was played and the
    // screen renders it as stored - there is no display map - so '%Cordite' is an
    // old game and '%Moscow 4' is one played since the roster became its cities.
    const games: { np: number; seed: number; me: number; names: string[] | null }[] = [
        { np: 3, seed: 41, me: 1, names: ['Ada', 'Me', '%Cordite'] },
        { np: 4, seed: 42, me: 0, names: null },
        { np: 2, seed: 7, me: 1, names: ['Boris', 'Me'] },
        { np: 8, seed: 43, me: 5, names: ['A', 'B', 'C', 'D', 'E', 'Me', '%Moscow 4', 'H'] },
    ];
    const snapshots: Rows['snapshots'] = [];
    for (const [i, x] of games.entries()) {
        const played = { code: seededCode(x.np, x.seed) };
        assert.ok(played, `${x.np}p seed ${x.seed} finished`);
        const summary = replaySummary(played!.code);
        assert.ok(summary, 'the code has a summary');
        const times = Array.from({ length: summary!.moves + 1 }, (_, k) => 1_780_000_000 + k * (7 + i));
        const extras = x.names ? hex(encodeExtrasBytes(x.names, times)) : null;
        const ids = Array.from({ length: x.np }, (_, s) => (s === x.me ? ME : `u-other-${i}-${s}`));
        snapshots.push({ id: `snap-${i}`, player_ids: ids, moves: hex(played!.code), extras, created_at: `2026-09-1${i}T12:00:00` });
    }
    await screen('match_history', 72, () => {
        rows.snapshots = snapshots;
        rows.elo = [{ user_id: ME, elo_rating: 1234, previous_elo: 1200 }];
    }, () => historyPage());
});

test('the tutorial, first screen', async () => {
    await screen('tutorial_intro', 81, () => {}, () => tutorialPage(false));
});

test('the tutorial board before the deal lands', async () => {
    await screen('tutorial_predeal', 82, () => {}, () => tutorialPage(true),
        (host, wait) => click(host, '[data-testid="tut-start"]', wait));
});

test('a replay, the board before the deal', async () => {
    const { TUTORIAL_MOVES_CODE } = await import('../src/components/tutorialGame.ts');
    await screen('replay_predeal', 91, () => {}, () => replayPage(TUTORIAL_MOVES_CODE));
});

test('a replay, three bouts in, hands revealed', async () => {
    const { TUTORIAL_MOVES_CODE } = await import('../src/components/tutorialGame.ts');
    await screen('replay_bout3_revealed', 92, () => {}, () => replayPage(TUTORIAL_MOVES_CODE), async (host, wait) => {
        for (let i = 0; i < 3; i++) await click(host, 'button[title="Next bout"]', wait);
        await click(host, 'button[title="Show all hands"]', wait);
    });
});

void IDLE;

// A shared replay with its extras: the seats named, the clock shown, and the last
// step's closing line naming the fool. A real share link is longer than a table id
// may be (the moves and the extras), and from Phase 6a until Phase 7 such a link
// rendered "invalid replay": the replay's boards carried the link as their game id,
// and the kernel's board writer refuses an id past its capacity. So this golden
// could not be recorded on the code before; it was recorded with the fix and read
// frame by frame (the named seats, the fool's closing line, the last board).
test('a replay with names, at its last step', async () => {
    const { TUTORIAL_MOVES_CODE } = await import('../src/components/tutorialGame.ts');
    const { replaySummary, kernelB32Decode } = await import('../sdk/ts/wasm/bots.ts');
    const { encodeExtras, joinReplayCode } = await import('../server/api/common/replay/extras.ts');
    await (await import('../sdk/ts/wasm/bots.ts')).ensureBotsAsync();
    const summary = replaySummary(kernelB32Decode(TUTORIAL_MOVES_CODE));
    assert.ok(summary, 'the code has a summary');
    const times = Array.from({ length: summary!.moves + 1 }, (_, k) => 1_780_000_000 + k * 9);
    const code = joinReplayCode(TUTORIAL_MOVES_CODE, encodeExtras(['Ada', 'Boris', '%Cy'], times));
    assert.ok(code.length > 64, `a share link longer than a table id (${code.length} characters)`);
    await screen('replay_named_end', 93, () => {}, () => replayPage(code), async (host, wait) => {
        assert.ok(host.querySelector('button[title="Next bout"]'), `the replay renders: ${host.textContent?.slice(0, 120)}`);
        for (let i = 0; i < 40; i++) {
            const next = host.querySelector('button[title="Next bout"]');
            if (!next) break;
            await click(host, 'button[title="Next bout"]', wait);
        }
    });
});

// ---- one move, one button ---------------------------------------------------------------
//
// The action buttons a board draws are the kernel's pills for the selection
// (legal.h play_board_pills, the rule the iMessage board draws by): a selected
// card takes Take and Good away, and the one time two buttons stand together is a
// card that both covers and transfers (a trump of the attack's rank). Each board
// below is rendered for real and its cards are TAPPED, as a player selects them
// (DragContext: a press and a release under 150 ms), and the button column is
// read back child by child, top to bottom. The column holds the kernel's pills
// and nothing else - no spacer keeps a slot for a button that is not there - so
// a lone pill is the column's only child, and Pass and Cover stand Cover above
// Pass, as the iMessage column does (FActionBar). Podkidnoy (GAME_RULE_NO_PASS)
// has no row: the website's boards carry no rules variant yet (Lobby.tsx
// TODO(podkidnoy)), and legal.c's own tests hold its pills.

/** The action column, top to bottom: each child's button label, "_" for a child that is no button. */
function actionSlots(host: HTMLElement): string[] {
    const col = host.querySelector('[style*="bottom: 90px"][style*="right: 20px"]');
    assert.ok(col, 'the action column renders');
    return Array.from(col.children, (c) => c.querySelector('.btn-action-text')?.textContent ?? '_');
}

/**
 * Taps the hand card `text` (e.g. "9h"): selects it, or deselects it.
 *
 * The finger is down for TAP_MS by the events' own clocks, and the release is
 * handled TAP_LATE_MS after the press - later than the 150 ms a tap may last.
 * That is the slow machine, made the rule: the press re-renders the whole board,
 * and under V8 coverage on a CI runner that render alone ran past 150 ms, so a
 * page that timed the tap by Date.now() in its handlers saw a long press and
 * selected nothing (DragContext endCardDrag). The tap must be judged on the
 * events' timestamps, and this tap holds the page to that on every machine.
 */
const TAP_MS = 40;
const TAP_LATE_MS = 200;
async function tapCard(host: HTMLElement, text: string, wait: (ms: number) => Promise<void>): Promise<void> {
    const [c] = parseCardText(text);
    const el = host.querySelector(`[data-location="hand"][data-card="${c.suit}-${c.value}"]`);
    assert.ok(el, `${text} is in the hand`);
    // A browser stamps events in ms since the page loaded (performance.now's
    // clock), not since 1970 as jsdom does, so a page that mixes either stamp with
    // Date.now() reads every tap as a press of decades.
    const down = new dom.window.MouseEvent('mousedown', { bubbles: true, clientX: 5, clientY: 5 });
    Object.defineProperty(down, 'timeStamp', { value: 1000 });
    el.dispatchEvent(down);
    await wait(TAP_LATE_MS);
    const up = new dom.window.MouseEvent('mouseup', { bubbles: true, clientX: 5, clientY: 5 });
    Object.defineProperty(up, 'timeStamp', { value: 1000 + TAP_MS });
    dom.window.document.dispatchEvent(up);
    await wait(12);
}

/** Presses the key `key` on the page, as the keyboard shortcuts hear it. */
async function press(key: string, wait: (ms: number) => Promise<void>): Promise<void> {
    dom.window.document.dispatchEvent(new dom.window.KeyboardEvent('keydown', { key, bubbles: true }));
    await wait(12);
}

/** Renders `board` for seat `viewer` (signed in as `userId`) and runs `steps` on it. */
async function onBoard(gid: string, board: { state: Uint8Array; roster: Uint8Array }, viewer: number, userId: string,
    steps: Interact): Promise<void> {
    rows = emptyRows();
    rows.playerViews.set(gid, envelope(gid, board, viewer));
    seedRandom(7);
    invoked.length = 0;
    // The long timers are dropped, as screen() drops them, so none outlives the board.
    const realSetTimeout = globalThis.setTimeout;
    g.setTimeout = (fn: (...a: unknown[]) => void, ms?: number, ...a: unknown[]) =>
        (ms ?? 0) >= 300 ? 0 : realSetTimeout(fn, ms, ...a);
    try { await render(() => gamePage(gid, userId), steps); } finally { g.setTimeout = realSetTimeout; }
}

// Me (seat 0) against Anna (seat 1), clubs trump. `table` is the bout; `attacker` names who leads.
function pillBoard(table: string[], attacker: 0 | 1) {
    return fixture().title('Pills').seats([seat(ME, 'Me'), seat('u-anna-0001', 'Anna')]).status(PLAYING)
        .trump('Kc').deck('7s 8s 9s Ts Js Qs Ks As 7d 8d 9d Td Jd Kd')
        .hand(0, attacker === 0 ? '6s 7h Qd Ad' : '9h Th 6d 6c').hand(1, attacker === 0 ? '9h Th 6d 6c' : '6s 7h Qd Ad')
        .table(...table).attacker(attacker).defender(attacker === 0 ? 1 : 0).build();
}

test('the attacker: Good over a covered table, gone once a throw-in is selected', async () => {
    await onBoard('pilla', pillBoard(['6h/8h'], 0), 0, ME, async (host, wait) => {
        assert.deepEqual(actionSlots(host), ['Good'], 'nothing selected over a covered table: Good');
        await tapCard(host, '6s', wait);
        assert.deepEqual(actionSlots(host), ['Attack'], 'a throw-in selected: Attack, and Good is gone');
        await press('g', wait);
        assert.equal(invoked.join(','), '', 'and G does not say Good over the selected card');
        await press('ArrowDown', wait);
        assert.equal(invoked.join(','), '', 'nor does the down arrow');
        await tapCard(host, 'Qd', wait);
        assert.deepEqual(actionSlots(host), [], 'a selection that is no throw-in: no button at all');
        await tapCard(host, '6s', wait);
        await tapCard(host, 'Qd', wait);
        assert.deepEqual(actionSlots(host), ['Good'], 'the selection put back: Good again');
        await press('ArrowDown', wait);
        assert.ok(invoked.includes('action'), `with nothing selected the down arrow says Good (${invoked.join(',')})`);
    });
});

test('the attacker over an open bout: no Good, and Attack for a throw-in', async () => {
    await onBoard('pillb', pillBoard(['6h'], 0), 0, ME, async (host, wait) => {
        assert.deepEqual(actionSlots(host), [], 'an uncovered attack: nothing to say yet');
        await tapCard(host, '6s', wait);
        assert.deepEqual(actionSlots(host), ['Attack'], 'a throw-in selected: Attack');
    });
});

test('the defender: Take with nothing selected; Cover, Pass, or both for the selected card, and never Take', async () => {
    await onBoard('pillc', pillBoard(['6h'], 1), 0, ME, async (host, wait) => {
        assert.deepEqual(actionSlots(host), ['Pickup'], 'nothing selected: Take alone');
        await tapCard(host, '9h', wait);
        assert.deepEqual(actionSlots(host), ['Cover'], 'a covering card: Cover alone');
        await press('u', wait);
        assert.equal(invoked.join(','), '', 'and U does not take the table over the selected card');
        await press('ArrowDown', wait);
        assert.equal(invoked.join(','), '', 'nor does the down arrow');
        await tapCard(host, '9h', wait);
        await tapCard(host, '6d', wait);
        assert.deepEqual(actionSlots(host), ['Pass'], 'a card of the attack\'s rank: Pass alone');
        await tapCard(host, '6d', wait);
        await tapCard(host, '6c', wait);
        assert.deepEqual(actionSlots(host), ['Cover', 'Pass'], 'a trump of the attack\'s rank: Cover above Pass, as iMessage stacks them');
        await tapCard(host, '6c', wait);
        assert.deepEqual(actionSlots(host), ['Pickup'], 'the selection put back: Take again');
        await press('u', wait);
        assert.ok(invoked.includes('action'), `with nothing selected U takes the table (${invoked.join(',')})`);
    });
});
