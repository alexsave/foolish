/* =============================================================================
 * What usePreventScroll allows, and what it refuses
 * =============================================================================
 * This hook is on every primary surface - Lobby, Tutorial, GameDisplay and the
 * replay stage - and it is the reason dragging a card does not drag the page
 * with it. It is a document-level, NON-PASSIVE touch listener: it decides, for
 * every touch on the site, whether the browser's default gesture happens.
 *
 * That makes it the kind of code where a tidy-up is dangerous, because nothing
 * else in the suite would notice it breaking. A rewrite that stopped calling
 * preventDefault on touchmove would still typecheck, still render, still pass
 * every other test, and the only symptom would be the page sliding around
 * under a dragged card on a phone - which no test here held.
 *
 * So this pins the BEHAVIOUR, not the shape. It was written against the
 * original implementation and passed against it before that implementation was
 * replaced, so it describes what shipped rather than what the rewrite happens
 * to do.
 *
 * It mounts the hook through real React, because the cleanup is half the
 * contract: a document-level non-passive touchmove that is not removed
 * outlives the component and keeps refusing gestures for the life of the tab.
 *
 * Pure test - no Postgres, no network, no compiler.
 */
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';

const dom = new JSDOM('<!DOCTYPE html><html><body><div id="host"></div></body></html>',
    { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
for (const k of ['window', 'document', 'HTMLElement', 'Node', 'Element', 'Event', 'UIEvent',
    'MouseEvent', 'navigator', 'requestAnimationFrame', 'cancelAnimationFrame']) {
    if (g[k] === undefined) g[k] = (dom.window as any)[k];
}
g.IS_REACT_ACT_ENVIRONMENT = true;

let mount: () => Promise<void>;
let unmount: () => Promise<void>;

before(async () => {
    const React = await import('react');
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { usePreventScroll } = await import('../../src/hooks/usePreventScroll.ts');

    const Probe = () => { usePreventScroll(); return null; };
    const host = document.getElementById('host')!;
    const root = createRoot(host);

    mount = async () => { await (act as any)(async () => { root.render(React.createElement(Probe)); }); };
    unmount = async () => { await (act as any)(async () => { root.render(null); }); };
});

/** Dispatch a touch event the way a browser would; report whether it was refused. */
function fire(type: string, target: Element, touchCount: number): boolean {
    const ev: any = new dom.window.Event(type, { bubbles: true, cancelable: true });
    const list = Array.from({ length: touchCount }, () => ({ identifier: 0, target }));
    ev.touches = list;
    ev.changedTouches = list;
    ev.targetTouches = list;
    target.dispatchEvent(ev);
    return ev.defaultPrevented;
}

function el(html: string): Element {
    const host = document.getElementById('host')!;
    host.innerHTML = html;
    return host.firstElementChild!;
}

const CARD = '<div draggable="true">card</div>';
const CHAT = '<div data-chat-scrollable>log</div>';
const BUTTON = '<button>Take</button>';
const INPUT = '<input value="name" />';
const PLAIN = '<div>board</div>';

test('a dragged card is never interfered with, on any touch event', async () => {
    // THE REASON THIS HOOK EXISTS. A card drag must reach the drag layer
    // untouched; if any of these starts refusing, the card stops moving.
    await mount();
    for (const type of ['touchstart', 'touchmove', 'touchend']) {
        for (const touches of [1, 2]) {
            assert.equal(fire(type, el(CARD), touches), false,
                `${type} with ${touches} touch(es) on a draggable card was prevented - the drag is broken`);
        }
    }
    await unmount();
});

test('a touch on a card\'s own child still counts as the card', async () => {
    // Cards render their face inside them, so the touch lands on a child and
    // `closest` is what keeps it a card. Dragging by the pip must work.
    await mount();
    el('<div draggable="true"><span id="pip">A</span></div>');
    const pip = document.getElementById('pip')!;
    assert.equal(fire('touchmove', pip, 1), false,
        'a touch on a card\'s child was prevented - dragging by the pip is broken');
    await unmount();
});

test('the chat log still scrolls', async () => {
    await mount();
    for (const type of ['touchstart', 'touchmove', 'touchend']) {
        assert.equal(fire(type, el(CHAT), 1), false,
            `${type} on [data-chat-scrollable] was prevented - the log cannot scroll`);
    }
    await unmount();
});

test('the page never scrolls under a single finger', async () => {
    // touchmove on anything that is not a card or the chat log is refused
    // outright. This is what stops the board sliding while you play.
    await mount();
    for (const html of [PLAIN, BUTTON, INPUT]) {
        assert.equal(fire('touchmove', el(html), 1), true,
            `a single-finger drag on ${html} was allowed - the page will scroll`);
    }
    await unmount();
});

test('a single tap is never refused, so buttons and inputs still work', async () => {
    await mount();
    for (const html of [BUTTON, INPUT, PLAIN]) {
        assert.equal(fire('touchstart', el(html), 1), false, `a single tap on ${html} was prevented`);
        assert.equal(fire('touchend', el(html), 1), false, `a single tap-release on ${html} was prevented`);
    }
    await unmount();
});

test('pinch-to-zoom is refused everywhere except a card and the chat log', async () => {
    await mount();
    for (const html of [PLAIN, BUTTON, INPUT]) {
        assert.equal(fire('touchstart', el(html), 2), true, `a two-finger start on ${html} was allowed - the page can zoom`);
        assert.equal(fire('touchend', el(html), 2), true, `a two-finger end on ${html} was allowed - the page can zoom`);
    }
    await unmount();
});

test('unmounting removes every listener, so nothing survives the screen', async () => {
    await mount();
    assert.equal(fire('touchmove', el(PLAIN), 1), true, 'the hook did not install at all');
    await unmount();
    assert.equal(fire('touchmove', el(PLAIN), 1), false,
        'touchmove is still prevented after unmount - the listener leaked and will '
        + 'refuse gestures for the life of the tab');
});

after(() => { dom.window.close(); });
