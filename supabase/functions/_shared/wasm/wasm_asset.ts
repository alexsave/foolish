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

// A filesystem read that must not be STATIC — bundlers resolve `node:fs` eagerly
// and a browser build would fail on it, so it is reached only where it exists.
function readAssetSync(name: string): Uint8Array | null {
    const url = new URL(`./${name}.wasm.gz`, import.meta.url);
    // deno-lint-ignore no-explicit-any
    const g = globalThis as any;
    if (g.Deno?.readFileSync) return g.Deno.readFileSync(url);
    // Node (tests / SSR): require is absent in a browser bundle.
    const req = typeof g.require === 'function' ? g.require
        : typeof module !== 'undefined' ? eval('require') : null;
    if (!req) return null;
    try {
        return new Uint8Array(req('node:fs').readFileSync(url));
    } catch {
        return null;
    }
}

export function wasmAssetUrl(name: 'rules' | 'bots'): URL {
    return new URL(`./${name}.wasm.gz`, import.meta.url);
}

/** Synchronous load. Server only — a browser has no filesystem, so it must
 *  prefetch the bytes with loadWasmGzAsync and hand them to `seedWasmGz`. */
export function loadWasmGz(name: 'rules' | 'bots'): Uint8Array {
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
export async function loadWasmGzAsync(name: 'rules' | 'bots'): Promise<Uint8Array> {
    const have = prefetched.get(name);
    if (have) return have;
    const sync = readAssetSync(name);
    const bytes = sync
        ? gunzip(sync)
        : gunzip(new Uint8Array(await (await fetch(wasmAssetUrl(name))).arrayBuffer()));
    prefetched.set(name, bytes);
    return bytes;
}
