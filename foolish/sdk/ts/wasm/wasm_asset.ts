// Loads the kernel module's raw wasm bytes: web.wasm, the browser's link of the
// kernel (c/Makefile, WASM_WEB_NAMES).
//
// TWO sources, chosen by what the runtime can actually do:
//
//   * a .gz FETCHED over the network, in the browser, which has no filesystem.
//     `new URL('./web.wasm.gz', import.meta.url)` makes the bundler emit it as
//     a real asset, so there is one file, built once, in its natural binary
//     form. Deliberately NOT a base64 embed: a second carrier of the same bytes
//     is exactly how rules_wasm.ts went stale.
//   * the same .gz read synchronously from disk, in Node (the e2e suites and
//     tools), so a test runs the very bytes a visitor downloads.
//
// There is ONE module here and no choice between links. There used to be two:
// bots.wasm, the whole kernel, which the Supabase edge functions read off disk,
// and this one. The edge functions are gone (the server is c_base, which links
// the kernel natively), and the whole-kernel link the Node suites still need
// for the C Table and the bots is a test build they load themselves
// (e2e/helpers/bots_test_wasm.ts).
import { gunzip } from './gunzip.ts';

// The bundler resolves this literal to the emitted asset; it must stay a
// literal, because a template here would make the bundler emit every
// `*.wasm.gz` the pattern could match.
function wasmAssetUrl(): URL {
    return new URL('./web.wasm.gz', import.meta.url);
}

// Reading the asset without a STATIC `node:fs` import, which a browser bundle
// would choke on. Two runtimes, two doors:
//
//   Node           -> require (tsx/CJS) or a dynamic node:fs (ESM)
//   browser        -> neither; it fetches (loadWasmGzAsync)
//
// The ESM door is not optional. Next's server bundle is ESM: `require` is
// undefined there, so the sync probe returns null, and falling through to
// fetch() would try `fetch('file:///…')` — which Node does not support and which
// fails at runtime while every tsx-run test stays green, because tsx IS CJS.
// That is exactly how /m/'s unfurl broke in production once already.
function readAssetSync(): Uint8Array | null {
    // deno-lint-ignore no-explicit-any
    const g = globalThis as any;
    const req = typeof g.require === 'function' ? g.require
        : typeof module !== 'undefined' ? eval('require') : null;
    if (!req) return null;                       // ESM: use readAssetAsync
    try {
        return new Uint8Array(req('node:fs').readFileSync(wasmAssetUrl()));
    } catch {
        return null;
    }
}

async function readAssetAsync(): Promise<Uint8Array | null> {
    const sync = readAssetSync();
    if (sync) return sync;
    if (typeof process !== 'undefined' && process.versions?.node) {
        // Computed specifier + webpackIgnore: the bundler must not resolve this
        // for the client build, where node:fs does not exist.
        const spec = 'node:fs/promises';
        const fs = await import(/* webpackIgnore: true */ spec);
        return new Uint8Array(await fs.readFile(wasmAssetUrl()));
    }
    return null;                                 // browser
}

let prefetched: Uint8Array | null = null;

/** Synchronous load. Only where the bytes can be read synchronously (Node) or
 *  were already fetched: a browser must await loadWasmGzAsync first. */
export function loadWasmGz(): Uint8Array {
    if (prefetched) return prefetched;
    const gz = readAssetSync();
    if (!gz) {
        throw new Error(
            'web.wasm is not available synchronously here (no filesystem). ' +
            'Await loadWasmGzAsync() first — see ensureBotsAsync.');
    }
    return gunzip(gz);
}

/** Fetch + inflate the module, for runtimes with no filesystem (the browser).
 *  Caches the inflated bytes so the sync loadWasmGz above can serve them. */
export async function loadWasmGzAsync(): Promise<Uint8Array> {
    if (prefetched) return prefetched;
    const local = await readAssetAsync();
    const bytes = local
        ? gunzip(local)
        : gunzip(new Uint8Array(await (await fetch(wasmAssetUrl())).arrayBuffer()));
    prefetched = bytes;
    return bytes;
}
