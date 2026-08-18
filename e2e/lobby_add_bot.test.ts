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

const GAME_STATUS = { WAITING: 'waiting', PLAYING: 'playing', GAME_OVER: 'game_over' };
const PLAYER_STATUS = { IDLE: 'idle', READY: 'ready', IN: 'in' };
const STRATEGY_KEY = { HUMAN: 'human' };

// Roster the picker fetches, newest first. Real bot ids on every row.
const ROSTER = [
    { id: 'bot-newest', nickname: '%Cordite', strategy_key: 'cordite' },
    { id: 'bot-old', nickname: '%Handwritten', strategy_key: 'handwritten' },
];

// Controls WHEN the roster fetch resolves, so the test can click mid-load.
let releaseRoster: () => void = () => {};
const rosterGate = new Promise<void>((r) => { releaseRoster = r; });

const addBotCalls: Array<string | undefined> = [];

// Minimal supabase whose bots query resolves only after `releaseRoster()`.
const supabaseMock = {
    from: () => ({
        select: () => ({
            order: () => ({
                then: (cb: (r: { data: any; error: any }) => void) =>
                    rosterGate.then(() => cb({ data: ROSTER, error: null })),
            }),
        }),
    }),
};

const GAME = {
    name: 'G', status: GAME_STATUS.WAITING, self: { player_id: 'h1' },
    players: [{ player_id: 'h1', name: 'Me', status: PLAYER_STATUS.IDLE, is_ai: false, hand_length: 0 }],
};
const SERVER = {
    game: GAME,
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
// Lobby.tsx imports these from @api/core (it used to be @shared, whose alias
// now points at a path with no types.ts — mock.module on an unresolvable
// specifier throws ERR_MODULE_NOT_FOUND and killed the whole file at load).
mock.module('@api/core/types.ts', { namedExports: { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY, PublicPlayer: {} } });
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
    assert.equal(addBotCalls.length, 1, 'the parked click resolved to exactly one add');
    assert.equal(addBotCalls[0], 'bot-newest', 'added the specific (newest) bot, not a random server pick');

    root.unmount();
});
