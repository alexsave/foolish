// Loads a kernel module's raw wasm bytes from a gzip STATIC ASSET bundled next
// to this file. Deployed with `supabase functions deploy --use-api` (no Docker)
// via config.toml static_files: the .wasm.gz ships as a real binary (bots 47KB
// vs a 155KB base64 embed) and is never base64-expanded. Read synchronously
// (Deno edge: Deno.readFileSync; Node tests: node:fs) and inflated with
// node:zlib gunzipSync (works on both — verified). Server-only; the browser
// client loads guards.wasm from its own base64 embed, not from here.
import { gunzipSync } from 'node:zlib';
import { readFileSync } from 'node:fs';

export function loadWasmGz(name: 'rules' | 'bots'): Uint8Array {
    const url = new URL(`./${name}.wasm.gz`, import.meta.url);
    // deno-lint-ignore no-explicit-any
    const D = (globalThis as any).Deno;
    const gz: Uint8Array = D?.readFileSync ? D.readFileSync(url) : new Uint8Array(readFileSync(url));
    return new Uint8Array(gunzipSync(gz));
}
