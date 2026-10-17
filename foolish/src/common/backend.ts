// backend.ts - the page's one HTTP transport and one WebSocket to c_base's binary
// surface (c_base/DESIGN.md "Binary surface").
//
// What each request says, which token it carries and when that token is
// refreshed, what an answer means, which pushes are stale, and when a refused
// subscription is retried are the kernel's client's (sdk/ts/net/net_client.ts).
// This file sends what it is given with fetch, keeps the session blob in
// localStorage so a signed-in player stays signed in, opens the stream the
// client names, and hands each push to whoever listens for its game.

import { netClient, NetConst as N, type NetAction, type NetBot, type NetBoardRow, type NetChat, type NetHistoryRow, type NetReplay, type NetRequest } from '@sdk/ts/net/net_client.ts';
import { bytesToBase64, base64ToBytes } from '@sdk/ts/wire/bytes.ts';
import { ensureBotsAsync } from '@sdk/ts/wasm/bots.ts';
import { rememberUser } from './session';

const BASE = process.env.NEXT_PUBLIC_BACKEND_URL ?? '';
const SESSION_KEY = 'foolish.session';

/** A refusal: the kernel's why (ASK_WHY_*), its code and the HTTP status. */
export class BackendError extends Error {
    constructor(readonly why: number, readonly code: number, readonly status: number) {
        super(BackendError.describe(why, code));
    }
    static describe(why: number, code: number): string {
        if (why !== N.ASK_WHY_ACCOUNT) return why === N.ASK_WHY_AUTH ? 'Auth session missing' : `Request refused (${why})`;
        switch (code) {
            case N.NW_ACCOUNT_E_BAD_LOGIN: return 'Invalid login credentials';
            case N.NW_ACCOUNT_E_EXISTS: return 'User already registered';
            case N.NW_ACCOUNT_E_WEAK_PASSWORD: return 'Password should be at least 6 characters.';
            case N.NW_ACCOUNT_E_LONG_PASSWORD: return 'Password cannot be longer than 72 characters';
            case N.NW_ACCOUNT_E_BAD_NAME: return 'USERNAME_RESERVED_PREFIX';
            case N.NW_ACCOUNT_E_SAME_PASSWORD: return 'New password should be different from the old password.';
            default: return 'Auth session missing';
        }
    }
    static last(): BackendError {
        const e = netClient().error();
        return new BackendError(e.why, e.code, e.status);
    }
}

// ---- the session ----------------------------------------------------------------

type SessionListener = () => void;
const sessionListeners = new Set<SessionListener>();
let restored = false;

function storage(): Storage | null {
    try { return typeof localStorage === 'undefined' ? null : localStorage; } catch { return null; }
}

/** Keeps the client's session (or forgets it), and tells whoever shows who is signed in. */
function keepSession(): void {
    const blob = netClient().sessionBlob();
    try {
        if (blob) storage()?.setItem(SESSION_KEY, bytesToBase64(blob));
        else storage()?.removeItem(SESSION_KEY);
    } catch { /* private mode: the session lasts this page */ }
    const s = netClient().session();
    rememberUser(s.signedIn ? { id: s.userId, username: s.username } : null);
    for (const l of sessionListeners) l();
    stream.flush();
}

/** The kernel the client lives in, loaded: wait for this before anything else here. */
export const ready = (): Promise<void> => ensureBotsAsync();

/** The signed-in user, restoring the kept session on first ask (after ready()). */
export function sessionUser(): { id: string; username: string } | null {
    if (!restored) {
        restored = true;
        const kept = storage()?.getItem(SESSION_KEY);
        if (kept) { try { netClient().load(base64ToBytes(kept)); } catch { /* signed out */ } }
        // What the page shows (session.ts) follows what the client holds.
        const s = netClient().session();
        rememberUser(s.signedIn ? { id: s.userId, username: s.username } : null);
        if (typeof window !== 'undefined') {
            // Another tab signed in, out, or refreshed: take its session.
            window.addEventListener('storage', (e) => {
                if (e.key !== SESSION_KEY) return;
                if (e.newValue) netClient().load(base64ToBytes(e.newValue));
                else netClient().reset();
                for (const l of sessionListeners) l();
            });
        }
    }
    const s = netClient().session();
    return s.signedIn ? { id: s.userId, username: s.username } : null;
}

export function onSessionChange(l: SessionListener): () => void {
    sessionListeners.add(l);
    return () => sessionListeners.delete(l);
}

// ---- requests ----------------------------------------------------------------------

async function send(r: NetRequest): Promise<{ status: number; body: Uint8Array }> {
    const headers: Record<string, string> = {};
    if (r.auth) headers.Authorization = r.auth;
    if (r.method === N.NC_POST) headers['Content-Type'] = 'application/octet-stream';
    const res = await fetch(BASE + r.path, {
        method: r.method === N.NC_GET ? 'GET' : 'POST',
        headers,
        body: r.method === N.NC_POST ? new Uint8Array(r.body) : undefined,
    });
    return { status: res.status, body: new Uint8Array(await res.arrayBuffer()) };
}

/**
 * One request (NC_REQ_*), built as many times as it takes: a refresh the client
 * asks for first is sent and kept, then the request is built again. Throws a
 * BackendError when it is refused before it is sent (nobody signed in).
 */
async function call(which: number, a?: string | Uint8Array, b?: string | Uint8Array, n = 0): Promise<{ status: number; body: Uint8Array }> {
    await ready();
    sessionUser();
    for (let i = 0; i < 3; i++) {
        const r = netClient().req(which, a, b, n);
        if (!r) throw new BackendError(N.ASK_WHY_AUTH, 0, 0);
        const answer = await send(r);
        if (!r.isRefresh) return answer;
        const why = netClient().readSession(answer.status, answer.body);
        keepSession();
        if (why !== 0) throw BackendError.last();
    }
    throw new BackendError(N.ASK_WHY_AUTH, 0, 0);
}

/** A request whose answer is only accepted or refused (or the caller's envelope): its body, or a throw. */
async function callOk(which: number, a?: string | Uint8Array, b?: string | Uint8Array): Promise<Uint8Array> {
    const { status, body } = await call(which, a, b);
    if (netClient().status(status, body) !== 0) throw BackendError.last();
    return body;
}

/** Sign up or in with the username as typed (upper-cased here, as both clients always did). */
export async function signIn(up: boolean, name: string, password: string): Promise<void> {
    const { status, body } = await call(up ? N.NC_REQ_SIGNUP : N.NC_REQ_SIGNIN, name.toUpperCase(), password);
    const why = netClient().readSession(status, body);
    keepSession();
    if (why !== 0) throw BackendError.last();
}

/** Sign out everywhere; this page forgets the session whatever the server answered. */
export async function signOut(): Promise<void> {
    try { await call(N.NC_REQ_SIGNOUT, undefined, undefined, 1); } catch { /* signed out here anyway */ }
    signedOut();
}

/** Forget the session on this page (the account is gone, or was signed out). */
export function signedOut(): void {
    stream.stop();
    netClient().reset();
    keepSession();
}

// ---- what the page asks ----------------------------------------------------------
//
// Each answers with what the page uses (an envelope, rows), or throws the
// refusal. The page imports these and never the client: this file is the one
// seam a test stands a fake backend in.

const nc = () => netClient();

/** Create a game: the creator's envelope. */
export const create = (): Promise<Uint8Array> => callOk(N.NC_REQ_CREATE);
/** An ask (ask.h, the kernel's bytes): the caller's envelope. */
export const meta = (ask: Uint8Array): Promise<Uint8Array> => callOk(N.NC_REQ_META, ask);
/** A move: the kernel's action wire for `gameId`, composed against `intent` (none: -1). */
export async function action(gameId: string, wire: Uint8Array, intent = -1): Promise<NetAction> {
    const { status, body } = await call(N.NC_REQ_ACTION, gameId, wire, intent);
    const a = nc().action(status, body);
    if (!a) throw BackendError.last();
    return a;
}
/** An answer read by `read` (null: refused, and the client says why). */
async function answer<T>(which: number, read: (status: number, body: Uint8Array) => T | null, a?: string): Promise<T> {
    const { status, body } = await call(which, a);
    const out = read(status, body);
    if (out === null) throw BackendError.last();
    return out;
}

/** The caller's view of every game they sit at, newest first. */
export const games = async (): Promise<Uint8Array[]> => (await answer(N.NC_REQ_GAMES, (s, b) => nc().games(s, b))).map((r) => r.envelope);
/** One game: the caller's view, or the spectators' for a non-member; null for no such game. */
export async function view(gameId: string): Promise<Uint8Array | null> {
    const { status, body } = await call(N.NC_REQ_GAME, gameId);
    return nc().view(status, body)?.envelope ?? null;
}
/** A game's chat, oldest first (members only). */
export const chats = (gameId: string): Promise<NetChat[]> => answer(N.NC_REQ_CHATS, (s, b) => nc().chats(s, b), gameId);
export const sendChat = (gameId: string, message: string): Promise<Uint8Array> => callOk(N.NC_REQ_CHAT_SEND, gameId, message);
/** The bots, in the server's order. */
export const bots = (): Promise<NetBot[]> => answer(N.NC_REQ_BOTS, (s, b) => nc().bots(s, b));
/** The leaderboard: the top humans and the top bots who have played. */
export const board = (): Promise<NetBoardRow[]> => answer(N.NC_REQ_BOARD, (s, b) => nc().board(s, b));
/** The caller's rating (null for none) and their replays, newest first. */
export const history = (): Promise<{ rating: number | null; rows: NetHistoryRow[] }> => answer(N.NC_REQ_HISTORY, (s, b) => nc().history(s, b));
/** The newest replay of a game the caller played in, or null. */
export async function replay(gameId: string): Promise<NetReplay | null> {
    const { status, body } = await call(N.NC_REQ_REPLAY, gameId);
    return nc().replay(status, body);
}
export const updatePassword = (password: string): Promise<Uint8Array> => callOk(N.NC_REQ_PASSWORD, password);
/** Delete the account, then forget it here. */
export async function deleteAccount(): Promise<void> {
    await callOk(N.NC_REQ_DELETE);
    signedOut();
}

// ---- the stream --------------------------------------------------------------------

export interface StreamListener {
    /** A view of one of my games (the envelope), or null: my seat there is gone. */
    view?: (gameId: string, envelope: Uint8Array | null) => void;
    /** A game's cast: my seat's when `seated`, the spectators' otherwise. */
    cast?: (gameId: string, cast: Uint8Array) => void;
    chat?: (gameId: string, message: NetChat) => void;
    /** A game subscription was accepted (and which stream it hears). */
    joined?: (gameId: string, seated: boolean) => void;
    /** A game subscription was refused (the client retries it by itself). */
    refused?: (gameId: string) => void;
}

class Stream {
    private socket: WebSocket | null = null;
    private wake: ReturnType<typeof setTimeout> | null = null;
    private readonly listeners = new Set<StreamListener>();
    private readonly holds = new Map<string, number>();

    listen(l: StreamListener): () => void {
        this.listeners.add(l);
        return () => this.listeners.delete(l);
    }

    /**
     * Hear `what` (NW_SUB_*) for `gameId` until the returned function is called.
     * Held twice, it is asked again (a spectator who took a seat hears their own
     * seat's casts from then on) and let go only by the last release.
     */
    hold(what: number, gameId = ''): () => void {
        const key = `${what}:${gameId}`;
        this.holds.set(key, (this.holds.get(key) ?? 0) + 1);
        netClient().subscribe(what, gameId);
        this.open();
        this.flush();
        let held = true;
        return () => {
            if (!held) return;
            held = false;
            const n = (this.holds.get(key) ?? 1) - 1;
            if (n > 0) { this.holds.set(key, n); return; }
            this.holds.delete(key);
            netClient().unsubscribe(what, gameId);
            this.flush();
        };
    }

    /** Hold the caller's views of every game they sit at. */
    games(): () => void { return this.hold(N.NW_SUB_GAMES); }
    /** Hold one game's casts: my seat's when I sit there, the spectators' otherwise. */
    game(gameId: string): () => void { return this.hold(N.NW_SUB_GAME, gameId); }
    /** Hold one game's chat (members only). */
    chat(gameId: string): () => void { return this.hold(N.NW_SUB_CHAT, gameId); }

    /** Sends whatever the client has due, and sleeps until the next thing is. */
    flush(): void {
        const ws = this.socket;
        if (!ws) { if (this.holds.size > 0) this.open(); return; }   // e.g. just signed in
        if (ws.readyState !== WebSocket.OPEN) return;
        for (let t = netClient().streamNext(); t !== null; t = netClient().streamNext()) ws.send(t);
        if (this.wake) clearTimeout(this.wake);
        const ms = netClient().streamDueMs();
        this.wake = ms >= 0 ? setTimeout(() => this.flush(), ms) : null;
    }

    stop(): void {
        this.holds.clear();
        if (this.wake) clearTimeout(this.wake);
        this.wake = null;
        const ws = this.socket;
        this.socket = null;
        ws?.close();
    }

    private open(): void {
        if (this.socket || typeof WebSocket === 'undefined' || !sessionUser()) return;
        const path = netClient().req(N.NC_REQ_STREAM)?.path;
        if (!path) return;
        const url = new URL(path, BASE || window.location.origin);
        url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
        const ws = new WebSocket(url);
        ws.binaryType = 'arraybuffer';
        this.socket = ws;
        ws.onopen = () => { netClient().streamOpened(); this.flush(); };
        ws.onmessage = (m) => this.frame(typeof m.data === 'string' ? m.data : new Uint8Array(m.data as ArrayBuffer));
        ws.onclose = () => {
            if (this.socket !== ws) return;
            this.socket = null;
            const ms = netClient().streamClosed();
            setTimeout(() => { if (this.holds.size > 0) this.open(); }, ms);
        };
    }

    private frame(data: string | Uint8Array): void {
        const got = netClient().streamIn(data);
        if (got) {
            const { event: e, payload } = got;
            for (const l of this.listeners) {
                if (e.kind === N.NC_EV_VIEW) l.view?.(e.gid, payload.length ? payload : null);
                else if (e.kind === N.NC_EV_CAST) l.cast?.(e.gid, payload);
                else if (e.kind === N.NC_EV_CHAT) l.chat?.(e.gid, e.chat);
                else if (e.kind === N.NC_EV_SEATED) l.joined?.(e.gid, e.seated === 1);
                else if (e.kind === N.NC_EV_REFUSED && e.what === N.NW_SUB_GAME) l.refused?.(e.gid);
            }
        }
        this.flush();
    }
}

export const stream = new Stream();
