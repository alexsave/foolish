/* =============================================================================
 * Every language reads the default theme
 * =============================================================================
 * Russian used to switch the site to the Soviet theme (src/contexts/
 * ThemeContext.tsx). Since 2026-10-31 nothing chooses it by language: a visitor
 * reading Russian gets the same theme as everybody else. This mounts the real
 * providers in jsdom with each language stored as the visitor's choice and reads
 * what the page puts on <html> and <body>.
 *
 * Pure test - no Postgres, no network, no compiler.
 * ========================================================================== */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';

const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>', { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
for (const k of ['window', 'document', 'HTMLElement', 'Node', 'Element', 'localStorage', 'sessionStorage']) {
    try { g[k] = (dom.window as any)[k]; } catch { /* a getter already there */ }
}
try { Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true }); } catch { /* ok */ }
g.IS_REACT_ACT_ENVIRONMENT = true;

/** The theme the page wears, and the language it settled on, with `stored` as the visitor's choice. */
async function themeWith(stored: string): Promise<{ theme: string | null; body: string | null; soviet: boolean; language: string }> {
    const React = (await import('react')).default;
    const h = React.createElement;
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { LocalizationProvider, useLocalization } = await import('../src/contexts/LocalizationContext.tsx');
    const { ThemeProvider, useTheme } = await import('../src/contexts/ThemeContext.tsx');
    dom.window.localStorage.setItem('foolish_language', stored);
    let seen = { soviet: true, language: '' };
    const Probe = () => { seen = { soviet: useTheme().isSoviet, language: useLocalization().language }; return null; };
    const host = dom.window.document.createElement('div');
    dom.window.document.body.appendChild(host);
    const root = createRoot(host);
    await act(async () => { root.render(h(LocalizationProvider, null, h(ThemeProvider, null, h(Probe)))); });
    await act(async () => { await new Promise((r) => setTimeout(r, 50)); });
    const out = {
        theme: dom.window.document.documentElement.getAttribute('data-theme'),
        body: dom.window.document.body.getAttribute('data-theme'),
        soviet: seen.soviet, language: seen.language,
    };
    await act(async () => { root.unmount(); });
    host.remove();
    return out;
}

test('a Russian reader gets the default theme, not the Soviet one', async () => {
    const r = await themeWith('ru');
    assert.equal(r.language, 'ru', 'the page is in Russian');
    assert.deepEqual({ theme: r.theme, body: r.body, soviet: r.soviet }, { theme: 'default', body: 'default', soviet: false });
});

test('and so does every other language, including the ones the old map never named', async () => {
    for (const lang of ['en', 'ko', 'de', 'uk', 'zh']) {
        const r = await themeWith(lang);
        assert.equal(r.language, lang);
        assert.equal(r.theme, 'default', `${lang}: the default theme`);
    }
});
