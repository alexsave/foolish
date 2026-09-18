/* =============================================================================
 * The shipped bots.wasm is the one the kernel sources build
 * =============================================================================
 * bots.wasm reaches BOTH runtimes as one tracked binary: the server reads
 * ./bots.wasm.gz off disk, the browser fetches the same file as a bundler
 * asset. One carrier, on purpose - a base64 embed would be a second copy of the
 * same bytes, and that is exactly how `rules_wasm.ts` went stale for weeks
 * while bots.wasm.gz kept being rebuilt. The tree held TWO different kernels,
 * and a C codec change was invisible to every TS test that loaded the stale one
 * - `npm run test:validate` passed 39/39 against a kernel without the change.
 * The tell is a suite that passes when it cannot possibly pass.
 *
 * So what is left to police is that the shipped artifact carries what the
 * hosts call. A missing export here is a runtime failure in the replay screen;
 * CI never rebuilds the wasm, so nothing else would catch it.
 *
 * WHY THIS READS THE WRAPPERS RATHER THAN A HAND-WRITTEN LIST. The list here
 * used to name five exports by hand, and two of them (`wasm_replay_events_n`,
 * `wasm_replay_events_next`) stopped existing when the replay reader became an
 * index the kernel writes. Nothing failed for months, because the names were
 * checked against an artifact rather than against what any host actually calls,
 * and then the test failed for the wrong reason: not "the artifact is stale"
 * but "the list is". The contract is the wrappers in sdk/ts, so the wrappers
 * are what this reads - a renamed export now moves the expectation with it, and
 * a genuinely missing one still fails.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

import { loadWasmGz } from '../sdk/ts/wasm/wasm_asset.ts';

/** Every `wasm_*` name the SDK wrappers call on a module instance. */
function calledExports(dir: string, out = new Set<string>()): Set<string> {
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
        const p = join(dir, entry.name);
        if (entry.isDirectory()) { calledExports(p, out); continue; }
        if (!entry.name.endsWith('.ts') || entry.name.endsWith('.d.ts')) continue;
        // Comments name kernel FILES (wasm_api.c) and retired entries, so they
        // are stripped before the scan: what counts is a call or a declaration.
        const src = readFileSync(p, 'utf8')
            .replace(/\/\*[\s\S]*?\*\//g, '')
            .replace(/^[ \t]*\/\/.*$/gm, '');
        // `ex.wasm_foo(`, `instance.exports.wasm_foo(`, and the interface
        // declarations the wrappers type their exports with: all are a name
        // immediately followed by its argument list.
        for (const m of src.matchAll(/\bwasm_[a-z0-9_]+(?=\s*[(:])/g)) out.add(m[0]);
    }
    return out;
}

test('the shipped bots.wasm exports what the web calls', () => {
    const mod = new WebAssembly.Module(loadWasmGz('bots') as BufferSource);
    const names = new Set(WebAssembly.Module.exports(mod).map(e => e.name));

    const called = calledExports(new URL('../sdk/ts', import.meta.url).pathname);
    // The generated modules and the test build live elsewhere; the SDK is the
    // shipped surface, so anything it names must be in the shipped artifact.
    assert.ok(called.size > 20, `expected the SDK wrappers to name many exports, saw ${called.size}`);

    const missing = [...called].filter((n) => !names.has(n)).sort();
    assert.deepEqual(missing, [],
        `bots.wasm is missing ${missing.join(', ')} - rebuild: cd c && make wasm-bots WASM_CC=/opt/homebrew/opt/llvm/bin/clang`);
});
