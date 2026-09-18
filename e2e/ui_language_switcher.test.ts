/* =============================================================================
 * The language picker, opened
 * =============================================================================
 * The DOM goldens (e2e/ui_dom_snapshots.test.ts) render the dashboard and the
 * match history, and the switcher appears in both - but only CLOSED. Its panel
 * is the part that changed when the site went from three languages to
 * twenty-five, and a golden of a closed button would bless any panel at all,
 * including no panel.
 *
 * So this opens it. It is a small test with a sharp job: the list a player sees
 * has to be the list the C declares, each language written the way its own
 * speakers write it, and the two right-to-left languages marked as such.
 *
 * Why that matters more than it looks: the endonym is the one string on the
 * page that a player who cannot read the current language must still be able
 * to find. If the picker says "Chinese" in English to somebody who reads only
 * Chinese, it is pointing at the way out in a language they do not speak.
 *
 * Pure test - no Postgres, no network, no compiler.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';

const dom = new JSDOM('<!DOCTYPE html><html><body></body></html>', { url: 'http://localhost/', pretendToBeVisual: true });
const g = globalThis as any;
for (const k of ['window', 'document', 'HTMLElement', 'Node', 'Element', 'MouseEvent', 'KeyboardEvent',
    'getComputedStyle', 'requestAnimationFrame', 'cancelAnimationFrame', 'localStorage', 'sessionStorage']) {
    try { g[k] = (dom.window as any)[k]; } catch { /* a getter already there */ }
}
try { Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true }); } catch { /* ok */ }
g.IS_REACT_ACT_ENVIRONMENT = true;
g.ResizeObserver ??= class { observe() {} unobserve() {} disconnect() {} };

/** Mount the switcher inside a real LocalizationProvider and open the panel. */
async function openPanel() {
    const React = (await import('react')).default;
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { LocalizationProvider } = await import('../src/contexts/LocalizationContext.tsx');
    const { LanguageSwitcher } = await import('../src/components/LanguageSwitcher.tsx');

    const host = dom.window.document.createElement('div');
    dom.window.document.body.appendChild(host);
    const root = createRoot(host);
    await act(async () => {
        root.render(React.createElement(LocalizationProvider, null, React.createElement(LanguageSwitcher)));
    });

    const button = host.querySelector('button.btn-language') as HTMLElement;
    const closed = host.innerHTML;
    await act(async () => { button.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true })); });

    const items = [...host.querySelectorAll('.language-menu__item')] as HTMLElement[];
    const opened = host.innerHTML;
    await act(async () => { root.unmount(); });
    host.remove();
    return { button, closed, opened, items };
}

test('closed, the button shows the language that is active', async () => {
    const { closed } = await openPanel();
    assert.match(closed, /aria-label="English"/);
    assert.match(closed, /aria-expanded="false"/);
    assert.match(closed, /aria-haspopup="listbox"/);
    // One button when closed. The panel is not rendered at all, so nothing of it
    // can overlay the page until a player asks for it.
    assert.equal((closed.match(/<button/g) ?? []).length, 1, 'the closed switcher is one button');
    assert.ok(!closed.includes('language-menu'), 'the closed switcher renders no panel');
});

test('opened, the panel lists every language the C declares', async () => {
    const { FoolishLanguages } = await import('../sdk/ts/gen/i18n/languages.ts');
    const { items, opened } = await openPanel();
    assert.equal(items.length, FoolishLanguages.length,
        `the panel shows ${items.length} languages, the registry declares ${FoolishLanguages.length}`);
    assert.match(opened, /aria-expanded="true"/);

    // Row order and content come from the registry, in its order.
    const shown = items.map((li) => ({
        name: (li.querySelector('.language-menu__name') as HTMLElement).textContent,
        code: (li.querySelector('.language-menu__code') as HTMLElement).textContent,
        dir: li.getAttribute('dir'),
        selected: li.getAttribute('aria-selected'),
    }));
    assert.deepEqual(shown.map((s) => s.name), FoolishLanguages.map((l) => l.display),
        'the panel does not name the languages the way the registry does');
    assert.deepEqual(shown.map((s) => s.code), FoolishLanguages.map((l) => l.label));
});

test('each language names itself, in its own script', async () => {
    // The endonym is the way out for a player who cannot read the language the
    // page is currently in, so it must not be the English name. Checked on the
    // scripts where "written in its own script" is machine-decidable.
    const { items } = await openPanel();
    const byName = new Map(items.map((li) => [
        (li.querySelector('.language-menu__code') as HTMLElement).textContent,
        (li.querySelector('.language-menu__name') as HTMLElement).textContent ?? '',
    ]));
    const script: Record<string, RegExp> = {
        'РУ': /^[Ѐ-ӿ]/,      // Cyrillic
        'УК': /^[Ѐ-ӿ]/,
        '한': /^[가-힯]/,       // Hangul
        '中': /^[一-鿿]/,       // Han
        '日': /^[぀-ヿ一-鿿]/,
        'ไทย': /^[฀-๿]/,     // Thai
        'עב': /^[֐-׿]/,      // Hebrew
        'ع': /^[؀-ۿ]/,       // Arabic
    };
    const wrong: string[] = [];
    for (const [label, re] of Object.entries(script)) {
        const name = byName.get(label);
        if (!name || !re.test(name)) wrong.push(`${label}: ${name ?? '(absent)'}`);
    }
    assert.deepEqual(wrong, [], `these languages are not named in their own script:\n  ${wrong.join('\n  ')}`);
});

test('the right-to-left languages are marked right to left, and only those', async () => {
    const { FoolishLanguages } = await import('../sdk/ts/gen/i18n/languages.ts');
    const { items } = await openPanel();
    const rtl = items
        .map((li, i) => ({ code: FoolishLanguages[i].code, dir: li.getAttribute('dir') }))
        .filter((r) => r.dir === 'rtl')
        .map((r) => r.code);
    assert.deepEqual(rtl, ['he', 'ar'],
        'the panel marks a different set of languages right-to-left than c/i18n/languages.h does');
});

test('choosing a language closes the panel and moves the button to it', async () => {
    const React = (await import('react')).default;
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { LocalizationProvider } = await import('../src/contexts/LocalizationContext.tsx');
    const { LanguageSwitcher } = await import('../src/components/LanguageSwitcher.tsx');

    const host = dom.window.document.createElement('div');
    dom.window.document.body.appendChild(host);
    const root = createRoot(host);
    await act(async () => {
        root.render(React.createElement(LocalizationProvider, null, React.createElement(LanguageSwitcher)));
    });
    const click = async (el: Element) => {
        await act(async () => { el.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true })); });
    };
    await click(host.querySelector('button.btn-language')!);
    const ru = [...host.querySelectorAll('.language-menu__item')].find(
        (li) => li.querySelector('.language-menu__code')?.textContent === 'РУ')!;
    assert.ok(ru, 'Russian is in the panel');
    await click(ru);

    assert.ok(!host.innerHTML.includes('language-menu'), 'the panel closes when a language is chosen');
    assert.match(host.innerHTML, /aria-label="Русский"/, 'the button moves to the chosen language');
    await act(async () => { root.unmount(); });
    host.remove();
});
