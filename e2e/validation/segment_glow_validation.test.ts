/* =============================================================================
 * How many filter passes the LED readout costs
 * =============================================================================
 * SegmentText draws the Oracle panel: every character is a 15-segment glyph, and
 * a lit segment glows. The glow is a CSS `drop-shadow`, and Chrome gives every
 * filtered element its own render surface - so WHERE the filter is attached is
 * not a style detail, it is the panel's frame budget.
 *
 * It was attached to each lit <line>. One Oracle panel is ~260 cells, which came
 * to 912 filtered elements, and because they all sit in one stacking context,
 * a single changed digit re-ran all 912 passes. Measured on the replay screen
 * with the oracle deliberating (production build, 6 s, same decision):
 *
 *     per-line filter (before)  15.5 fps   p95 350 ms   14 frames over 100 ms
 *     per-run filter  (after)   48-55 fps  p95  19 ms    0 frames over 100 ms
 *
 * Nothing about that is visible in a screenshot, a typecheck or a render test -
 * the panel looked right the whole time - so this pins the one property that
 * actually carries the cost: a run of cells spends ONE filter per colour, never
 * one per segment. A tidy-up that pushes the glow back down onto the segments
 * would still look identical and would put the 250 ms frames straight back.
 *
 * Pure test - no Postgres, no network, no compiler.
 */
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';

const dom = new JSDOM('<!DOCTYPE html><html><body><div id="host"></div></body></html>',
    { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
for (const k of ['window', 'document', 'HTMLElement', 'Node', 'Element', 'Event', 'UIEvent',
    'MouseEvent', 'SVGElement', 'navigator', 'requestAnimationFrame', 'cancelAnimationFrame']) {
    if (g[k] === undefined) g[k] = (dom.window as any)[k];
}
g.IS_REACT_ACT_ENVIRONMENT = true;

/** Render a SegmentText and hand back the host element. */
let draw: (props: Record<string, unknown>) => Promise<Element>;

before(async () => {
    const React = await import('react');
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { SegmentText } = await import('../../src/components/SegmentDisplay.tsx');

    const host = document.getElementById('host')!;
    const root = createRoot(host);
    draw = async (props) => {
        await (act as any)(async () => { root.render(React.createElement(SegmentText as any, props)); });
        return host;
    };
});

/** Every element carrying its own CSS filter, however it was set. */
const filtered = (host: Element): Element[] =>
    [...host.querySelectorAll('*')].filter((e) => {
        const inline = (e as HTMLElement).style?.filter;
        return !!inline && inline !== 'none';
    });

const AMBER = '#FFA53C';
const TEAL = '#5EEAD4';

// One row of the Oracle panel, as the overlay actually asks for it.
const EF_ROW = { text: 'EF 1.38 ±0.00', color: AMBER, height: 10, gap: 1.5 };

test('a run of cells spends one filter, not one per lit segment', async () => {
    const host = await draw(EF_ROW);

    // The readout is really there: 15 ghost segments per full cell.
    const lines = host.querySelectorAll('line');
    assert.ok(lines.length >= 15 * 8, `expected a full segment array, got ${lines.length} lines`);

    // ...and it costs exactly one filter, on the group - not one per segment.
    const f = filtered(host);
    assert.equal(f.length, 1, `expected 1 filtered element, got ${f.length}`);
    assert.equal(f[0].tagName.toLowerCase(), 'g',
        `the filter belongs on the group, found it on <${f[0].tagName.toLowerCase()}>`);

    // No segment, dot or suit path carries a filter of its own.
    for (const tag of ['line', 'circle', 'path']) {
        const own = [...host.querySelectorAll(tag)]
            .filter((e) => (e as unknown as HTMLElement).style?.filter);
        assert.equal(own.length, 0, `${own.length} <${tag}> elements carry their own filter`);
    }
});

test('the glow itself is unchanged - same drop-shadow, in the cell user space', async () => {
    const host = await draw(EF_ROW);
    const glow = (filtered(host)[0] as HTMLElement).style.filter;
    assert.match(glow, /drop-shadow\(/, `expected a drop-shadow, got ${glow}`);
    // 2 user units of blur at the cell scale: the radius the readout always had.
    assert.match(glow, /0\s+0\s+2px/, `expected the 2px blur radius, got ${glow}`);
    assert.ok(glow.toLowerCase().includes(AMBER.toLowerCase()),
        `expected the run's colour in the glow, got ${glow}`);
});

test('filters scale with colours, not with cells', async () => {
    // A tinted suit letter is a second colour in the same run (colorAt), so the
    // run pays a second filter - and still nothing per segment.
    const host = await draw({
        ...EF_ROW,
        text: 'ATTACK 9S',
        colorAt: (i: number) => (i >= 7 ? TEAL : undefined),
    });
    const f = filtered(host);
    assert.equal(f.length, 2, `two colours should cost two filters, got ${f.length}`);
    assert.deepEqual(f.map((e) => e.tagName.toLowerCase()), ['g', 'g']);

    // The whole panel is this shape: a long run must not drift toward per-cell.
    const long = await draw({ ...EF_ROW, text: 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789' });
    assert.equal(filtered(long).length, 1,
        'a 36-cell run of one colour must still cost exactly one filter');
});

test('a whole run of cells is one <svg>, so one stacking context', async () => {
    const host = await draw({ ...EF_ROW, text: 'ABCDEFGHIJ' });
    assert.equal(host.querySelectorAll('svg').length, 1,
        'contiguous cells belong in a single <svg>');
});
