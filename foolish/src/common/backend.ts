// backend.ts - the page's one HTTP transport and one WebSocket to c_base's binary
// surface (c_base/DESIGN.md "Binary surface").
//
// The page says what the user wants (an intent); the kernel's client
// (sdk/ts/net/net_client.ts, c/src/netclient.h) decides everything else: the
// requests and their tokens, refreshes, retries, what an answer means, which
// pushes are stale, what is refetched and what the stream is subscribed to.
// This file runs its transport loop (pump), keeps the session blob in
// localStorage so a signed-in player stays signed in, and hands what changed to
// whoever listens.

import { netClient, NetConst as N, type NetAction, type NetAnswer, type NetBot, type NetBoardRow, type NetChat, type NetHistoryRow, type NetIntent, type NetReplay, type NetRequest } from '@sdk/ts/net/net_client.ts';
import { bytesToBase64, base64ToBytes } from '@sdk/ts/wire/bytes.ts';
import { ensureBotsAsync } from '@sdk/ts/wasm/bots.ts';
import { rememberUser } from './session';

const BASE = process.env.NEXT_PUBLIC_BACKEND_URL ?? '';
const SESSION_KEY = 'foolish.session';
const NONE = new Uint8Array(0);
const enc = new TextEncoder();

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
            case N.NW_ACCOUNT_E_WEAK_PASSWORD: return 'Password should be at least 8 characters, and not a common one.';
            case N.NW_ACCOUNT_E_LONG_PASSWORD: return 'Password cannot be longer than 72 characters';
            case N.NW_ACCOUNT_E_BAD_NAME: return 'USERNAME_RESERVED_PREFIX';
            case N.NW_ACCOUNT_E_SAME_PASSWORD: return 'New password should be different from the old password.';
            case N.NW_ACCOUNT_E_TOO_MANY: return 'Too many attempts. Wait a few minutes and try again.';
            default: return 'Auth session missing';
        }
    }
    static of(e: { why: number; code: number; status: number }): BackendError { return new BackendError(e.why, e.code, e.status); }
}

// ---- the session ----------------------------------------------------------------

let restored = false;

function storage(): Storage | null {
    try { return typeof localStorage === 'undefined' ? null : localStorage; } catch { return null; }
}

/** Shows who the client holds as signed in; `keep`: and keeps its session (or forgets it). */
function showSession(keep: boolean): void {
    const nc = netClient();
    if (keep) {
        const blob = nc.sessionBlob();
        try {
            if (blob) storage()?.setItem(SESSION_KEY, bytesToBase64(blob));
            else storage()?.removeItem(SESSION_KEY);
        } catch { /* private mode: the session lasts this page */ }
    }
    const s = nc.session();
    rememberUser(s.signedIn ? { id: s.userId, username: s.username } : null);
    if (!s.signedIn) stream.stop();
}

/** The kernel the client lives in, loaded: wait for this before anything else here. */
export const ready = (): Promise<void> => ensureBotsAsync();

/** The signed-in user, restoring the kept session on first ask (after ready()). */
export function sessionUser(): { id: string; username: string } | null {
    if (!restored) {
        restored = true;
        const kept = storage()?.getItem(SESSION_KEY);
        if (kept) { try { netClient().load(base64ToBytes(kept)); } catch { /* signed out */ } }
        showSession(false);
        if (typeof window !== 'undefined') {
            // Another tab signed in, out, or refreshed: take its session.
            window.addEventListener('storage', (e) => {
                if (e.key !== SESSION_KEY) return;
                if (e.newValue) netClient().load(base64ToBytes(e.newValue));
                else netClient().reset();
                showSession(false);
                pump();
            });
        }
    }
    const s = netClient().session();
    return s.signedIn ? { id: s.userId, username: s.username } : null;
}

// ---- the transport loop ---------------------------------------------------------------

const waiting = new Map<number, (a: NetAnswer) => void>();
let wake: ReturnType<typeof setTimeout> | null = null;

/** Sends whatever the client has for the server, settles what it finished, and sleeps until it says. */
function pump(): void {
    const nc = netClient();
    for (let r = nc.nextRequest(); r; r = nc.nextRequest()) void send(r);
    for (let d = nc.nextDone(); d; d = nc.nextDone()) settle({ done: d, body: NONE, rows: [] });
    stream.send();
    if (wake) clearTimeout(wake);
    const ms = nc.dueMs();
    wake = ms >= 0 ? setTimeout(pump, ms) : null;
}

async function send(r: NetRequest): Promise<void> {
    let status = 0, body = NONE;   // status 0: it never arrived, and the client knows what that means
    try {
        const res = await fetch(BASE + r.path, {
            method: r.method === N.NC_GET ? 'GET' : 'POST',
            headers: { ...(r.auth ? { Authorization: r.auth } : {}), ...(r.method === N.NC_POST ? { 'Content-Type': 'application/octet-stream' } : {}) },
            body: r.method === N.NC_POST ? new Uint8Array(r.body) : undefined,
        });
        status = res.status;
        body = new Uint8Array(await res.arrayBuffer());
    } catch { /* never arrived */ }
    const a = netClient().onResponse(r.id, status, body);
    changed();
    if (a) settle(a);
    pump();
}

function settle(a: NetAnswer): void {
    const resolve = waiting.get(a.done.ticket);
    waiting.delete(a.done.ticket);
    resolve?.(a);
}

/** One intent, answered: its finished ticket, or a throw with the refusal. */
async function ask(i: NetIntent): Promise<NetAnswer> {
    await ready();
    sessionUser();
    const t = netClient().do(i);
    if (!t) throw BackendError.of(netClient().error());
    const answer = new Promise<NetAnswer>((resolve) => waiting.set(t, resolve));
    pump();
    const a = await answer;
    if (a.done.why) throw BackendError.of(a.done);
    return a;
}

/** What the last answer or frame changed, to whoever listens. */
function changed(): void {
    for (const { event: e, payload } of netClient().events()) {
        if (e.kind === N.NC_EV_SESSION) { showSession(true); continue; }
        for (const l of stream.listeners) {
            if (e.kind === N.NC_EV_VIEW) l.view?.(e.gid, payload.length ? payload : null);
            else if (e.kind === N.NC_EV_CAST) l.cast?.(e.gid, payload);
            else if (e.kind === N.NC_EV_CHAT) l.chat?.(e.gid, e.chat);
        }
    }
}

const span = (a: NetAnswer, off: number, len: number): Uint8Array => a.body.slice(off, off + len);

// ---- what the page asks ----------------------------------------------------------
//
// Each answers with what the page uses (an envelope, rows), or throws the
// refusal. The page imports these and never the client: this file is the one
// seam a test stands a fake backend in.

/** Sign up or in with the username as typed (upper-cased here, as both clients always did). */
export async function signIn(up: boolean, name: string, password: string): Promise<void> {
    await ask({ kind: up ? N.NC_DO_SIGN_UP : N.NC_DO_SIGN_IN, name: name.toUpperCase(), password });
}

/** Sign out everywhere; this page forgets the session whatever the server answered. */
export async function signOut(): Promise<void> {
    try { await ask({ kind: N.NC_DO_SIGN_OUT, flag: 1 }); } catch { /* signed out here anyway */ }
    if (netClient().session().signedIn) { netClient().reset(); showSession(true); }
}

/** Create a game: the creator's envelope. */
export async function create(): Promise<Uint8Array> {
    const a = await ask({ kind: N.NC_DO_CREATE });
    return span(a, a.done.aOff, a.done.aLen);
}
/** An ask (ask.h, the kernel's bytes): the asker's envelope when it moved them (a join, their own leave), else null. */
export async function meta(bytes: Uint8Array): Promise<Uint8Array | null> {
    const a = await ask({ kind: N.NC_DO_ASK, bytes });
    return a.done.aLen ? span(a, a.done.aOff, a.done.aLen) : null;
}
/** A move: the kernel's action wire for `gameId`, composed against `intent` (none: -1). */
export async function action(gameId: string, wire: Uint8Array, intent = -1): Promise<NetAction> {
    const { done: d } = await ask({ kind: N.NC_DO_PLAY, gid: gameId, bytes: wire, intent });
    return { status: d.actionStatus, rejectCode: d.rejectCode, version: d.version };
}
/** The caller's view of every game they sit at, newest first. */
export async function games(): Promise<Uint8Array[]> {
    const a = await ask({ kind: N.NC_DO_GAMES });
    return (a.rows as { envOff: number; envLen: number }[]).map((r) => span(a, r.envOff, r.envLen));
}
/**
 * One game: the caller's view, or the spectators' for a non-member; null for
 * no such game; 'stale' when what the page was handed meanwhile is newer.
 */
export async function view(gameId: string): Promise<Uint8Array | null | 'stale'> {
    const a = await ask({ kind: N.NC_DO_OPEN_GAME, gid: gameId }).catch(() => null);
    if (!a) return null;
    return a.done.stale ? 'stale' : span(a, a.done.aOff, a.done.aLen);
}
/** A game's chat, oldest first (members only). */
export const chats = async (gameId: string): Promise<NetChat[]> => (await ask({ kind: N.NC_DO_CHAT_HISTORY, gid: gameId })).rows as NetChat[];
export async function sendChat(gameId: string, message: string): Promise<void> {
    await ask({ kind: N.NC_DO_SEND_CHAT, gid: gameId, bytes: enc.encode(message) });
}
/** The bots, in the server's order. */
export const bots = async (): Promise<NetBot[]> => (await ask({ kind: N.NC_DO_BOTS })).rows as NetBot[];
/** The leaderboard: the top humans and the top bots who have played. */
export const board = async (): Promise<NetBoardRow[]> => (await ask({ kind: N.NC_DO_LEADERBOARD })).rows as NetBoardRow[];
/** The caller's rating (null for none) and their replays, newest first. */
export async function history(): Promise<{ rating: number | null; rows: NetHistoryRow[] }> {
    const a = await ask({ kind: N.NC_DO_HISTORY });
    const rows = a.rows as { id: string; createdMs: bigint; nPlayers: number; seat: number; rank: number; fool: number; durationS: number; names: { name: string }[] }[];
    return {
        rating: a.done.hasRating ? a.done.rating : null,
        rows: rows.map((r) => ({ id: r.id, createdMs: Number(r.createdMs), nPlayers: r.nPlayers, seat: r.seat, rank: r.rank, fool: r.fool, durationS: r.durationS, names: r.names.map((n) => n.name) })),
    };
}
/** The newest replay of a game the caller played in (the game's id or a history row's), or null. */
export async function replay(gameId: string): Promise<NetReplay | null> {
    const a = await ask({ kind: N.NC_DO_REPLAY, gid: gameId }).catch(() => null);
    return a ? { moves: span(a, a.done.aOff, a.done.aLen), extras: span(a, a.done.bOff, a.done.bLen) } : null;
}
/** A new password; the current one proves the caller owns the account. Every other session ends. */
export async function updatePassword(current: string, password: string): Promise<void> {
    await ask({ kind: N.NC_DO_PASSWORD, password, bytes: enc.encode(current) });
}
/** Delete the account; the session ends with it. */
export async function deleteAccount(): Promise<void> {
    await ask({ kind: N.NC_DO_DELETE_ACCOUNT });
}

// ---- the stream --------------------------------------------------------------------

export interface StreamListener {
    /** A view of a game (the envelope), or null: my seat there is gone. */
    view?: (gameId: string, envelope: Uint8Array | null) => void;
    /** A game's cast: my seat's when I sit there, the spectators' otherwise. */
    cast?: (gameId: string, cast: Uint8Array) => void;
    chat?: (gameId: string, message: NetChat) => void;
}

class Stream {
    private socket: WebSocket | null = null;
    private holds = 0;
    readonly listeners = new Set<StreamListener>();

    listen(l: StreamListener): () => void {
        this.listeners.add(l);
        return () => this.listeners.delete(l);
    }

    /** Hear `what` (NW_SUB_*) for `gameId` until the returned function is called. */
    hold(what: number, gameId = ''): () => void {
        netClient().subscribe(what, gameId);
        this.holds++;
        this.open();
        pump();
        let held = true;
        return () => {
            if (!held) return;
            held = false;
            this.holds--;
            netClient().unsubscribe(what, gameId);
            pump();
        };
    }

    /** Hold the caller's views of every game they sit at. */
    games(): () => void { return this.hold(N.NW_SUB_GAMES); }
    /** Hold one game's casts: my seat's when I sit there, the spectators' otherwise. */
    game(gameId: string): () => void { return this.hold(N.NW_SUB_GAME, gameId); }
    /** Hold one game's chat (members only). */
    chat(gameId: string): () => void { return this.hold(N.NW_SUB_CHAT, gameId); }

    /** Sends every frame the client has for the socket. */
    send(): void {
        const ws = this.socket;
        if (!ws) { if (this.holds > 0) this.open(); return; }   // e.g. just signed in
        if (ws.readyState !== WebSocket.OPEN) return;
        for (let b = netClient().streamNext(); b !== null; b = netClient().streamNext()) ws.send(b);
    }

    stop(): void {
        const ws = this.socket;
        this.socket = null;
        ws?.close();
    }

    private open(): void {
        if (this.socket || typeof WebSocket === 'undefined' || !sessionUser()) return;
        const url = new URL(netClient().streamPath(), BASE || window.location.origin);
        url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
        const ws = new WebSocket(url, netClient().streamProtocol());
        ws.binaryType = 'arraybuffer';
        this.socket = ws;
        ws.onopen = () => { netClient().streamOpened(); pump(); };
        ws.onmessage = (m) => {
            netClient().streamIn(typeof m.data === 'string' ? m.data : new Uint8Array(m.data as ArrayBuffer));
            changed();
            pump();
        };
        ws.onclose = () => {
            if (this.socket !== ws) return;
            this.socket = null;
            setTimeout(() => { if (this.holds > 0) this.open(); }, netClient().streamClosed());
        };
    }
}

export const stream = new Stream();
