// net_client.ts - the backend's client (c/src/netclient.h) as the web reaches it,
// over the kernel link the runtime loads (web.wasm in a browser, bots.wasm on
// Node).
//
// Every decision is the kernel's: which request an intent becomes, its token,
// when the token is refreshed, what is sent again and when, what an answer
// means, which pushes are stale, what is refetched and what the stream is
// subscribed to. This is a call-and-copy wrapper like client_table.ts: each
// method writes its input into the module (c/wasm/wasm_net_api.c), has the
// client take it, and copies what came back out through the generated readers
// (sdk/ts/gen/net_layout.bots.ts) before it returns. The sockets are
// src/common/backend.ts's.

import type { WebExports } from '../gen/exports.web.ts';
import * as N from '../gen/net_layout.bots.ts';
import { __clientKernelExports } from '../wasm/bots.ts';

export type NetRequest = N.NcRequest_Snap;
export type NetDone = N.NcDone_Snap;
export type NetEvent = N.NcEvent_Snap;
export type NetSession = N.NcSessionInfo_Snap;
export type NetChat = N.NcChatRow_Snap;
export type NetBot = N.NcBotRow_Snap;
export type NetBoardRow = N.NcBoardRow_Snap;
export { N as NetConst };

/** What the user wants (netclient.h NcIntent): its NC_DO_* and what that one takes. */
export interface NetIntent {
    kind: number;
    gid?: string;
    name?: string;
    password?: string;
    bytes?: Uint8Array;
    flag?: number;
    /** NC_DO_PLAY: the version the move was composed against (-1: none). */
    intent?: number;
}
/** A finished ticket, the body its answer came in (spans are into it), and a list's rows. */
export interface NetAnswer { done: NetDone; body: Uint8Array; rows: unknown[] }
/** A move's answer: TABLE_STATUS_*, the reject code, the version it was judged at. */
export interface NetAction { status: number; rejectCode: number; version: number }
/** One replay: the moves code's bytes and its extras (empty for none). */
export interface NetReplay { moves: Uint8Array; extras: Uint8Array }
/** A row of match history, its blobs sliced out. */
export interface NetHistoryRow { id: string; createdMs: number; players: string[]; moves: Uint8Array; extras: Uint8Array }

const enc = new TextEncoder();
const NONE = new Uint8Array(0);

// Each list's rows, by the kind the answer names.
const ROW_READERS: Partial<Record<number, (m: N.Mem, p: number) => unknown>> = {
    [N.NW_GAMES]: N.readNcGameRow, [N.NW_CHATS]: N.readNcChatRow, [N.NW_BOTS]: N.readNcBotRow,
    [N.NW_BOARD]: N.readNcBoardRow, [N.NW_HISTORY]: N.readNcHistoryRow,
};

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

    /** Writes `bytes` at the inbox; their length, or -1 when they cannot be held. */
    private put(bytes: Uint8Array): number {
        const at = this.ex.wasm_net_inbox(bytes.length);
        if (!at) return -1;
        this.m().u8.set(bytes, at);   // after the inbox call: it may have grown memory
        return bytes.length;
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

    /** Why the last intent was refused at once. */
    error(): N.NcError_Snap { return N.readNcError(this.m(), this.ex.wasm_net_error()); }

    // ---- the pump ----

    /** A ticket for what the user wants, or 0: refused at once (error() says why). */
    do(i: NetIntent): number {
        N.writeNcIntent(this.m(), this.ex.wasm_net_intent_ptr(), {
            kind: i.kind, flag: i.flag ?? 0, gid: i.gid ?? '', name: i.name ?? '', password: i.password ?? '',
            bytes: [...(i.bytes ?? NONE)], intent: BigInt(i.intent ?? -1),
        });
        return this.ex.wasm_net_do(Date.now()) >>> 0;
    }

    /** The next request to send, or null for none now. */
    nextRequest(): NetRequest | null {
        const p = this.ex.wasm_net_next_request(Date.now());
        return p ? N.readNcRequest(this.m(), p) : null;
    }

    /** The answer to request `id` (status 0: it never arrived): the ticket it finished, if any. */
    onResponse(id: number, status: number, body: Uint8Array): NetAnswer | null {
        const len = Math.max(this.put(body), 0);
        const p = this.ex.wasm_net_on_response(id, status, len, Date.now());
        if (!p) return null;
        const done = N.readNcDone(this.m(), p);
        const read = ROW_READERS[done.listKind];
        const rows: unknown[] = [];
        for (let i = 0; read && i < done.nRows; i++) {
            const r = this.ex.wasm_net_row(done.listKind, len, i);
            if (r) rows.push(read(this.m(), r));
        }
        return { done, body, rows };
    }

    /** A ticket finished with no answer of its own (the session ended under it), or null. */
    nextDone(): NetDone | null {
        const p = this.ex.wasm_net_next_done();
        return p ? N.readNcDone(this.m(), p) : null;
    }

    /** What the last answer or frame changed, each with its payload. */
    events(): { event: NetEvent; payload: Uint8Array }[] {
        const out: { event: NetEvent; payload: Uint8Array }[] = [];
        for (let i = 0, p; (p = this.ex.wasm_net_event(i)); i++) {
            const event = N.readNcEvent(this.m(), p);
            const at = this.ex.wasm_net_event_payload(i);
            out.push({ event, payload: at && event.payloadLen ? this.m().u8.slice(at, at + event.payloadLen) : NONE });
        }
        return out;
    }

    /** When to call again, in ms (-1: nothing waits). */
    dueMs(): number { return this.ex.wasm_net_due_ms(Date.now()); }

    // ---- the stream ----

    streamPath(): string { return N.readNcRequest(this.m(), this.ex.wasm_net_stream_request()).path; }
    subscribe(what: number, gameId = ''): void {
        const n = this.put(enc.encode(gameId));
        if (n >= 0) this.ex.wasm_net_stream_sub(what, n);
    }
    unsubscribe(what: number, gameId = ''): void {
        const n = this.put(enc.encode(gameId));
        if (n >= 0) this.ex.wasm_net_stream_unsub(what, n);
    }
    streamOpened(): void { this.ex.wasm_net_stream_opened(Date.now()); }
    /** The socket closed: how long to wait before reopening it, in ms. */
    streamClosed(): number { return this.ex.wasm_net_stream_closed(Date.now()); }
    /** One frame as it arrived; then events() says what it changed. */
    streamIn(frame: Uint8Array | string): void {
        const n = this.put(typeof frame === 'string' ? enc.encode(frame) : frame);
        this.ex.wasm_net_stream_in(Math.max(n, 0), typeof frame === 'string' ? 1 : 0, Date.now());   // too long: a bad frame
    }
    /** The next frame to send as text, or null for none now. */
    streamNext(): string | null {
        const p = this.ex.wasm_net_stream_next(Date.now());
        return p ? N.readNcOut(this.m(), p).text : null;
    }
}

let shared: NetClient | null = null;
/** The one client of this page (or process): the module's one set of statics. */
export function netClient(): NetClient {
    return (shared ??= new NetClient(__clientKernelExports() as unknown as WebExports));
}
