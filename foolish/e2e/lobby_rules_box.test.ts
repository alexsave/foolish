// The lobby's rules box on the website (docs/PODKIDNOY.md "The web lobby"),
// rendered for real in jsdom: the deployed Lobby.tsx, its click handler and the
// kernel's own envelope for each viewer, with only the surrounding contexts and
// the backend seam stubbed.
//
// What is pinned: unticking the box sends set-rules with passing off, for this
// game; a spectator sees the box and cannot move it (a click sends nothing),
// because who may move it is the kernel's rule (ViewRules.canSetRules), read and
// never restated; and a podkidnoy table draws the box unticked. The server's
// half (who may set rules, every viewer reads them, the deal plays them) is the
// kernel's test_table_set_rules and test_masked_board_carries_the_rules, run by
// c_base through the same table code; it was e2e/web_lobby_rules.test.ts against
// the TypeScript server until stage 4 retired it.
//
// Needs --experimental-test-module-mocks (see the test:e2e script).

import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';
import { fixture, IDLE } from './helpers/table_fixture.ts';
import { fixtureView } from './helpers/table_mem.ts';
import { fakeBackend } from './helpers/fake_backend.ts';

// ---- jsdom DOM env the React client renders into ----
const dom = new JSDOM('<!DOCTYPE html><html><body><div id="root"></div></body></html>', { url: 'http://localhost/' });
const g = globalThis as any;
g.window = dom.window;
g.document = dom.window.document;
try { Object.defineProperty(globalThis, 'navigator', { value: dom.window.navigator, configurable: true }); } catch { /* already a getter */ }
g.HTMLElement = dom.window.HTMLElement;
g.Node = dom.window.Node;
g.getComputedStyle = dom.window.getComputedStyle;
g.MouseEvent = dom.window.MouseEvent;
g.IS_REACT_ACT_ENVIRONMENT = true;


const ANA = 'u-ana', BO = 'u-bo', SAM = 'u-sam';
const table = (passing: boolean) => fixture().title('Rules').passing(passing)
    .seats([{ id: ANA, name: 'Ana' }, { id: BO, name: 'Bo' }]).seatStatus(0, IDLE).seatStatus(1, IDLE).build();

// What the page holds and who it is; each case sets them before it renders.
let viewer = ANA;
const setRulesCalls: Array<{ gameId: string; passing: boolean }> = [];
const SERVER: any = {
    view: null,
    updateGameName: () => Promise.resolve(),
    setRules: (gameId: string, passing: boolean) => { setRulesCalls.push({ gameId, passing }); return Promise.resolve(); },
    rearrangePlayer: () => Promise.resolve(),
    addBot: () => Promise.resolve(),
    exitGame: () => Promise.resolve(),
    joinGame: () => Promise.resolve(),
    startGame: () => Promise.resolve(),
};
const fb = fakeBackend({ user: { id: ANA, username: 'ANA' } });

// Stub every module Lobby imports (paths resolve the same absolute files Lobby does).
mock.module('../src/contexts/ServerContext.tsx', { namedExports: { useServer: () => SERVER } });
mock.module('next/navigation', { namedExports: { useParams: () => ({ game_id: 'ABCDE' }), usePathname: () => '/ABCDE', useRouter: () => ({ push: () => {} }) } });
mock.module('../src/common/constants.ts', { namedExports: { WEBSITE_DOMAIN: 'example.com' } });
mock.module('../src/contexts/AuthContext.tsx', { namedExports: { useAuth: () => ({ user_id: viewer }) } });
mock.module('qrcode.react', { namedExports: { QRCodeSVG: () => null } });
mock.module('../src/common/backend.ts', { namedExports: fb.module });
mock.module('../src/utils/usePreventScroll.ts', { namedExports: { usePreventScroll: () => {} } });
mock.module('../src/components/TexturedSurface.tsx', { namedExports: {
    useTexture: () => ({ woodUrl: null, concreteUrl: null }),
    getTextureStyle: () => ({}), seedFromString: () => 0.5, flipFromString: () => 1,
} });
mock.module('../src/components/WoolBackgroundLayer.tsx', { namedExports: { WoolBackgroundLayer: () => null } });
mock.module('../src/components/BackButton.tsx', { namedExports: { BackButton: () => null } });
mock.module('../src/components/Text.tsx', { namedExports: { Text: () => null } });
mock.module('../src/contexts/LocalizationContext.tsx', { namedExports: { useLocalization: () => ({ t: (id: string, v?: any) => (v?.name ? `Add ${v.name}` : id) }) } });
mock.module('../src/components/SovietIcon.tsx', { namedExports: { SovietIcon: () => null } });
mock.module('../src/contexts/StyleContext.tsx', { namedExports: { useStyles: () => ({ texture: { useWoodTexture: false } }) } });
// Lobby.tsx reads the board's statuses from src/state/view.ts (the kernel's
// generated constants), which it loads for real, as the page does.

async function render(view: unknown): Promise<{ box: () => HTMLInputElement; unmount: () => void }> {
    SERVER.view = view;
    const React = (await import('react')).default;
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { Lobby } = await import('../src/components/Lobby.tsx');
    const container = dom.window.document.getElementById('root')!;
    const root = createRoot(container);
    await act(async () => { root.render(React.createElement(Lobby)); });
    const box = () => {
        const input = container.querySelector<HTMLInputElement>('.lobby__rules input[type="checkbox"]');
        assert.ok(input, `the lobby draws the rules box (${container.textContent?.slice(0, 160)})`);
        return input;
    };
    return { box, unmount: () => { act(() => root.unmount()); } };
}

// HTMLElement.click(), as a user's click: a disabled control does not fire.
async function click(el: HTMLElement): Promise<void> {
    const { act } = await import('react');
    await act(async () => { el.click(); await Promise.resolve(); });
}

test('rules box: a seat unticks it and the page asks for podkidnoy, for this game', async () => {
    viewer = ANA;
    setRulesCalls.length = 0;
    const page = await render(fixtureView(table(true), 0, 'abcde'));
    assert.equal(page.box().checked, true, 'a new table: the box is ticked, the passing game');
    assert.equal(page.box().disabled, false, 'a seated player in the lobby may move it');
    await click(page.box());
    assert.deepEqual(setRulesCalls, [{ gameId: 'abcde', passing: false }], 'unticking sends one set-rules, passing off');
    page.unmount();
});

test('rules box: a podkidnoy table draws it unticked, for a seat and a spectator alike', async () => {
    viewer = BO;
    const seated = await render(fixtureView(table(false), 1, 'abcde'));
    assert.equal(seated.box().checked, false, "Bo's lobby opens on Ana's choice: podkidnoy");
    assert.equal(seated.box().disabled, false, 'and Bo, seated, may change it too');
    seated.unmount();
    viewer = SAM;
    const spectator = await render(fixtureView(table(false), -1, 'abcde'));
    assert.equal(spectator.box().checked, false, 'the spectator reads podkidnoy');
    spectator.unmount();
});

test('rules box: a spectator cannot move it, and a click sends nothing', async () => {
    viewer = SAM;
    setRulesCalls.length = 0;
    const page = await render(fixtureView(table(true), -1, 'abcde'));
    assert.equal(page.box().disabled, true, 'the box is not the spectator\'s to move');
    await click(page.box());
    assert.deepEqual(setRulesCalls, [], "a click on a spectator's box sends nothing");
    page.unmount();
});
