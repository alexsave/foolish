/* =============================================================================
 * web.wasm is the browser's LINK of the kernel, not a second kernel.
 * =============================================================================
 *
 * c/Makefile builds two modules out of one set of object files. `bots.wasm` is
 * the server-side link - the edge functions and Node read it off disk, and the
 * e2e suites drive it through every entry a binder declares. `web.wasm` is the
 * same objects under a smaller `-Wl,--export=` allow-list, and the browser
 * fetches that one: 31 KB gz against 82 KB, because an export is a GC root and
 * the names the browser never calls were keeping the four Monte-Carlo brains,
 * cordite's solver working set and the whole C Table alive in a module no bot
 * ever ran in.
 *
 * THIS FILE IS THE REASON THAT IS SAFE, and it is written against a specific
 * history. docs/ARCHITECTURE_AS_A_PATTERN.md Part 1 piece 1 records that
 * guards.wasm (a validate-only client build) and rules.wasm (a server-only
 * build) were deleted because they were "three kernels answering the same
 * questions". They were three SOURCES. Two links of one object set are not that
 * - but "are not that" is a claim, and a claim with no gate has a half-life, so
 * the claim is checked here in four ways:
 *
 *   1. SUBSET. Every name web.wasm exports, bots.wasm exports too, with the
 *      same wasm signature. There is no entry point that exists only for the
 *      browser, which is where a second meaning would start.
 *   2. ONE LAYOUT. Both modules answer wasm_layout_hash() with the generated
 *      constant. A layout hash is over field path, offset, size, kind and bit
 *      range, so two modules agreeing on it agree about every struct that
 *      crosses the boundary.
 *   3. ONE ANSWER. The two modules are handed the same replay code and asked
 *      every question the browser asks of it - the summary, the step count, each
 *      step's masked board for each viewer, each step's public log - and every
 *      byte must match. That is the engine, the v6 decoder, the step walker and
 *      view.c's masking, driven end to end through both links.
 *   4. THE REACH SET. The export list is not a guess: esbuild bundles every
 *      browser entry with tree shaking on, and the wasm_* names that survive
 *      must be exactly the names web.wasm exports. A new browser call site that
 *      the module does not carry fails HERE rather than as a TypeError in a tab,
 *      and a name that stops being reachable has to leave the list.
 *
 * And one thing the browser's link makes true that the blob could not: the
 * unmasked-state entry points e2e/security_client_boundary.test.ts denies are
 * not merely unreferenced by the bundle, they are ABSENT FROM THE MODULE. A
 * reader that is not in the binary is one nobody can reach by any route.
 *
 * Pure test - no Postgres, no network, no compiler. It reads the two built .gz
 * files, so it runs in any lane that has built the `bots` group.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';

import { LAYOUT_HASH } from '../sdk/ts/gen/layout_hash.bots.ts';
import { TUTORIAL_MOVES_CODE } from '../src/components/tutorialGame.ts';
import { CLIENT_BOUNDARY, browserWasmReach } from './helpers/client_bundle.ts';

const ROOT = fileURLToPath(new URL('../', import.meta.url));

/** The names c/Makefile's WASM_WEB_NAMES was written from - the two links' own tables. */
function exportNames(rel: string): Set<string> {
    const w = gunzipSync(readFileSync(ROOT + rel));
    const out = new Set<string>();
    let p = 8;
    const vu = () => { let r = 0, s = 0, b: number; do { b = w[p++]; r += (b & 0x7f) * 2 ** s; s += 7; } while (b & 0x80); return r; };
    while (p < w.length) {
        const id = w[p++], size = vu(), start = p;
        if (id === 7) {
            const n = vu();
            for (let i = 0; i < n; i++) {
                const len = vu();
                const nm = Buffer.from(w.slice(p, p + len)).toString('utf8');
                p += len;
                const kind = w[p++];
                vu();
                if (kind === 0) out.add(nm);
            }
        }
        p = start + size;
    }
    return out;
}

/** A module's export table as {name -> "params->results"}, so a shared name cannot change shape. */
function signatures(rel: string): Map<string, string> {
    const w = gunzipSync(readFileSync(ROOT + rel));
    const types: string[] = [], ftypes: number[] = [], out = new Map<string, string>();
    let p = 8, nimp = 0;
    const vu = () => { let r = 0, s = 0, b: number; do { b = w[p++]; r += (b & 0x7f) * 2 ** s; s += 7; } while (b & 0x80); return r; };
    const name = () => { const n = vu(); const s = Buffer.from(w.slice(p, p + n)).toString('utf8'); p += n; return s; };
    const exportsAt: [string, number][] = [];
    while (p < w.length) {
        const id = w[p++], size = vu(), start = p;
        if (id === 1) {
            const n = vu();
            for (let i = 0; i < n; i++) {
                p++;                                     // 0x60, the func form
                const np = vu(); const ps: number[] = [];
                for (let j = 0; j < np; j++) ps.push(w[p++]);
                const nr = vu(); const rs: number[] = [];
                for (let j = 0; j < nr; j++) rs.push(w[p++]);
                types.push(`${ps.join(',')}->${rs.join(',')}`);
            }
        } else if (id === 2) {
            const n = vu();
            for (let i = 0; i < n; i++) {
                name(); name();
                const k = w[p++];
                if (k === 0) { vu(); nimp++; }
                else if (k === 1) { p++; const f = w[p++]; vu(); if (f & 1) vu(); }
                else if (k === 2) { const f = w[p++]; vu(); if (f & 1) vu(); }
                else { p += 2; }
            }
        } else if (id === 3) {
            const n = vu();
            for (let i = 0; i < n; i++) ftypes.push(vu());
        } else if (id === 7) {
            const n = vu();
            for (let i = 0; i < n; i++) { const nm = name(); const k = w[p++]; const idx = vu(); if (k === 0) exportsAt.push([nm, idx]); }
        }
        p = start + size;
    }
    for (const [nm, idx] of exportsAt) out.set(nm, types[ftypes[idx - nimp]] ?? '?');
    return out;
}

// A raw instance of one link: no bridge, no wrapper, so the comparison below is
// of the MODULES and not of the TypeScript that usually stands in front of them.
interface RawKernel {
    memory: WebAssembly.Memory;
    wasm_init(): void;
    wasm_layout_hash(): number;
    wasm_io_ptr(): number;
    wasm_replay_io_ptr(): number;
    wasm_replay_io_cap(): number;
    wasm_replay_b32_decode(len: number): number;
    wasm_replay_summary(len: number): number;
    wasm_replay_step_count(len: number): number;
    wasm_replay_step_index(len: number): number;
    wasm_replay_step_masked_state(len: number, step: number, viewer: number): number;
    wasm_replay_step_logs(len: number, step: number): number;
}

function instantiate(rel: string): RawKernel {
    const bytes = gunzipSync(readFileSync(ROOT + rel));
    const m = new WebAssembly.Module(bytes as BufferSource);
    const ex = new WebAssembly.Instance(m, {}).exports as unknown as RawKernel;
    ex.wasm_init();
    return ex;
}

const bytesOf = (ex: RawKernel) => new Uint8Array(ex.memory.buffer);

test('web.wasm exports nothing bots.wasm does not, and nothing with a different shape', () => {
    const web = signatures('sdk/ts/wasm/web.wasm.gz');
    const bots = signatures('sdk/ts/wasm/bots.wasm.gz');
    assert.ok(web.size > 40, `web.wasm exports ${web.size} functions - that is not a kernel`);

    const extra = [...web.keys()].filter((n) => !bots.has(n)).sort();
    assert.deepEqual(extra, [],
        'web.wasm exports entry points bots.wasm does not have. The two links come out of ONE\n'
        + 'object set and one export list (c/Makefile, WASM_WEB_NAMES picked out of\n'
        + 'WASM_BOTS_SHIPPED_EXPORTS), so a browser-only entry point means the two surfaces have\n'
        + 'started to diverge - which is how guards.wasm and rules.wasm became three kernels.');

    const reshaped = [...web.entries()]
        .filter(([n, sig]) => bots.get(n) !== sig)
        .map(([n, sig]) => `${n}: web ${sig} vs bots ${bots.get(n)}`);
    assert.deepEqual(reshaped, [], 'a shared export changed arity or type between the two links');
});

test('both links of the kernel carry the generated layout hash', () => {
    for (const rel of ['sdk/ts/wasm/bots.wasm.gz', 'sdk/ts/wasm/web.wasm.gz']) {
        const ex = instantiate(rel);
        assert.equal(ex.wasm_layout_hash() >>> 0, LAYOUT_HASH >>> 0,
            `${rel} was built from a different tree than sdk/ts/gen/layout_hash.bots.ts.\n`
            + 'The two links share one compile and therefore one layout hash; a difference here is\n'
            + 'the one way a per-call-site link could become a second kernel. Rebuild the group:\n'
            + '  scripts/wasm_build.sh bots');
    }
});

test('both links answer the same replay code identically, step by step', () => {
    const bots = instantiate('sdk/ts/wasm/bots.wasm.gz');
    const web = instantiate('sdk/ts/wasm/web.wasm.gz');

    // The tutorial's real 3-player game (src/components/tutorialGame.ts), cut by
    // the engine. It exercises the lot: lead, cover, trump cover, throw-in,
    // perevod, pickup, round end, refills, the deck running out and a seat
    // going out.
    const code = new TextEncoder().encode(TUTORIAL_MOVES_CODE);

    const decode = (ex: RawKernel): Uint8Array => {
        bytesOf(ex).set(code, ex.wasm_replay_io_ptr());
        const w = ex.wasm_replay_b32_decode(code.length);
        assert.ok(w > 0, 'the tutorial code did not decode - it may need re-cutting (tests/gen_tutorial_game.ts)');
        const at = ex.wasm_io_ptr();
        return bytesOf(ex).slice(at, at + w);
    };
    const moves = decode(bots);
    assert.deepEqual([...decode(web)], [...moves], 'the two links disagree about base32');

    const put = (ex: RawKernel) => bytesOf(ex).set(moves, ex.wasm_replay_io_ptr());

    // The summary: seats, trump, opener, the fool and the order the rest went out.
    put(bots); const sa = bots.wasm_replay_summary(moves.length);
    put(web); const sb = web.wasm_replay_summary(moves.length);
    assert.ok(sa > 0 && sb > 0, 'the summary did not decode in one of the links');
    // Same struct, so the same 64 bytes at the same offset either side.
    assert.deepEqual([...bytesOf(bots).slice(sa, sa + 64)], [...bytesOf(web).slice(sb, sb + 64)],
        'the two links read a different summary out of one code');

    put(bots); const steps = bots.wasm_replay_step_count(moves.length);
    put(web); assert.equal(web.wasm_replay_step_count(moves.length), steps, 'different step counts');
    assert.ok(steps > 20, `the tutorial game should have tens of steps, got ${steps}`);

    // The step index: what every step IS, in one buffer.
    put(bots); const ia = bots.wasm_replay_step_index(moves.length);
    put(web); const ib = web.wasm_replay_step_index(moves.length);
    assert.equal(ib, ia, 'different step-index lengths');
    assert.ok(ia > 0, 'the step index did not decode');
    assert.deepEqual(
        [...bytesOf(web).slice(web.wasm_io_ptr(), web.wasm_io_ptr() + ib)],
        [...bytesOf(bots).slice(bots.wasm_io_ptr(), bots.wasm_io_ptr() + ia)],
        'the two links disagree about what the steps of one code are');

    let masked = 0, logged = 0;
    for (let step = 0; step < steps; step++) {
        for (let viewer = 0; viewer < 3; viewer++) {
            put(bots); const la = bots.wasm_replay_step_masked_state(moves.length, step, viewer);
            put(web); const lb = web.wasm_replay_step_masked_state(moves.length, step, viewer);
            assert.equal(lb, la, `step ${step} viewer ${viewer}: different masked-state length`);
            if (la <= 0) continue;
            masked++;
            const a = bytesOf(bots).slice(bots.wasm_io_ptr(), bots.wasm_io_ptr() + la);
            const b = bytesOf(web).slice(web.wasm_io_ptr(), web.wasm_io_ptr() + lb);
            assert.deepEqual([...b], [...a], `step ${step} viewer ${viewer}: the two links masked the board differently`);
        }

        put(bots); const ga = bots.wasm_replay_step_logs(moves.length, step);
        put(web); const gb = web.wasm_replay_step_logs(moves.length, step);
        assert.equal(gb, ga, `step ${step}: different log length`);
        if (ga <= 0) continue;
        logged++;
        const a = bytesOf(bots).slice(bots.wasm_io_ptr(), bots.wasm_io_ptr() + ga);
        const b = bytesOf(web).slice(web.wasm_io_ptr(), web.wasm_io_ptr() + gb);
        assert.deepEqual([...b], [...a], `step ${step}: the two links wrote a different public log`);
    }
    // The loop must have done real work: a `continue` on every step would make
    // every assertion above vacuous and the test would still be green.
    assert.ok(masked > 30, `only ${masked} masked boards were compared`);
    assert.ok(logged > 10, `only ${logged} public logs were compared`);
});

test('the unmasked-state entry points the client boundary denies are absent from the browser module', () => {
    const web = exportNames('sdk/ts/wasm/web.wasm.gz');
    const present = CLIENT_BOUNDARY.deniedWasmExports.filter((n) => web.has(n));
    assert.deepEqual(present, [],
        'e2e/security_client_boundary.test.ts denies these to the browser bundle. Now that the\n'
        + 'browser has its own link they can be denied harder - by not being in the module at all -\n'
        + 'and putting one back into c/Makefile WASM_WEB_NAMES would give that up.');
    // And the same for the rest of the server's surface, which is the bulk of
    // the bytes the browser used to download.
    const bots = exportNames('sdk/ts/wasm/bots.wasm.gz');
    const serverOnly = [...bots].filter((n) => n.startsWith('wasm_table_') || n === 'wasm_choose_move' || n === 'wasm_bot_drive');
    assert.ok(serverOnly.length > 30, `expected bots.wasm to carry the C Table, found ${serverOnly.length} names`);
    assert.deepEqual(serverOnly.filter((n) => web.has(n)), [], 'the browser module carries server-side entry points');
});

test('web.wasm exports exactly the kernel entries the browser bundle reaches', async () => {
    const reach = await browserWasmReach();
    const web = exportNames('sdk/ts/wasm/web.wasm.gz');
    const bots = exportNames('sdk/ts/wasm/bots.wasm.gz');

    // Names in the bundle that no kernel module exports (the Oracle's own
    // modules, `wasm_asset`) are not this test's business.
    const wanted = [...reach].filter((n) => bots.has(n)).sort();
    assert.ok(wanted.length > 40, `the reach scan found only ${wanted.length} kernel entries - did the bundle build?`);

    const missing = wanted.filter((n) => !web.has(n));
    assert.deepEqual(missing, [],
        'the browser bundle calls these and its module does not export them - they would be\n'
        + '`undefined is not a function` in a tab. Add each to WASM_WEB_NAMES in c/Makefile.');

    const unreachable = [...web].filter((n) => !reach.has(n)).sort();
    assert.deepEqual(unreachable, [],
        'web.wasm exports these and no browser entry reaches them any more. An export is a GC\n'
        + 'root, so each one is code the browser downloads and never runs - drop it from\n'
        + 'WASM_WEB_NAMES in c/Makefile. (If it is genuinely called from somewhere the bundler\n'
        + 'cannot see, say so where the name is listed.)');
});
