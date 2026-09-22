// Loads a kernel module's raw wasm bytes.
//
// TWO sources, chosen by what the runtime can actually do:
//
//   * a gzip STATIC ASSET next to this file, on the server. Deployed with
//     `supabase functions deploy --use-api` (no Docker) via config.toml
//     static_files: the .wasm.gz ships as a real binary and is never
//     base64-expanded. Read synchronously (Deno edge: Deno.readFileSync; Node
//     tests: node:fs).
//   * a .gz FETCHED over the network, in the browser, which has no filesystem.
//     `new URL('./<name>.wasm.gz', import.meta.url)` makes the bundler emit it
//     as a real asset, so there is one file, tracked once, in its natural
//     binary form. Deliberately NOT a base64 embed like rules/guards: 78 KB of
//     single-line base64 rewritten on every kernel build is a miserable thing
//     to keep in git, and a second carrier of the same bytes is exactly how
//     rules_wasm.ts went stale.
//
// And TWO LINKS of one kernel, which is a different axis and is kernelModule()
// below. The browser ran the whole thing for a while - one big module
// everywhere, with the split left to A10 - and paid 82 KB for a download that
// never called the C Table or a bot. It fetches web.wasm.gz now: the same
// objects, the same layout hash, a smaller export allow-list, 31 KB.
import { gunzip } from './gunzip.ts';

// Reading the asset without a STATIC `node:fs` import, which a browser bundle
// would choke on. Three runtimes, three doors:
//
//   Deno edge      -> Deno.readFileSync / Deno.readFile
//   Node           -> require (tsx/CJS) or a dynamic node:fs (ESM)
//   browser        -> neither; it fetches (loadWasmGzAsync)
//
// The ESM door is not optional. Next's server bundle is ESM: `require` is
// undefined there, so the sync probe returns null, and falling through to
// fetch() would try `fetch('file:///…')` — which Node does not support and which
// fails at runtime while every tsx-run test stays green, because tsx IS CJS.
// That is exactly how /m/'s unfurl broke in production once already.
function readAssetSync(name: WasmModuleName): Uint8Array | null {
    const url = wasmAssetUrl(name);
    // deno-lint-ignore no-explicit-any
    const g = globalThis as any;
    if (g.Deno?.readFileSync) return g.Deno.readFileSync(url);
    const req = typeof g.require === 'function' ? g.require
        : typeof module !== 'undefined' ? eval('require') : null;
    if (!req) return null;                       // ESM: use readAssetAsync
    try {
        return new Uint8Array(req('node:fs').readFileSync(url));
    } catch {
        return null;
    }
}

async function readAssetAsync(name: WasmModuleName): Promise<Uint8Array | null> {
    // deno-lint-ignore no-explicit-any
    const g = globalThis as any;
    // Deno FIRST, and async: routing through readAssetSync here would run
    // Deno.readFileSync inside an async callback, which Deno warns against and
    // will disallow. Node's CJS/require path stays sync below (Deno is handled).
    if (g.Deno?.readFile) return await g.Deno.readFile(wasmAssetUrl(name));
    const sync = readAssetSync(name);
    if (sync) return sync;
    if (typeof process !== 'undefined' && process.versions?.node) {
        // Computed specifier + webpackIgnore: the bundler must not resolve this
        // for the client build, where node:fs does not exist.
        const spec = 'node:fs/promises';
        const fs = await import(/* webpackIgnore: true */ spec);
        return new Uint8Array(await fs.readFile(wasmAssetUrl(name)));
    }
    return null;                                 // browser
}

export type WasmModuleName = 'rules' | 'bots' | 'web';

export function wasmAssetUrl(name: WasmModuleName): URL {
    return new URL(`./${name}.wasm.gz`, import.meta.url);
}

/**
 * WHICH LINK OF THE ONE KERNEL THIS RUNTIME LOADS.
 *
 * Not two kernels: `bots.wasm` and `web.wasm` are the same object files under
 * two `-Wl,--export=` allow-lists (c/Makefile, WASM_WEB_NAMES), so they carry
 * one set of rules, one layout hash, and identical bytes for every function
 * they share. What differs is what an export keeps alive - the browser's link
 * sheds the Monte-Carlo brains and the whole C Table it never calls, which is
 * 82 KB of download against 31 KB.
 *
 * THE TEST IS THE FILESYSTEM, not a browser sniff, and that is not a
 * coincidence: the runtimes that read the module off disk (the Deno edge
 * functions, Node's suites and tools) are exactly the ones that play the table
 * and run the bot loop, and the runtime that has to fetch it is exactly the one
 * that does neither. Asking the same question the loader above already asks
 * keeps the two answers from drifting apart.
 *
 * `process.versions?.node` IS SAFE IN THE BROWSER, and it was worth checking
 * rather than assuming, because Next bundles a `process` shim for client code
 * and a truthy answer here would silently hand every visitor the server's
 * module again - 82 KB instead of 31, with nothing broken to notice. Read out
 * of a real `next build` (Next 16.2.6, Turbopack): the client shim is used only
 * when there is no real global `process`, and it sets `versions = {}`, so the
 * `?.node` is undefined and the browser answers 'web'. The same build emits
 * both modules to .next/static/media and the chunk that asks resolves each by
 * name, so the fetch that follows is for the one this returns.
 *
 * The Deno edge branch is first for a reason beyond order: config.toml stages
 * bots.wasm.gz and nothing else, so an edge answer of 'web' is a 500 on the
 * first request after a deploy. e2e/validation/deploy_paths_validation.test.ts
 * fakes a Deno global and pins it.
 */
export function kernelModule(): WasmModuleName {
    // deno-lint-ignore no-explicit-any
    const g = globalThis as any;
    if (g.Deno?.readFileSync) return 'bots';
    if (typeof process !== 'undefined' && process.versions?.node) return 'bots';
    return 'web';
}

/** Synchronous load. Server only — a browser has no filesystem, so it must
 *  prefetch the bytes with loadWasmGzAsync and hand them to `seedWasmGz`. */
export function loadWasmGz(name: WasmModuleName): Uint8Array {
    const seeded = prefetched.get(name);
    if (seeded) return seeded;
    const gz = readAssetSync(name);
    if (!gz) {
        throw new Error(
            `${name}.wasm is not available synchronously here (no filesystem). ` +
            `Await loadWasmGzAsync('${name}') first — see ensureBotsAsync.`);
    }
    return gunzip(gz);
}

const prefetched = new Map<string, Uint8Array>();

/** Fetch + inflate the module, for runtimes with no filesystem (the browser).
 *  Caches the inflated bytes so the sync loadWasmGz above can serve them. */
export async function loadWasmGzAsync(name: WasmModuleName): Promise<Uint8Array> {
    const have = prefetched.get(name);
    if (have) return have;
    const local = await readAssetAsync(name);
    const bytes = local
        ? gunzip(local)
        : gunzip(new Uint8Array(await (await fetch(wasmAssetUrl(name))).arrayBuffer()));
    prefetched.set(name, bytes);
    return bytes;
}
