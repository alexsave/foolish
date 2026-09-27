/* =============================================================================
 * The Oracle's "why" proof panel is dark unless the flag is on
 * =============================================================================
 * The panel (docs/INFINITE_ORACLE_DESIGN.md §9.7) landed on main so it would
 * stop rotting in a branch, not so it would ship: the replay route is a
 * self-contained base32 payload that needs no auth and no database row, so a
 * client-side panel cannot be metered. It is gated by ORACLE_WHY_PANEL
 * (src/oracle/types.ts), whose shipping value is false.
 *
 * Two things have to hold, and neither is provable by reading the flag:
 *   - with the flag OFF the overlay is the panel that shipped before this
 *     landed: candidate rows carry no click handler, no pointer cursor and no
 *     proof, and the hint line is not drawn;
 *   - with the flag ON, clicking a row opens the proof.
 *
 * The flag is read through process.env at call time, so this file can drive
 * both states in one process. In the browser Next inlines NEXT_PUBLIC_*, which
 * only makes the OFF case stronger (the panel becomes unreachable code).
 * ========================================================================== */

import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';
import type { OracleSnapshot } from '../src/oracle/types.ts';

const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>', { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
for (const k of ['window', 'document', 'HTMLElement', 'HTMLCanvasElement', 'Node', 'Element', 'MouseEvent',
    'getComputedStyle', 'requestAnimationFrame', 'cancelAnimationFrame', 'localStorage', 'sessionStorage', 'Image']) {
    try { g[k] = (dom.window as any)[k]; } catch { /* a getter already there */ }
}
try { Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true }); } catch { /* ok */ }
g.IS_REACT_ACT_ENVIRONMENT = true;
g.ResizeObserver ??= class { observe() {} unobserve() {} disconnect() {} };
(dom.window.HTMLCanvasElement.prototype as any).getContext = () => null;

const FLAG = 'NEXT_PUBLIC_FOOLISH_ORACLE_WHY';
const saved = process.env[FLAG];
before(() => { delete process.env[FLAG]; });
after(() => { if (saved === undefined) delete process.env[FLAG]; else process.env[FLAG] = saved; });

/** One candidate's merged path data, shaped exactly as the sidecar decodes it. */
const why = (mepk: number, fin: number) => ({
    agg: { n: 400, mepk, oppk: 0.8, metr: 0.5, opptr: 0.4, rnds: 3.2 },
    replies: [{ type: 1, card: 22, n: 180 }],
    paths: [
        { seq: [1, 3], n: 220, fin },
        { seq: [2, 4], n: 120, fin: fin + 0.4 },
    ],
});

const SNAPSHOT: OracleSnapshot = {
    decisionId: 'x:1:1',
    status: 'converged',
    regime: 'mc',
    candidates: [
        {
            key: 'attack|6S|', type: 'attack', label: 'attack 6S', cards: ['6S'],
            n: 400, mean: 1.24, se: 0.01, adjusted: 1.24, verdict: 'none',
            forcedLoss: false, pruned: false, chosen: true, played: true, why: why(0.4, 1.20),
        },
        {
            key: 'attack|9H|', type: 'attack', label: 'attack 9H', cards: ['9H'],
            n: 400, mean: 1.61, se: 0.02, adjusted: 1.61, verdict: 'none',
            forcedLoss: false, pruned: false, chosen: false, played: false, why: why(1.3, 1.55),
        },
    ],
    totalWorlds: 800,
    worldsPerSec: 1000,
    batches: 8,
    elapsedMs: 800,
    memoryOn: true,
    seat: 0,
    recordedKey: 'attack|6S|',
    recordedLabel: 'attack 6S',
    recordedPresent: true,
    approx: false,
    deckAlive: true,
    numPlayers: 2,
    belief: {
        pinned: [[], ['KD*']], voids: [[], ['7C']], floor: [0, 0], poolCount: 11,
        hand: ['6S'], oppCounts: [4, 5], table: [], defender: 1, trump: 3,
    },
};

/** Render the overlay into jsdom and hand back its host element. */
async function overlay(): Promise<{ host: HTMLElement; unmount: () => Promise<void> }> {
    const React = (await import('react')).default;
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { LocalizationProvider } = await import('../src/contexts/LocalizationContext.tsx');
    const { OracleOverlay } = await import('../src/components/OracleOverlay.tsx');
    const host = dom.window.document.createElement('div');
    dom.window.document.body.appendChild(host);
    const root = createRoot(host);
    const noop = () => {};
    await act(async () => {
        root.render(React.createElement(LocalizationProvider, null,
            React.createElement(OracleOverlay, {
                snapshot: SNAPSHOT, onClose: noop, onToggleMemory: noop, onRetry: noop,
            })));
    });
    return {
        host: host as unknown as HTMLElement,
        unmount: async () => { await act(async () => { root.unmount(); }); host.remove(); },
    };
}

const click = async (el: Element) => {
    const { act } = await import('react');
    await act(async () => {
        el.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
    });
};

test('flag off: the candidate rows are inert and no proof is drawn', async () => {
    delete process.env[FLAG];
    const { host, unmount } = await overlay();
    assert.equal(host.querySelectorAll('[data-testid="oracle-row"]').length, 0,
        'no row advertises itself as clickable');
    assert.equal(host.querySelector('[data-testid="oracle-why"]'), null, 'no proof panel');
    // The rows themselves still render: this is the shipped panel, unchanged.
    const panel = host.querySelector('[data-testid="oracle-panel"]');
    assert.ok(panel, 'the overlay still renders');
    const body = panel!.lastElementChild!;
    assert.equal(body.children.length, 3, 'two candidate rows and the footnote block, and nothing else');
    // No invitation to click, and clicking anyway does nothing.
    assert.ok(!body.textContent!.includes('Tap a move'), 'the hint line is not drawn');
    const before = host.innerHTML;
    for (const row of [...body.children]) await click(row);
    assert.equal(host.innerHTML, before, 'clicking a row changes nothing');
    assert.equal(host.querySelector('[data-testid="oracle-why"]'), null, 'still no proof panel');
    await unmount();
});

test('flag on: clicking a candidate opens its measured proof', async () => {
    process.env[FLAG] = '1';
    const { host, unmount } = await overlay();
    const rows = host.querySelectorAll('[data-testid="oracle-row"]');
    assert.equal(rows.length, 2, 'every candidate row is clickable');
    assert.equal(host.querySelector('[data-testid="oracle-why"]'), null, 'closed until clicked');

    await click(rows[1]);
    const panel = host.querySelector('[data-testid="oracle-why"]');
    assert.ok(panel, 'the proof opens on click');
    const text = panel!.textContent ?? '';
    // Real measured numbers, not placeholders: the 0.37 gap to the best move,
    // the dominant cluster's 55% share, and the pickup counterfactual.
    assert.ok(!text.includes('{'), `no unfilled template params: ${text.slice(0, 160)}`);
    assert.ok(text.includes('0.37'), `the gap to the best move: ${text.slice(0, 160)}`);
    assert.ok(text.includes('55%'), `the dominant cluster's share: ${text.slice(0, 160)}`);
    assert.ok(text.includes('1.30'), `the measured pickup rate: ${text.slice(0, 160)}`);
    // The counterfactual names the measured driver of the gap, not just the gap.
    assert.ok(text.includes('biggest measured driver'), `the counterfactual: ${text}`);
    // The most likely reply is a concrete card, decoded from the sidecar's id.
    assert.ok(/covers with J\u2665/.test(text), `the likely reply: ${text}`);
    // The storyline tree is drawn, with its own share percentages.
    assert.ok(panel!.textContent!.includes('FUTURES') || text.includes('%'), 'the tree renders');
    assert.ok(text.includes('11'), 'the unseen-pool count from the belief block');

    // Clicking the same row again closes it (one panel open at a time).
    await click(rows[1]);
    assert.equal(host.querySelector('[data-testid="oracle-why"]'), null, 'a second click closes it');
    await unmount();
});

/* -----------------------------------------------------------------------------
 * The played row is findable, which is a different file's subject but this
 * file's jsdom harness. Rather than stand a second one up to render the same
 * overlay, the marker test rides this one.
 * -------------------------------------------------------------------------- */
test('exactly one row is marked as the move that was played', async () => {
    // THE PANEL IS ABOUT THE MOVE THAT WAS PLAYED, and rows are sorted by
    // estimate, so that row is wherever it deserves to be. On a wide board it
    // is a long way down: one 8p defence ranks 129 replies against twelve that
    // fit the panel, so the row the whole panel exists to show sat 116 rows
    // below the fold. The overlay scrolls it into view once per decision, and
    // `data-played` is how it finds it - a marker with no marker is a scroll
    // that silently does nothing.
    const { host, unmount } = await overlay();
    const marked = host.querySelectorAll('[data-played]');
    assert.equal(marked.length, 1, 'one row carries the played marker');
    // ...and it wraps a row that actually drew. A row's text is empty by
    // construction - the labels are 15-segment glyphs, so every character is
    // an <svg> and textContent sees nothing - which is exactly why this looks
    // for the drawn glyphs instead.
    assert.ok(marked[0].querySelector('svg'), 'the marked row drew its glyphs');
    // The fixture's played move is the FIRST candidate, so the marker must be
    // on the first rendered row and not merely somewhere.
    const rows = [...host.querySelectorAll('[data-testid="oracle-row"]')];
    assert.ok(rows.length >= 2, `both candidate rows render (${rows.length})`);
    assert.ok(marked[0].contains(rows[0]), 'the marker wraps the played candidate, which is first here');
    assert.ok(!marked[0].contains(rows[1]), 'and not the one that was not played');
    await unmount();
});

test('the played row is scrolled into view once, and it is the played row', async () => {
    // SCROLLING IS A BROWSER BEHAVIOUR AND THIS IS JSDOM, which has no
    // scrollIntoView at all - the first cut of this change threw on mount here,
    // which is how the guard in the overlay got written. Defining one makes the
    // call observable: what has to hold is that it fires on the MARKED row and
    // fires ONCE for a decision, because the list re-sorts with every published
    // batch as the estimates sharpen and a panel that re-scrolls on each of
    // them would yank the page away from someone reading it.
    const proto = (dom.window as any).Element.prototype;
    const had = Object.prototype.hasOwnProperty.call(proto, 'scrollIntoView');
    const prev = proto.scrollIntoView;
    const calls: { el: Element; opts: unknown }[] = [];
    proto.scrollIntoView = function (this: Element, opts: unknown) { calls.push({ el: this, opts }); };
    try {
        const { host, unmount } = await overlay();
        assert.equal(calls.length, 1, `scrolled exactly once (${calls.length})`);
        const marked = host.querySelector('[data-played]');
        assert.ok(marked, 'the played row is marked');
        assert.equal(calls[0].el, marked, 'the row it scrolled to is the played one');
        // `nearest` is the difference between "make sure it is on screen" and
        // "drag it to the top even though it already was on screen".
        assert.deepEqual(calls[0].opts, { block: 'nearest' }, 'it does not move a row that is already visible');
        await unmount();
    } finally {
        if (had) proto.scrollIntoView = prev; else delete proto.scrollIntoView;
    }
});
