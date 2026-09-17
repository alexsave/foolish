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
import { fixture, fixtureTable, PLAYING, GAME_OVER, IN, OUT, READY, IDLE, type FixtureSeat } from './helpers/table_fixture.ts';
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
dom.window.matchMedia ??= ((q: string) => ({ matches: false, media: q, addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {} })) as any;
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
const channel: any = { on: () => channel, subscribe: () => channel, unsubscribe: () => Promise.resolve() };
const supabaseMock = {
    from: query,
    channel: () => channel,
    getChannels: () => [],
    removeChannel: () => Promise.resolve(),
    realtime: { setAuth: () => Promise.resolve() },
    functions: { invoke: () => Promise.resolve({ data: null, error: null }) },
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
    await act(async () => { root.render(tree); });
    await wait(settle);
    if (interact) await interact(host, wait);
    const html = host.innerHTML;
    await act(async () => { root.unmount(); });
    host.remove();
    void React;
    return html;
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
    { id: 'b-cordite-01', nickname: '%Cordite', strategy_key: 'cordite' },
    { id: 'b-robusta-02', nickname: '%Robusta', strategy_key: 'robusta' },
    { id: 'b-powder-003', nickname: '%Blackpowder', strategy_key: 'blackpowder' },
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
        .seats([seat(ME, 'Me'), seat('b-cordite-01', 'Cordite', 'cordite'), seat('u-dmitry-01', 'Дмитрий')]).build();
    await screen('lobby_3', 12, () => {
        rows.playerViews.set(gid, envelope(gid, board, 0));
        rows.bots = bots;
    }, () => gamePage(gid, ME));
});

test('lobby, 8 seats: full, with bots', async () => {
    const gid = 'lob8';
    const seats = [seat(ME, 'Me'), seat('u-anna-0001', 'Anna'), seat('b-cordite-01', 'Cordite', 'cordite'),
        seat('u-boris-001', 'Boris'), seat('b-robusta-02', 'Robusta', 'robusta'), seat('u-vera-0001', 'Vera'),
        seat('b-powder-003', 'Blackpowder', 'blackpowder'), seat('u-gleb-0001', 'Gleb')];
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
        .seats([seat(ME, 'Me'), seat('u-anna-0001', 'Anna'), seat('b-cordite-01', 'Cordite', 'cordite'), seat('u-boris-001', 'Boris')])
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
    const seats = [seat(ME, 'Me'), seat('u-anna-0001', 'Anna'), seat('b-cordite-01', 'Cordite', 'cordite'),
        seat('u-boris-001', 'Boris'), seat('b-robusta-02', 'Robusta', 'robusta'), seat('u-vera-0001', 'Vera'),
        seat('b-powder-003', 'Blackpowder', 'blackpowder'), seat('u-gleb-0001', 'Gleb')];
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
    const board = fixture().title('Finished').seats([seat(ME, 'Me'), seat('b-cordite-01', 'Cordite', 'cordite'), seat('u-anna-0001', 'Anna')])
        .status(GAME_OVER).powerSuit(1).hand(1, '6s 7s').seatStatus(0, OUT).seatStatus(1, IN).seatStatus(2, OUT)
        .eliminated(2, 0).discard(34).build();
    await screen('win_screen', 61, () => {
        rows.playerViews.set(gid, envelope(gid, board, 0));
        rows.elo = [{ user_id: ME, elo_rating: 1216, previous_elo: 1200 }, { user_id: 'u-anna-0001', elo_rating: 1190, previous_elo: 1180 }];
        rows.botElo = [{ id: 'b-cordite-01', elo_rating: 1400, previous_elo: 1426 }];
    }, () => gamePage(gid, ME));
});

test('the dashboard', async () => {
    const lobby = fixture().title('Waiting room').seats([seat(ME, 'Me'), seat('b-cordite-01', 'Cordite', 'cordite')]).build();
    await screen('dashboard', 71, () => {
        rows.dashboard = [envelope('dash1', twoSeatBout(), 0), envelope('dash2', lobby, 0), envelope('dash3', eightSeats(), 0)];
    }, () => gamePage('', ME));
});

// The match history reads each finished game's code: its seats, its fool, the
// order the others went out, and the moves its extras time (the names). Recorded
// on the code before Phase 7, which read them through the TS replay decoder.
test('the match history', async () => {
    const { playSeededV6 } = await import('./helpers/seeded_game.ts');
    const { replaySummary } = await import('../sdk/ts/wasm/bots.ts');
    const { encodeExtrasBytes } = await import('../server/api/common/replay/extras.ts');
    const games: { np: number; seed: number; me: number; names: string[] | null }[] = [
        { np: 3, seed: 41, me: 1, names: ['Ada', 'Me', '%Cordite'] },
        { np: 4, seed: 42, me: 0, names: null },
        { np: 2, seed: 7, me: 1, names: ['Boris', 'Me'] },
        { np: 8, seed: 43, me: 5, names: ['A', 'B', 'C', 'D', 'E', 'Me', 'G', 'H'] },
    ];
    const snapshots: Rows['snapshots'] = [];
    for (const [i, x] of games.entries()) {
        const played = await playSeededV6(x.np, x.seed);
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
