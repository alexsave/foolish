// fake_backend.ts - a stand-in for src/common/backend.ts, the page's one seam to
// c_base, for the tests that render the real page tree in jsdom.
//
// The page asks backend.ts for envelopes and rows and holds stream
// subscriptions through it; nothing past that seam is the page's. So a test
// mocks this one module (mock.module('../src/common/backend.ts', { namedExports:
// fb.module })), serves real kernel-made envelopes from `fb`, records what the
// page asked and held, and pushes views, casts and chat into the page's
// listeners as the stream would.

import type { StreamListener } from '../../src/common/backend.ts';
import type { NetAction, NetBot, NetBoardRow, NetChat, NetHistoryRow, NetReplay } from '../../sdk/ts/net/net_client.ts';

export interface FakeBackend {
    /** Who is signed in (null: nobody). */
    user: { id: string; username: string } | null;
    /** One game's view by game id (the player's or the spectators', as the server would pick). */
    views: Record<string, Uint8Array>;
    /** The dashboard's list. */
    games: Uint8Array[];
    chats: Record<string, NetChat[]>;
    bots: NetBot[];
    board: NetBoardRow[];
    history: { rating: number | null; rows: NetHistoryRow[] };
    replays: Record<string, NetReplay>;
    /** What meta answers an ask with (default: refused): the asker's envelope, or null for an ask that did not move them. */
    onMeta: (ask: Uint8Array) => Promise<Uint8Array | null>;
    onCreate: () => Promise<Uint8Array>;
    onAction: (gameId: string, wire: Uint8Array, intent: number) => Promise<NetAction>;
    /** Every call the page made, in order: its name and its first argument. */
    calls: { fn: string; arg?: unknown }[];
    /** Every hold the page made ('games', 'game:<id>', 'chat:<id>'), and what is held now (with counts). */
    holdLog: string[];
    held: Map<string, number>;
    listeners: Set<StreamListener>;
    /** Push as the stream would. */
    push: {
        view(gameId: string, envelope: Uint8Array | null): void;
        cast(gameId: string, cast: Uint8Array): void;
        chat(gameId: string, message: NetChat): void;
    };
    /** The module's named exports, for mock.module. */
    module: Record<string, unknown>;
}

class FakeBackendError extends Error {
    constructor(readonly why = 0, readonly code = 0, readonly status = 0) { super(`refused (${why})`); }
}

export function fakeBackend(init: Partial<Pick<FakeBackend, 'user'>> = {}): FakeBackend {
    const refused = async (): Promise<never> => { throw new FakeBackendError(3); };
    const fb: FakeBackend = {
        user: init.user ?? null,
        views: {}, games: [], chats: {}, bots: [], board: [],
        history: { rating: null, rows: [] }, replays: {},
        onMeta: refused, onCreate: refused, onAction: refused,
        calls: [], holdLog: [], held: new Map(), listeners: new Set(),
        push: {
            view: (gid, env) => { for (const l of fb.listeners) l.view?.(gid, env); },
            cast: (gid, cast) => { for (const l of fb.listeners) l.cast?.(gid, cast); },
            chat: (gid, m) => { for (const l of fb.listeners) l.chat?.(gid, m); },
        },
        module: {},
    };
    const log = (fn: string, arg?: unknown) => { fb.calls.push({ fn, arg }); };
    const hold = (key: string) => {
        fb.holdLog.push(key);
        fb.held.set(key, (fb.held.get(key) ?? 0) + 1);
        let on = true;
        return () => {
            if (!on) return;
            on = false;
            const n = (fb.held.get(key) ?? 1) - 1;
            if (n > 0) fb.held.set(key, n); else fb.held.delete(key);
        };
    };
    fb.module = {
        BackendError: FakeBackendError,
        ready: async () => {},
        sessionUser: () => fb.user,
        signIn: async (_up: boolean, name: string) => { log('signIn', name); },
        signOut: async () => { log('signOut'); fb.user = null; },
        updatePassword: async () => { log('updatePassword'); },
        deleteAccount: async () => { log('deleteAccount'); },
        create: async () => { log('create'); return fb.onCreate(); },
        meta: async (ask: Uint8Array) => { log('meta', ask); return fb.onMeta(ask); },
        action: async (gameId: string, wire: Uint8Array, intent = -1) => { log('action', gameId); return fb.onAction(gameId, wire, intent); },
        games: async () => { log('games'); return fb.games; },
        view: async (gameId: string) => { log('view', gameId); return fb.views[gameId] ?? null; },
        chats: async (gameId: string) => { log('chats', gameId); return fb.chats[gameId] ?? []; },
        sendChat: async (gameId: string, message: string) => { log('sendChat', { gameId, message }); },
        bots: async () => { log('bots'); return fb.bots; },
        board: async () => { log('board'); return fb.board; },
        history: async () => { log('history'); return fb.history; },
        replay: async (gameId: string) => { log('replay', gameId); return fb.replays[gameId] ?? null; },
        stream: {
            listen: (l: StreamListener) => { fb.listeners.add(l); return () => fb.listeners.delete(l); },
            hold: (what: number, gameId = '') => hold(`${what}:${gameId}`),
            games: () => hold('games'),
            game: (gameId: string) => hold(`game:${gameId}`),
            chat: (gameId: string) => hold(`chat:${gameId}`),
            send: () => {},
            stop: () => { fb.held.clear(); },
        },
    };
    return fb;
}
