// Loads a kernel module's raw wasm bytes.
//
// TWO sources, one module, chosen by what the runtime can actually do:
//
//   * a gzip STATIC ASSET next to this file, on the server. Deployed with
//     `supabase functions deploy --use-api` (no Docker) via config.toml
//     static_files: the .wasm.gz ships as a real binary (bots 57KB vs a 78KB
//     base64 embed) and is never base64-expanded. Read synchronously (Deno
//     edge: Deno.readFileSync; Node tests: node:fs).
//   * the SAME .gz FETCHED over the network, in the browser, which has no
//     filesystem. `new URL('./bots.wasm.gz', import.meta.url)` makes the
//     bundler emit it as a real asset, so there is one file, tracked once, in
//     its natural binary form. Deliberately NOT a base64 embed like
//     rules/guards: 78 KB of single-line base64 rewritten on every kernel
//     rebuild is a miserable thing to keep in git, and a second carrier of the
//     same bytes is exactly how rules_wasm.ts went stale.
//
// The browser path exists because the client runs the WHOLE kernel now, not a
// rules subset: replaying a shared code rebuilds the game and plays it through
// the real engine (A5), and replay_steps.c cannot live in rules.wasm — its
// decoded-action buffer alone is ~272 KB against rules.wasm's linear memory,
// PINNED at 196,608 B. One big module everywhere; split later (A10).
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

export type WasmModuleName = 'rules' | 'bots';

export function wasmAssetUrl(name: WasmModuleName): URL {
    return new URL(`./${name}.wasm.gz`, import.meta.url);
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
