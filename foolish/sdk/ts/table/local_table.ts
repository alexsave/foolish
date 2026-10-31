// local_table.ts - the browser's local table (c/src/local_table.h), over web.wasm.
//
// A call-and-copy wrapper like client_table.ts: each method writes its input
// into the IO buffer, has the kernel run one operation, and copies the answer
// out before it returns. Every byte it moves is opaque here - a game id, a seed,
// a name, an action request or an ask in; an envelope, a response body or a cast
// out - and every one that comes out is the human seat's, which the page reads
// through the client slot exactly as it reads a server's.

import type { WebExports } from '../gen/exports.web.ts';
import { __clientKernelExports } from '../wasm/bots.ts';

const enc = new TextEncoder();

export class LocalTable {
    private readonly ex: WebExports;

    constructor(ex: WebExports) { this.ex = ex; }

    private put(...parts: Uint8Array[]): number {
        const total = parts.reduce((n, p) => n + p.length, 0);
        if (total > this.ex.wasm_io_cap()) throw new Error('local table: input too long');
        const u8 = new Uint8Array(this.ex.memory.buffer);
        let at = this.ex.wasm_io_ptr();
        for (const p of parts) { u8.set(p, at); at += p.length; }
        return total;
    }

    private out(n: number, what: string): Uint8Array {
        if (n < 0) throw new Error(`local table: ${what} refused (${n})`);
        const at = this.ex.wasm_io_ptr();
        return new Uint8Array(this.ex.memory.buffer).slice(at, at + n);
    }

    /** A new table: `seed` the deal's (32 bytes from crypto), `you` the human's name. The human's envelope. */
    open(gameId: string, seed: Uint8Array, you: string): Uint8Array {
        const gid = enc.encode(gameId), name = enc.encode(you);
        this.put(gid, seed, name);
        return this.out(this.ex.wasm_local_open(gid.length, name.length, Date.now()), 'open');
    }

    /** The human's envelope of the table as it stands. */
    view(): Uint8Array { return this.out(this.ex.wasm_local_view(), 'view'); }

    /** The human readies and the table deals: the kernel's TABLE_* result. */
    start(): number { return this.ex.wasm_local_start(Date.now()); }

    /** An action request body (ClientTable.actionRequest) in, the response body out. */
    action(request: Uint8Array): Uint8Array {
        return this.out(this.ex.wasm_local_action(this.put(request), Date.now()), 'action');
    }

    /** An ask body (ClientTable.askBody): the kernel's TABLE_* result, negative for a refusal. */
    ask(body: Uint8Array): number { return this.ex.wasm_local_ask(this.put(body), Date.now()); }

    /** Milliseconds until the bot acts, -1 when it has nothing to do. */
    botWaitMs(): number { return this.ex.wasm_local_bot_wait(Date.now()); }

    /** One bot cycle: the actions it applied. */
    bot(): number { return this.ex.wasm_local_bot(Date.now()); }

    /** The cast the last committed operation left for the human, once; null for none. */
    cast(): Uint8Array | null {
        const n = this.ex.wasm_local_cast();
        return n > 0 ? this.out(n, 'cast') : null;
    }
}

let shared: LocalTable | null = null;

/** The page's one local table, on the page's kernel (warm it first: ensureBotsAsync). */
export function localTable(): LocalTable {
    return (shared ??= new LocalTable(__clientKernelExports() as unknown as WebExports));
}
