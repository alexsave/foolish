/* =============================================================================
 * Labels drawn straight on a texture carry the label shadow
 * =============================================================================
 * Light text on the pale wool or the orange wood reads only with a dark edge
 * under it: the stylesheet's --text-shadow-label token (src/styles/variables.css;
 * none in the Soviet theme, whose surfaces are flat). The win screen's own row -
 * my name in the success green and its "(You)" at 0.75rem - had no shadow at all
 * and all but vanished on the wood.
 *
 * jsdom has no paint, so this reads the cascade: the real stylesheet applied to
 * the markup WinScreen.tsx and Leaderboard.tsx draw (e2e/fixtures/ui_dom
 * win_screen holds that the page still draws these class names).
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { JSDOM } from 'jsdom';

const STYLES = new URL('../src/styles/', import.meta.url);

function stylesheet(): string {
    const index = readFileSync(new URL('index.css', STYLES), 'utf8');
    return index.replace(/@import '\.\/([^']+)';/g, (_, path: string) => readFileSync(new URL(path, STYLES), 'utf8'));
}

test('my row on the win screen: my name and its "(You)" carry the label shadow', () => {
    const dom = new JSDOM(`<!DOCTYPE html><html><head><style>${stylesheet()}</style></head><body>
        <div class="result-card result-card--current-user"><span class="result-card__name result-card__name--current">Me</span><span class="result-card__you">(You)</span></div>
    </body></html>`, { pretendToBeVisual: true });
    const golden = readFileSync(new URL('./fixtures/ui_dom/win_screen.html', import.meta.url), 'utf8');
    for (const cls of ['result-card__name--current', 'result-card__you']) {
        assert.ok(golden.includes(cls), `the win screen still draws .${cls}`);
        const el = dom.window.document.querySelector<HTMLElement>(`.${cls}`)!;
        assert.equal(dom.window.getComputedStyle(el).textShadow, 'var(--text-shadow-label)', `.${cls} carries the label shadow`);
    }
});
