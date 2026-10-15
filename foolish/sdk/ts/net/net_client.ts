// net_client.ts - the backend's client (c/src/netclient.h) as the web reaches it,
// over the kernel link the runtime loads (web.wasm in a browser, bots.wasm on
// Node).
//
// Every decision is the kernel's: what a request says and which token it
// carries, when the token is refreshed, what an answer means, which pushes are
// stale, when a refused subscription is retried and when to ping. This is a
// call-and-copy wrapper like client_table.ts: each method writes its input into
// the module (c/wasm/wasm_net_api.c), has the client read it, and copies the
// answer out through the generated readers (sdk/ts/gen/net_layout.bots.ts)
// before it returns. A span in an answer is sliced out of the caller's own
// bytes. The sockets are src/common/backend.ts's.

import type { WebExports } from '../gen/exports.web.ts';
import * as N from '../gen/net_layout.bots.ts';
import { __clientKernelExports } from '../wasm/bots.ts';

export type NetRequest = N.NcRequest_Snap;
export type NetError = N.NcError_Snap;
export type NetEvent = N.NcEvent_Snap;
export type NetSession = N.NcSessionInfo_Snap;
export type NetAction = N.NcAction_Snap;
export type NetChat = N.NcChatRow_Snap;
export type NetBot = N.NcBotRow_Snap;
export type NetBoardRow = N.NcBoardRow_Snap;
export { N as NetConst };

/** One game's view: the envelope the kernel's client slot adopts, and its version. */
export interface NetView { version: number; envelope: Uint8Array }
/** One replay: the moves code's bytes and its extras (empty for none). */
export interface NetReplay { moves: Uint8Array; extras: Uint8Array }
/** A row of match history, its blobs sliced out. */
export interface NetHistoryRow { id: string; createdMs: number; players: string[]; moves: Uint8Array; extras: Uint8Array }

const enc = new TextEncoder();
const NONE = new Uint8Array(0);
const bytesOf = (v: string | Uint8Array | undefined): Uint8Array =>
    v === undefined ? NONE : typeof v === 'string' ? enc.encode(v) : v;

export class NetClient {
    private readonly ex: WebExports;
    private mem: N.Mem;

    constructor(ex: WebExports) {
        this.ex = ex;
        this.mem = N.memOf(ex.memory.buffer);
    }

    private m(): N.Mem {
        if (this.mem.u8.buffer !== this.ex.memory.buffer) this.mem = N.memOf(this.ex.memory.buffer);
        return this.mem;
    }

    /** Writes the parts back to back at the inbox; their total, or -1 when they cannot be held. */
    private put(...parts: Uint8Array[]): number {
        const total = parts.reduce((n, p) => n + p.length, 0);
        const at = this.ex.wasm_net_inbox(total);
        if (!at) return -1;
        const u8 = this.m().u8;   // after the inbox call: it may have grown memory
        let p = at;
        for (const part of parts) { u8.set(part, p); p += part.length; }
        return total;
    }

    // ---- the session ----

    reset(): void { this.ex.wasm_net_reset(); }

    /** The session as bytes to keep, or null when signed out. */
    sessionBlob(): Uint8Array | null {
        const n = this.ex.wasm_net_session_save();
        if (n <= 0) return null;
        const at = this.ex.wasm_io_ptr();
        return this.m().u8.slice(at, at + n);
    }

    /** Adopt kept bytes; false (and signed out) when they do not read. */
    load(blob: Uint8Array): boolean {
        const n = this.put(blob);
        return n >= 0 && this.ex.wasm_net_session_load(n) === N.NW_OK;
    }

    session(): NetSession { return N.readNcSessionInfo(this.m(), this.ex.wasm_net_session_info()); }

    /** An answer to sign-up, sign-in or refresh: 0 and the session is kept, or the refusal's why. */
    readSession(status: number, body: Uint8Array): number {
        const n = this.put(body);
        return this.ex.wasm_net_session_read(status, Math.max(n, 0));
    }

    /** Why the last answer read was refused. */
    error(): NetError { return N.readNcError(this.m(), this.ex.wasm_net_error()); }

    // ---- requests ----

    /** The request `which` (NC_REQ_*) asks for, or null; a refresh comes first when the token is due. */
    req(which: number, a?: string | Uint8Array, b?: string | Uint8Array, n = 0): NetRequest | null {
        const x = bytesOf(a), y = bytesOf(b);
        if (this.put(x, y) < 0) return null;
        const p = this.ex.wasm_net_req(which, x.length, y.length, n, Date.now());
        return p ? N.readNcRequest(this.m(), p) : null;
    }

    // ---- answers ----

    /** 0 for an accepted request, else the refusal's why (error() says more). */
    status(status: number, body: Uint8Array): number {
        const n = this.put(body);
        return this.ex.wasm_net_status_read(status, Math.max(n, 0));
    }

    action(status: number, body: Uint8Array): NetAction | null {
        const n = this.put(body);
        const p = this.ex.wasm_net_action_read(status, Math.max(n, 0));
        return p ? N.readNcAction(this.m(), p) : null;
    }

    view(status: number, body: Uint8Array): NetView | null {
        const n = this.put(body);
        const p = this.ex.wasm_net_view_read(status, Math.max(n, 0));
        if (!p) return null;
        const r = N.readNcGameRow(this.m(), p);
        return { version: r.version, envelope: body.slice(r.envOff, r.envOff + r.envLen) };
    }

    replay(status: number, body: Uint8Array): NetReplay | null {
        const n = this.put(body);
        const p = this.ex.wasm_net_replay_read(status, Math.max(n, 0));
        if (!p) return null;
        const r = N.readNcReplay(this.m(), p);
        return { moves: body.slice(r.movesOff, r.movesOff + r.movesLen), extras: body.slice(r.extrasOff, r.extrasOff + r.extrasLen) };
    }

    /** Every row of a list of `kind` (NW_GAMES ...), or null with error() set. */
    private rows<T>(status: number, body: Uint8Array, kind: number, read: (m: N.Mem, p: number) => T): T[] | null {
        const len = this.put(body);
        const n = this.ex.wasm_net_list_read(status, Math.max(len, 0), kind);
        if (n < 0) return null;
        const out: T[] = [];
        for (let i = 0; i < n; i++) {
            const p = this.ex.wasm_net_row(kind, len, i);
            if (!p) return null;
            out.push(read(this.m(), p));
        }
        return out;
    }

    games(status: number, body: Uint8Array): NetView[] | null {
        return this.rows(status, body, N.NW_GAMES, N.readNcGameRow)
            ?.map((r) => ({ version: r.version, envelope: body.slice(r.envOff, r.envOff + r.envLen) })) ?? null;
    }
    chats(status: number, body: Uint8Array): NetChat[] | null { return this.rows(status, body, N.NW_CHATS, N.readNcChatRow); }
    bots(status: number, body: Uint8Array): NetBot[] | null { return this.rows(status, body, N.NW_BOTS, N.readNcBotRow); }
    board(status: number, body: Uint8Array): NetBoardRow[] | null { return this.rows(status, body, N.NW_BOARD, N.readNcBoardRow); }

    /** The caller's rating (null for none) and their replays, newest first. */
    history(status: number, body: Uint8Array): { rating: number | null; rows: NetHistoryRow[] } | null {
        const rows = this.rows(status, body, N.NW_HISTORY, N.readNcHistoryRow);
        if (!rows) return null;
        const len = this.put(body);
        const hp = this.ex.wasm_net_history_head(len);
        if (!hp) return null;
        const h = N.readNcHistoryHead(this.m(), hp);
        return {
            rating: h.hasRating ? h.rating : null,
            rows: rows.map((r) => ({
                id: r.id, createdMs: Number(r.createdMs), players: r.players.map((p) => p.id),
                moves: body.slice(r.movesOff, r.movesOff + r.movesLen),
                extras: body.slice(r.extrasOff, r.extrasOff + r.extrasLen),
            })),
        };
    }

    // ---- the stream ----

    subscribe(what: number, gameId = ''): void {
        const g = enc.encode(gameId);
        if (this.put(g) >= 0) this.ex.wasm_net_stream_sub(what, g.length);
    }
    unsubscribe(what: number, gameId = ''): void {
        const g = enc.encode(gameId);
        if (this.put(g) >= 0) this.ex.wasm_net_stream_unsub(what, g.length);
    }
    streamOpened(): void { this.ex.wasm_net_stream_opened(Date.now()); }
    /** The socket closed: how long to wait before reopening it, in ms. */
    streamClosed(): number { return this.ex.wasm_net_stream_closed(Date.now()); }

    /** One frame as it arrived: what to do, and its payload (an envelope or a cast). */
    streamIn(frame: Uint8Array | string): { event: NetEvent; payload: Uint8Array } | null {
        const isText = typeof frame === 'string';
        const n = this.put(bytesOf(frame));
        if (n < 0) return null;
        const p = this.ex.wasm_net_stream_in(n, isText ? 1 : 0, Date.now());
        if (!p) return null;
        const event = N.readNcEvent(this.m(), p);
        const at = this.ex.wasm_net_event_payload();
        const payload = at && event.payloadLen ? this.m().u8.slice(at, at + event.payloadLen) : NONE;
        return { event, payload };
    }

    /** The next frame to send as text, or null for none now. */
    streamNext(): string | null {
        const p = this.ex.wasm_net_stream_next(Date.now());
        return p ? N.readNcOut(this.m(), p).text : null;
    }
    /** How long until there is one (-1: the socket is shut). */
    streamDueMs(): number { return this.ex.wasm_net_stream_due_ms(Date.now()); }

    gateReset(gameId: string): void {
        const g = enc.encode(gameId);
        if (this.put(g) >= 0) this.ex.wasm_net_gate_reset(g.length);
    }
}

let shared: NetClient | null = null;
/** The one client of this page (or process): the module's one set of statics. */
export function netClient(): NetClient {
    return (shared ??= new NetClient(__clientKernelExports() as unknown as WebExports));
}
