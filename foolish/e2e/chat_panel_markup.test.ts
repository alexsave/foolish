/* =============================================================================
 * The chat panel, rendered in both layouts, held to its markup
 * =============================================================================
 * src/components/GameDisplay/Chat.tsx drew the open panel twice, once per
 * layout, and the two trees differed only in a `--mobile` modifier on some
 * blocks, the header, and a style that lifts the panel over the keyboard. It
 * is one tree now. The stylesheet and the scroll guard select on these class
 * names and data attributes, so this file renders the real component in jsdom
 * at a desktop and at a phone width and holds each to the markup the two
 * separate trees produced.
 *
 * Only the server context is stubbed (the panel reads a view, the messages and
 * a send from it). The time in each line is drawn by the same call the
 * component makes, so the expected text does not depend on the machine's locale.
 * ========================================================================== */

import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.error = () => {}; }
process.env.TZ = 'UTC';

const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>', { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
for (const k of ['window', 'document', 'HTMLElement', 'Node', 'Element', 'MouseEvent', 'localStorage']) {
    try { g[k] = (dom.window as any)[k]; } catch { /* a getter already there */ }
}
try { Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true }); } catch { /* ok */ }
g.IS_REACT_ACT_ENVIRONMENT = true;
dom.window.matchMedia ??= ((q: string) => ({
    matches: false, media: q, addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {},
})) as any;
// jsdom lays nothing out, so it has no scrollTo; the panel scrolls its log on every message.
(dom.window.HTMLElement.prototype as any).scrollTo ??= function scrollTo() {};

const MESSAGES = [
    { sender_name: 'Anna', message: 'good luck', created_at: '2026-01-02T03:04:05Z' },
    { sender_name: '', message: 'a <b>bold</b> move', created_at: '2026-01-02T03:05:00Z' },
];
const at = (i: number) => new Date(MESSAGES[i].created_at).toLocaleTimeString();

let seat = 0;
mock.module('../src/contexts/ServerContext.tsx', { namedExports: {
    useServer: () => ({ view: { mySeat: seat }, sendMessage: async () => {}, chatMessages: MESSAGES }),
} });

type Wait = (ms: number) => Promise<void>;

/** Mounts the panel in a window `width` wide, hands it to `body`, and unmounts it. */
async function withChat(width: number, body: (host: HTMLElement, wait: Wait) => Promise<void>): Promise<void> {
    const React = (await import('react')).default;
    const { act } = await import('react');
    const { createRoot } = await import('react-dom/client');
    const { LocalizationProvider } = await import('../src/contexts/LocalizationContext.tsx');
    const { ThemeProvider } = await import('../src/contexts/ThemeContext.tsx');
    const { Chat } = await import('../src/components/GameDisplay/Chat.tsx');
    (dom.window as any).innerWidth = width;
    (dom.window as any).innerHeight = 768;
    const host = dom.window.document.createElement('div');
    dom.window.document.body.appendChild(host);
    const root = createRoot(host);
    const wait: Wait = async (ms) => { await act(async () => { await new Promise((r) => setTimeout(r, ms)); }); };
    try {
        await act(async () => { root.render(React.createElement(LocalizationProvider, null, React.createElement(ThemeProvider, null, React.createElement(Chat)))); });
        await wait(10);
        await body(host, wait);
    } finally {
        await act(async () => { root.unmount(); });
        host.remove();
    }
}

async function click(host: HTMLElement, selector: string, wait: Wait): Promise<void> {
    const el = host.querySelector(selector);
    assert.ok(el, `nothing matches ${selector}`);
    el.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
    await wait(10);
}

// The two panels as the two separate trees drew them, recorded from that code.
const desktopPanel = () =>
    '<div class="chat">'
    + '<div class="chat__header" data-touch-interactive="true">Chat ▼</div>'
    + '<div class="chat__messages" data-chat-scrollable="true">'
    + `<div class="chat__message"><span class="chat__message-sender">Anna:</span> <span class="chat__message-text">good luck</span> <span class="chat__message-time">[${at(0)}]</span></div>`
    + `<div class="chat__message"><span class="chat__message-sender">Unknown:</span> <span class="chat__message-text">a &lt;b&gt;bold&lt;/b&gt; move</span> <span class="chat__message-time">[${at(1)}]</span></div>`
    + '</div>'
    + '<form class="chat__form" data-touch-interactive="true">'
    + '<input class="chat__input" placeholder="Type message..." inputmode="none" maxlength="1000" type="text" value="">'
    + '<button class="chat__submit" type="submit" disabled="">Send</button>'
    + '</form>'
    + '</div>';

const mobilePanel = (style: string) =>
    `<div class="chat chat--mobile"${style}>`
    + '<div class="chat__header chat__header--mobile" data-touch-interactive="true"><span>Chat</span><button class="chat__close-btn">✕</button></div>'
    + '<div class="chat__messages chat__messages--mobile" data-chat-scrollable="true">'
    + `<div class="chat__message chat__message--mobile"><span class="chat__message-sender">Anna:</span> <span class="chat__message-text">good luck</span> <span class="chat__message-time chat__message-time--mobile">[${at(0)}]</span></div>`
    + `<div class="chat__message chat__message--mobile"><span class="chat__message-sender">Unknown:</span> <span class="chat__message-text">a &lt;b&gt;bold&lt;/b&gt; move</span> <span class="chat__message-time chat__message-time--mobile">[${at(1)}]</span></div>`
    + '</div>'
    + '<form class="chat__form chat__form--mobile" data-touch-interactive="true">'
    + '<input class="chat__input chat__input--mobile" placeholder="Type message..." inputmode="none" maxlength="1000" type="text" value="">'
    + '<button class="chat__submit chat__submit--mobile" type="submit" disabled="">Send</button>'
    + '</form>'
    + '</div>';

test('closed, the panel is the toggle button; a spectator has no chat at all', async () => {
    seat = 0;
    await withChat(1024, async (host) => {
        assert.match(host.innerHTML, /^<div class="chat-toggle" data-chat-button="true"><div class="chat-toggle__icon">.+<\/div><\/div>$/);
    });
    seat = -1;
    try {
        await withChat(1024, async (host) => { assert.equal(host.innerHTML, ''); });
    } finally { seat = 0; }
});

test('open at a desktop width: no modifier on any block, and the whole header closes it', async () => {
    await withChat(1024, async (host, wait) => {
        await click(host, '.chat-toggle', wait);
        assert.equal(host.innerHTML, desktopPanel());
        await click(host, '.chat__header', wait);
        assert.ok(host.querySelector('.chat-toggle') && !host.querySelector('.chat'), 'a click on the header closes the panel');
    });
});

test('open at a phone width: the --mobile modifier on eight blocks, and only the close button closes it', async () => {
    await withChat(390, async (host, wait) => {
        await click(host, '.chat-toggle', wait);
        assert.equal(host.innerHTML, mobilePanel(''));
        await click(host, '.chat__header', wait);
        assert.ok(host.querySelector('.chat'), 'a tap on the phone header leaves the panel open');
        await click(host, '.chat__close-btn', wait);
        assert.ok(host.querySelector('.chat-toggle') && !host.querySelector('.chat'), 'the close button closes it');
    });
});

test('the keyboard lifts the phone panel over itself, and never the desktop one', async () => {
    const win = dom.window as any;
    // The keyboard takes 368 px of a 768 px window; the visual viewport is 100 px shorter still.
    const raiseKeyboard = async (wait: Wait) => {
        win.innerHeight = 400;
        win.visualViewport.height = 300;
        win.dispatchEvent(new dom.window.Event('resize'));
        await wait(10);
    };
    win.visualViewport = { height: 768, addEventListener() {}, removeEventListener() {} };
    try {
        await withChat(390, async (host, wait) => {
            await click(host, '.chat-toggle', wait);
            assert.equal(host.innerHTML, mobilePanel(''), 'no style before the keyboard is up');
            await raiseKeyboard(wait);
            assert.equal(host.innerHTML, mobilePanel(' style="bottom: 110px; transition: none;"'));
        });
        win.visualViewport.height = 768;
        await withChat(1024, async (host, wait) => {
            await click(host, '.chat-toggle', wait);
            await raiseKeyboard(wait);
            assert.equal(host.innerHTML, desktopPanel(), 'the desktop panel takes no style');
        });
    } finally {
        delete win.visualViewport;
    }
});
