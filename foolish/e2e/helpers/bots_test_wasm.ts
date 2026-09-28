// The TEST build of bots.wasm: the shipped module's objects and link plus the
// exports only tests call (c/Makefile WASM_TEST_EXPORTS: the fixture sealer, the
// card-list parser, the durable roster codec). It is never committed and never
// shipped; this helper builds it on first use and whenever the wasm sources
// moved since (c/build/bots_test.stamp holds the source hash it was built from,
// scripts/wasm_stamp.sh --hash). Concurrent test processes build it once: the
// build runs under a lock directory, and a waiter re-checks the stamp after.
//
// Needs the wasm toolchain the committed module is built with: WASM_CC (on a Mac,
// /opt/homebrew/opt/llvm/bin/clang, picked automatically when present), wasm-ld,
// wasm-opt and libclang for structgen. CI installs them before the suites run.
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('../../', import.meta.url));
const WASM = `${ROOT}c/build/bots_test.wasm`;
const STAMP = `${ROOT}c/build/bots_test.stamp`;
const LOCK = `${ROOT}c/build/bots_test.lock`;
const LOCK_STALE_MS = 15 * 60_000;

function sourceHash(): string {
    return execFileSync('bash', [`${ROOT}scripts/wasm_stamp.sh`, '--hash'], { encoding: 'utf8' }).trim();
}

function current(hash: string): boolean {
    return existsSync(WASM) && existsSync(STAMP) && readFileSync(STAMP, 'utf8').trim() === hash;
}

function sleepMs(ms: number): void {
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

function wasmCc(): string {
    if (process.env.WASM_CC) return process.env.WASM_CC;
    const brew = '/opt/homebrew/opt/llvm/bin/clang';
    return existsSync(brew) ? brew : 'clang';
}

function build(hash: string): void {
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
            if (current(hash)) return;
        }
    }
    try {
        if (current(hash)) return;
        execFileSync('make', ['-C', `${ROOT}c`, `WASM_CC=${wasmCc()}`, 'wasm-bots-test'], { stdio: ['ignore', 'ignore', 'inherit'] });
        if (!current(hash)) {
            throw new Error(`make -C c wasm-bots-test ran, but ${STAMP} does not match the wasm sources (${hash}): did a source change during the build?`);
        }
    } finally {
        rmdirSync(LOCK);
    }
}

let bytes: Uint8Array | null = null;

/** The test build's bytes, built first if missing or stale. */
export function botsTestWasm(): Uint8Array {
    if (!bytes) {
        const hash = sourceHash();
        if (!current(hash)) build(hash);
        bytes = new Uint8Array(readFileSync(WASM));
    }
    return bytes;
}
