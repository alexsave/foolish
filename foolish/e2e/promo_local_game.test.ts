/* =============================================================================
 * A promo code is a dealt game against the bot, in its language, with no server
 * =============================================================================
 * The promo playing cards carry a QR code to foolish.cards/br. A visitor who
 * scans one lands on a game already being dealt - them against the bot - played
 * in the tab by the kernel's local table (c/src/local_table.h): no sign-in, no
 * account, and not one request to the backend. /br opens in Russian, /ws in
 * English (c/promo/promo_codes.h).
 *
 * This file mounts the site's real route for a path segment (GameRoute, the
 * page the static shell serves for every one-segment path) in jsdom, over the
 * REAL backend.ts and the real kernel. Only the router is stubbed (Next's hooks
 * have no app here), and the network is replaced by recorders that fail the
 * test on any use: a fetch or a WebSocket from this page would be a request to
 * the backend.
 * ========================================================================== */

import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }

const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>', { url: 'https://foolish.cards/', pretendToBeVisual: true });
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

// ---- the network: anything that would leave the page --------------------------------
const requests: string[] = [];
const realFetch = globalThis.fetch;
g.fetch = async (url: unknown, init?: unknown) => {
    const u = String(url instanceof URL ? url.href : (url as Request)?.url ?? url);
    // Only the network counts: the page's own blob: textures and the kernel's
    // file: asset never leave it.
    if (/^(https?|wss?):/i.test(u)) {
        requests.push(`fetch ${u}`);
        throw new Error(`a request left the page: ${u}`);
    }
    return realFetch(url as any, init as any);
};
g.WebSocket = class { constructor(url: string) { requests.push(`websocket ${url}`); throw new Error(`a socket left the page: ${url}`); } };

// ---- the route ---------------------------------------------------------------------
let segment = '';
mock.module('next/navigation', { namedExports: {
    useParams: () => ({ game_id: segment }),
    useRouter: () => ({ push() {}, replace() {}, back() {} }),
    usePathname: () => `/${segment}`,
} });

interface Page { host: HTMLElement; wait: (ms: number) => Promise<void>; close: () => Promise<void> }

/** The site's page for /`seg`, under the root providers, as the static shell renders it. */
async function open(seg: string): Promise<Page> {
    const React = (await import('react')).default;
    const h = React.createElement;
    const { act } = await import('react');
    const { createRoot } = await import('react-dom/client');
    const { LocalizationProvider } = await import('../src/contexts/LocalizationContext.tsx');
    const { ThemeProvider } = await import('../src/contexts/ThemeContext.tsx');
    const { StyleProvider } = await import('../src/contexts/StyleContext.tsx');
    const { TextureProvider } = await import('../src/components/TexturedSurface.tsx');
    const { AuthProvider } = await import('../src/contexts/AuthContext.tsx');
    // The page itself, kernel gate and all: the gate picks the kernel's link by
    // the address (promo.wasm for a code), so this runs on the module a visitor's
    // tab loads, and nothing else.
    const { default: GamePage } = await import('../src/app/[game_id]/page.tsx');
    segment = seg;
    (dom as unknown as { reconfigure(o: { url: string }): void }).reconfigure({ url: `https://foolish.cards/${seg}` });
    const host = dom.window.document.createElement('div');
    dom.window.document.body.appendChild(host);
    const root = createRoot(host);
    const wait = async (ms: number) => { for (let i = 0; i < 6; i++) await act(async () => { await new Promise((r) => setTimeout(r, ms / 6)); }); };
    await act(async () => {
        root.render(h(LocalizationProvider, null, h(ThemeProvider, null, h(StyleProvider, null, h(TextureProvider, null,
            h(AuthProvider, null, h(GamePage)))))));
    });
    return { host, wait, close: async () => { await act(async () => { root.unmount(); }); host.remove(); } };
}

/** Waits (bounded) until `ok` holds of the page. */
async function until(page: Page, ok: () => boolean, what: string): Promise<void> {
    for (let i = 0; i < 60 && !ok(); i++) await page.wait(60);
    assert.ok(ok(), `${what} (the page drew: ${page.host.innerHTML.slice(0, 300)})`);
}

const handCards = (host: HTMLElement) => host.querySelectorAll('[data-location="hand"][data-card]').length;

/** Holds that `seg` is a dealt local game against the bot, the page in `lang`, and nothing sent. */
async function landsInADealtGame(seg: string, lang: string, theme: string, you: string): Promise<Page> {
    requests.length = 0;
    const { GAME_STATUS } = await import('../src/state/view.ts');
    const page = await open(seg);
    try {
    await until(page, () => page.host.querySelector(`[data-promo-game="${GAME_STATUS.PLAYING}"]`) !== null,
        `/${seg}: a game is being played, with no tap`);
    await until(page, () => handCards(page.host) === 6, `/${seg}: the deal lands: six cards in the visitor's hand`);
    assert.match(page.host.textContent ?? '', /Miami 1/, `/${seg}: the opponent is the bot`);
    assert.ok((page.host.textContent ?? '').includes(you), `/${seg}: the visitor's seat is named in ${lang} (${you})`);
    await until(page, () => dom.window.document.documentElement.getAttribute('data-theme') === theme,
        `/${seg}: the page is in ${lang} (its theme, ${theme})`);
    await page.wait(400);   // the bot's turn, if it leads
    assert.deepEqual(requests, [], `/${seg}: nothing left the page`);
    } catch (e) { await page.close(); throw e; }
    return page;
}

// One game per code per process: a page's version gate is the tab's, and a tab
// meets a second game at one address only through a reload (the end card's
// Rematch reloads), so each code is opened once here.
test('/BR lands in a dealt game against the bot, in Russian, with no request sent', async () => {
    dom.window.localStorage.clear();
    const page = await landsInADealtGame('BR', 'ru', 'soviet', 'Вы');
    try {
        assert.equal(dom.window.localStorage.getItem('foolish_language'), 'ru', 'a visitor with no language of their own keeps Russian');
    } finally { await page.close(); }
});

test('/ws lands in a dealt game in English, and a visitor\'s own language choice is not overwritten', async () => {
    dom.window.localStorage.clear();
    dom.window.localStorage.setItem('foolish_language', 'ru');
    const page = await landsInADealtGame('ws', 'en', 'default', 'You');
    try {
        assert.equal(dom.window.localStorage.getItem('foolish_language'), 'ru', 'their own choice is kept for their next visit');
    } finally { await page.close(); }
});

test('the page ran on promo.wasm alone', async () => {
    const { chooseKernelLink } = await import('../sdk/ts/wasm/wasm_asset.ts');
    assert.equal(chooseKernelLink('promo'), true, 'the promo link is the one this tab loaded');
    assert.equal(chooseKernelLink('web'), false, 'and web.wasm never was');
});
