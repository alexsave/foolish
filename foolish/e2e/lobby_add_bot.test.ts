// E2E for the lobby bot picker (src/components/Lobby.tsx) rendered for real in
// jsdom — the deployed component, its effects, and its click handlers, with only
// the surrounding contexts / supabase client stubbed.
//
// Guards the "adds a random bot" regression: the bot roster loads over the network,
// and until it lands the "Add Bot" button has no specific bot selected. A click in
// that window used to fire addBot() with NO bot_id, so the server fell back to a
// RANDOM pick — exactly the create-game→add-bot flow where the roster is still in
// flight. The fix parks the click and, once the roster loads, adds the SPECIFIC bot
// the picker points at. This test clicks during the load and asserts a specific
// bot id is sent (never undefined).
//
// It also pins WHICH bot that is. The picker walks the ladder - weakest rung
// first, then instance number, from the kernel's `tier` - so a parked click adds
// the bot the picker opens on. That used to be whichever row was newest, which
// is seed.sql's insert order backwards.
//
// Needs --experimental-test-module-mocks (see the test:e2e script).

import { test, mock, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';

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


// Roster the picker fetches. Deliberately NOT in ladder order and not in insert
// order either: the picker sorts it by the kernel's tier
// (src/common/botLadder.ts), so handwritten (tier 3) must come out ahead of
// cordite (tier 9) whatever order the rows arrive in. Real bot ids on every row.
const ROSTER = [
    { id: 'bot-cordite-2', nickname: '%Cordite 2', strategy_key: 'cordite' },
    { id: 'bot-handwritten-3', nickname: '%Handwritten 3', strategy_key: 'handwritten' },
    { id: 'bot-handwritten-1', nickname: '%Handwritten 1', strategy_key: 'handwritten' },
];

// Controls WHEN the roster fetch resolves, so the test can click mid-load.
let releaseRoster: () => void = () => {};
const rosterGate = new Promise<void>((r) => { releaseRoster = r; });

const addBotCalls: Array<string | undefined> = [];

// Minimal supabase whose bots query resolves only after `releaseRoster()`.
const supabaseMock = {
    from: () => ({
        // No .order(): the ladder is the kernel's tier, not a column, so the
        // query is a plain select and the component sorts what comes back.
        select: () => ({
            then: (cb: (r: { data: any; error: any }) => void) =>
                rosterGate.then(() => cb({ data: ROSTER, error: null })),
        }),
    }),
};

// The board the lobby renders (a TableView snapshot): one human seated, the viewer.
const GAME = {
    gameId: 'abcde', title: 'G', status: 0, mySeat: 0,
    seats: [{ id: 'h1', name: 'Me', status: 0, isAi: false, handCount: 0, awaitingAttack: false }],
};
const SERVER = {
    view: GAME,
    updateGameName: () => Promise.resolve(),
    rearrangePlayer: () => Promise.resolve(),
    addBot: (_gameId: string, botId?: string) => { addBotCalls.push(botId); return Promise.resolve({ game_id: _gameId }); },
    exitGame: () => Promise.resolve(),
    joinGame: () => Promise.resolve(),
    startGame: () => Promise.resolve(),
};

// Stub every module Lobby imports (paths resolve the same absolute files Lobby does).
mock.module('../src/contexts/ServerContext.tsx', { namedExports: { useServer: () => SERVER } });
mock.module('next/navigation', { namedExports: { useParams: () => ({ game_id: 'ABCDE' }), useRouter: () => ({ push: () => {} }) } });
mock.module('../src/constants/constants.ts', { namedExports: { WEBSITE_DOMAIN: 'example.com' } });
mock.module('../src/contexts/AuthContext.tsx', { namedExports: { useAuth: () => ({ user_id: 'h1' }) } });
mock.module('qrcode.react', { namedExports: { QRCodeSVG: () => null } });
mock.module('../src/backend/Connector.ts', { defaultExport: supabaseMock });
mock.module('../src/hooks/usePreventScroll.ts', { namedExports: { usePreventScroll: () => {} } });
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
mock.module('@api/core/constants.ts', { namedExports: { MAX_PLAYERS: 6 } });

test('lobby: clicking Add Bot before the roster loads adds a SPECIFIC bot, not a random one', async () => {
    const React = (await import('react')).default;
    const { createRoot } = await import('react-dom/client');
    const { act } = await import('react');
    const { Lobby } = await import('../src/components/Lobby.tsx');

    const container = dom.window.document.getElementById('root')!;
    const root = createRoot(container);

    await act(async () => { root.render(React.createElement(Lobby)); });

    // Roster is still in flight → the button shows the plain "Add Bot" (no bot picked yet).
    const label = () => container.querySelector('.btn-add-bot__text')?.textContent;
    assert.equal(label(), 'add_bot', 'roster not loaded yet → plain Add Bot');

    // The user clicks NOW, mid-load (the create-game→add-bot flow).
    await act(async () => {
        container.querySelector('.btn-add-bot')!.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
        await Promise.resolve();
    });
    // The click must NOT have fired a bot_id-less (random) request.
    assert.deepEqual(addBotCalls, [], 'early click is parked, not sent as a random add');

    // Roster arrives.
    releaseRoster();
    await act(async () => { await rosterGate; await Promise.resolve(); await Promise.resolve(); });

    // Now exactly one add happened, for the SPECIFIC newest bot — never undefined.
    // …and for the bot the ladder opens on: weakest rung, lowest instance number.
    // Handwritten 1 arrives LAST in the mock's rows, so this also pins that the
    // picker orders by tier rather than by arrival.
    assert.equal(addBotCalls.length, 1, 'the parked click resolved to exactly one add');
    assert.equal(addBotCalls[0], 'bot-handwritten-1',
        'added the bot the ladder opens on (weakest rung, first instance), not a random server pick');

    root.unmount();
});
