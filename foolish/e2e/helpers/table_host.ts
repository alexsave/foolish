// table_host.ts - the native table host (c/tests/table_host.c), the kernel's
// server half for the Node suites.
//
// A test that holds shipped TypeScript (the page in jsdom, the web's move gates,
// its replay screen) needs what a server makes: a sealed board, the envelope a
// viewer reads, a bot game played to its code. The server is c_base, which links
// the kernel natively, so the suites take that half from the same native kernel:
// this file starts c/build/table_host and calls the wasm entry files' functions
// in it, by name, over two FIFOs. Every entry behaves as it does in web.wasm -
// the same sources, the same caps - and answers synchronously, so the helpers
// over it (server_table.ts, table_fixture.ts, bot_table.ts) keep their shape.
//
// WHAT CROSSES. Numbers, and bytes at the addresses the entries hand out: an
// entry that answers a pointer answers the host's native address, and read() /
// write() move bytes there. Structs are read and written through the generated
// wasm32 accessors over a local copy (mem()), which is sound because the native
// layout IS the wasm32 layout - checked on every start against the hash the
// accessors were generated with (layoutHash below), never assumed.
//
// ONE PROCESS PER HOST. startTableHost() is a fresh kernel with no history, the
// way a fresh wasm instance was; tests that want two independent kernels start
// two. The host exits when this process does (its request pipe closes).

import { execFileSync, spawn } from 'node:child_process';
import { closeSync, existsSync, mkdirSync, mkdtempSync, openSync, readSync, rmdirSync, rmSync, statSync, writeSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { TableHostExports } from '../../sdk/ts/gen/exports.table_host.ts';
import { LAYOUT_HASH } from '../../sdk/ts/gen/layout_hash.bots.ts';
import { assertLayoutHash } from '../../sdk/ts/wasm/layout_hash.ts';

const ROOT = fileURLToPath(new URL('../../', import.meta.url));
const LOCK = `${ROOT}c/build/table_host.lock`;
const LOCK_STALE_MS = 15 * 60_000;

/** The host's entries, called by name. No `memory`: the host's memory is reached through read() and write(). */
export type HostEntries = Omit<TableHostExports, 'memory'>;

function sleepMs(ms: number): void {
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

/**
 * The host binary, built first. scripts/run_e2e.mjs builds it once for a whole
 * run and names it in TABLE_HOST; a file run on its own builds it here, with
 * make deciding what is stale, under a lock directory so parallel test
 * processes do not run two makes in one tree (c/Makefile's layout hash writes
 * fixed temp paths).
 */
let built: string | null = null;
function hostBinary(): string {
    if (built) return built;
    if (process.env.TABLE_HOST) return (built = process.env.TABLE_HOST);
    mkdirSync(`${ROOT}c/build`, { recursive: true });
    for (;;) {
        try {
            mkdirSync(LOCK);
            break;
        } catch {
            let age = 0;
            try { age = Date.now() - statSync(LOCK).mtimeMs; } catch { continue; }   // released meanwhile
            if (age > LOCK_STALE_MS) { try { rmdirSync(LOCK); } catch { /* another waiter took it */ } continue; }
            sleepMs(250);
        }
    }
    try {
        execFileSync('make', ['-s', '-C', `${ROOT}c`, 'table-host'], { stdio: ['ignore', 'ignore', 'inherit'] });
    } finally {
        rmdirSync(LOCK);
    }
    return (built = `${ROOT}c/build/table_host`);
}

export class TableHost {
    /** Every entry the host offers, called by name with numbers. */
    readonly ex: HostEntries;
    private readonly wfd: number;
    private readonly rfd: number;
    private readonly scratch = new Uint8Array(65536);

    constructor(wfd: number, rfd: number) {
        this.wfd = wfd;
        this.rfd = rfd;
        this.ex = new Proxy({} as HostEntries, {
            get: (_, name) => (typeof name === 'string' ? (...args: number[]) => this.call(name, args) : undefined),
        });
    }

    private send(frame: Uint8Array): Uint8Array<ArrayBuffer> {
        for (let at = 0; at < frame.length;) at += writeSync(this.wfd, frame, at, frame.length - at);
        // One answer is in flight at a time, so whatever a read returns belongs
        // to it: read into the scratch buffer, and only a long answer reads again.
        let got = this.more(this.scratch, 0, 4);
        const n = new DataView(this.scratch.buffer).getUint32(0, true);
        const body = new Uint8Array(n);
        body.set(this.scratch.subarray(4, Math.min(got, 4 + n)), 0);
        for (let at = got - 4; at < n;) at = this.more(body, at, n);
        if (body[0] !== 0) throw new Error(new TextDecoder().decode(body.subarray(1)));
        return body.subarray(1);
    }

    /** Reads into `buf` from `at` until at least `want` bytes are there; how many are. */
    private more(buf: Uint8Array, at: number, want: number): number {
        while (at < want) {
            const n = readSync(this.rfd, buf, at, buf.length - at, null);
            if (n === 0) throw new Error('table host: the host process is gone (it crashed, or was killed)');
            at += n;
        }
        return at;
    }

    /** Frames one request: its length, its op and the body `fill` writes. */
    private frame(op: string, len: number, fill: (dv: DataView, u8: Uint8Array) => void): Uint8Array {
        const u8 = new Uint8Array(5 + len);
        const dv = new DataView(u8.buffer);
        dv.setUint32(0, 1 + len, true);
        u8[4] = op.charCodeAt(0);
        fill(new DataView(u8.buffer, 5), u8.subarray(5));
        return u8;
    }

    private call(name: string, args: number[]): number {
        const id = new TextEncoder().encode(name);
        const out = this.send(this.frame('C', 2 + id.length + 8 * args.length, (dv, u8) => {
            u8[0] = id.length;
            u8.set(id, 1);
            u8[1 + id.length] = args.length;
            args.forEach((a, i) => dv.setFloat64(2 + id.length + 8 * i, a, true));
        }));
        return new DataView(out.buffer, out.byteOffset).getFloat64(0, true);
    }

    /** `n` bytes at host address `ptr`, copied. */
    read(ptr: number, n: number): Uint8Array<ArrayBuffer> {
        return this.send(this.frame('R', 12, (dv) => {
            dv.setFloat64(0, ptr, true);
            dv.setUint32(8, n, true);
        })).slice();
    }

    /** `bytes` written at host address `ptr`. */
    write(ptr: number, bytes: Uint8Array): void {
        this.send(this.frame('W', 8 + bytes.length, (dv, u8) => {
            dv.setFloat64(0, ptr, true);
            u8.set(bytes, 8);
        }));
    }
}

// An empty environment: the kernel reads getenv for bot knobs, and wasm's
// getenv saw nothing a test did not set.
const NO_ENV = {} as NodeJS.ProcessEnv;

/** A fresh host process: a kernel of its own, with no history. */
export function startTableHost(): TableHost {
    const bin = hostBinary();
    if (!existsSync(bin)) throw new Error(`table host: ${bin} does not exist (make -C c table-host)`);
    // Run once to completion first: a binary that cannot start, or one built
    // for another layout, fails HERE rather than leaving this process blocked
    // opening a FIFO nobody will open.
    const hash = Number(execFileSync(bin, ['--layout-hash'], { encoding: 'utf8', env: NO_ENV }).trim());
    assertLayoutHash('table_host', { wasm_layout_hash: () => hash }, LAYOUT_HASH, 'sdk/ts/gen/layout_hash.bots.ts');

    const dir = mkdtempSync(join(tmpdir(), 'table-host-'));
    const req = join(dir, 'req');
    const res = join(dir, 'res');
    execFileSync('mkfifo', [req, res]);
    const child = spawn(bin, [req, res], { stdio: ['ignore', 'ignore', 'inherit'], env: NO_ENV });
    child.unref();
    // The order the host opens them in (request, then response), or neither open returns.
    const wfd = openSync(req, 'w');
    const rfd = openSync(res, 'r');
    rmSync(dir, { recursive: true, force: true });   // the open ends keep the pipes
    process.once('exit', () => { try { closeSync(wfd); closeSync(rfd); } catch { /* already closed */ } });
    const host = new TableHost(wfd, rfd);
    host.ex.wasm_init();
    return host;
}
