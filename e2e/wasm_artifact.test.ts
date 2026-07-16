/* =============================================================================
 * The shipped bots.wasm is the one the kernel sources build
 * =============================================================================
 * bots.wasm reaches BOTH runtimes as one tracked binary: the server reads
 * ./bots.wasm.gz off disk, the browser fetches the same file as a bundler
 * asset. One carrier, on purpose — a base64 embed would be a second copy of the
 * same bytes, and that is exactly how `rules_wasm.ts` went stale for weeks
 * while bots.wasm.gz kept being rebuilt. The tree held TWO different kernels,
 * and a C codec change was invisible to every TS test that loaded the stale one
 * — `npm run test:validate` passed 39/39 against a kernel without the change.
 * The tell is a suite that passes when it cannot possibly pass.
 *
 * So what is left to police is that the shipped artifact carries what the
 * client calls. A missing export here is a runtime failure in the replay
 * screen; CI never rebuilds the wasm, so nothing else would catch it.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadWasmGz } from '../supabase/functions/_shared/sdk/ts/wasm/wasm_asset.ts';

test('the shipped bots.wasm exports what the web calls', () => {
    // A5: the browser replays a shared code through the real engine. If the
    // committed artifact predates that work these exports are simply absent, and
    // the replay screen fails at runtime rather than here.
    const mod = new WebAssembly.Module(loadWasmGz('bots') as BufferSource);
    const names = new Set(WebAssembly.Module.exports(mod).map(e => e.name));
    for (const want of [
        'wasm_replay_events', 'wasm_replay_events_n', 'wasm_replay_events_next',
        'wasm_replay_step_count', 'wasm_replay_encode_v6_from_game',
    ]) {
        assert.ok(names.has(want), `bots.wasm is missing ${want} — rebuild: cd cnitro && make wasm-bots WASM_CC=/opt/homebrew/opt/llvm/bin/clang`);
    }
});
