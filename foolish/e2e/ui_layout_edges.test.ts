/* =============================================================================
 * The screen's edges on a phone: what the stylesheet keeps clear of them
 * =============================================================================
 * Two things ran into the edges of a 390 px screen:
 *   - my hand: its row spans the screen and its cards grow to fill it, so with
 *     8 or 9 cards the outer cards sat on the screen's edges, where a phone's
 *     rounded corners and bezel cut them;
 *   - the lobby's title: an input as wide as its default size at 2rem (403 px),
 *     centred, so on a phone it ran under the back button ("<LICE84440's Game").
 *
 * jsdom has no layout, so this reads the cascade: the real stylesheet
 * (src/styles/index.css and its imports) applied to the elements the page draws
 * (e2e/fixtures/ui_dom holds that the page still draws them with these hooks),
 * at a 390 px wide window. The shots that show both screens before and after are
 * in the Part 4 report.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { JSDOM } from 'jsdom';

const STYLES = new URL('../src/styles/', import.meta.url);

/** index.css with its @imports inlined, in order. */
function stylesheet(): string {
    const index = readFileSync(new URL('index.css', STYLES), 'utf8');
    return index.replace(/@import '\.\/([^']+)';/g, (_, path: string) => readFileSync(new URL(path, STYLES), 'utf8'));
}

function page(body: string): JSDOM {
    const dom = new JSDOM(`<!DOCTYPE html><html><head><style>${stylesheet()}</style></head><body>${body}</body></html>`,
        { pretendToBeVisual: true });
    Object.defineProperty(dom.window, 'innerWidth', { value: 390 });
    return dom;
}

const px = (v: string) => {
    const m = /^(\d+(?:\.\d+)?)px$/.exec(v.trim());
    assert.ok(m, `a length in px: "${v}"`);
    return Number(m![1]);
};

test('my hand keeps a gutter from both edges of the screen', () => {
    // The row as ActionButtons draws it: data-hand-container, a flex row as wide as the screen.
    const dom = page('<div data-touch-interactive data-hand-container data-player-id="u-me" style="display: flex; flex-direction: row; align-items: center; justify-content: center; width: 100%;"></div>');
    const row = dom.window.document.querySelector('[data-hand-container]')!;
    const style = dom.window.getComputedStyle(row);
    // The back button's inset (.btn-icon--left: 10px), so the outer cards line up with it.
    assert.ok(px(style.paddingLeft) >= 10, `the left gutter is ${style.paddingLeft}`);
    assert.ok(px(style.paddingRight) >= 10, `the right gutter is ${style.paddingRight}`);
    assert.equal(style.boxSizing, 'border-box', 'the gutter is inside the row\'s width, not added to it');
});

test('the lobby title stays between the back button and its mirror image', () => {
    const dom = page('<div class="lobby"><button class="btn-icon btn-icon--left"></button><input class="lobby__name-input" value="ALICE84440\'s Game"></div>');
    const title = dom.window.document.querySelector('.lobby__name-input')!;
    const style = dom.window.getComputedStyle(title);
    // The back button takes 10px + 44px from the left edge; the title keeps that
    // room, and a margin, on both sides so it stays centred.
    const maxWidth = style.maxWidth.replace(/\s+/g, ' ');
    assert.match(maxWidth, /^calc\(100% - 2 \* \(var\(--btn-icon-size\) \+ (\d+)px\)\)$/, `the title's max-width: "${style.maxWidth}"`);
    const margin = Number(/\+ (\d+)px/.exec(maxWidth)![1]);
    assert.ok(margin >= 10 + 8, `the room beside the button covers its 10px inset and a gap (${margin}px)`);
    assert.equal(style.textOverflow, 'ellipsis', 'a title too long for the room ends in an ellipsis');
});
