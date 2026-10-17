/* =============================================================================
 * web.wasm is the browser's LINK of the kernel, not a second kernel.
 * =============================================================================
 *
 * The browser fetches web.wasm: the bots objects (c/Makefile WASM_BOT_OBJ)
 * under a small `-Wl,--export=` allow-list (WASM_WEB_NAMES). An export is a GC
 * root, so the names the browser never calls do not keep the Monte-Carlo
 * brains, cordite's solver working set or the C Table alive in it. The same
 * sources run natively everywhere else: in c_base, in the iOS apps, and in the
 * table host the Node suites drive (c/tests/table_host.c), which calls the very
 * entry files this module exports.
 *
 * THIS FILE IS THE REASON THAT IS SAFE, and it is written against a specific
 * history. ../docs/ARCHITECTURE_AS_A_PATTERN.md Part 1 piece 1 records that
 * guards.wasm (a validate-only client build) and rules.wasm (a server-only
 * build) were deleted because they were "three kernels answering the same
 * questions". They were three SOURCES. One object set compiled for two targets
 * is not that - but "is not that" is a claim, and a claim with no gate has a
 * half-life, so the claim is checked here:
 *
 *   1. ONE LAYOUT. web.wasm answers wasm_layout_hash() with the generated
 *      constant, and the native host is refused on start unless it does too
 *      (e2e/helpers/table_host.ts). A layout hash is over field path, offset,
 *      size, kind and bit range, so agreeing on it is agreeing about every
 *      struct that crosses the boundary.
 *   2. ONE ANSWER. web.wasm and the native kernel are handed the same replay
 *      code and asked every question the browser asks of it - the step count,
 *      what each step is, each step's masked board for each viewer, each step's
 *      public log - and every byte must match. That is the engine, the v6
 *      decoder, the step walker and view.c's masking, compiled to wasm32 at -Oz
 *      and run through wasm-opt on one side and native on the other: the
 *      replay a phone or the server cut is the replay a tab reads.
 *   3. THE REACH SET. The export list is not a guess: esbuild bundles every
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
 * Pure test - no Postgres, no network. It reads web.wasm.gz (the `web` group of
 * scripts/wasm_build.sh) and starts a table host.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';

import { LAYOUT_HASH } from '../sdk/ts/gen/layout_hash.bots.ts';
import type { WebExports } from '../sdk/ts/gen/exports.web.ts';
import { RS_PLAY_STRIDE } from '../sdk/ts/gen/kernel_consts.bots.ts';
import { TUTORIAL_MOVES_CODE } from '../src/components/tutorialGame.ts';
import { CLIENT_BOUNDARY, browserWasmReach } from './helpers/client_bundle.ts';
import { startTableHost } from './helpers/table_host.ts';
import { bindersIn, calledExports, makefileExportNames } from './helpers/wasm_exports.ts';

const ROOT = fileURLToPath(new URL('../', import.meta.url));

const WEB = 'sdk/ts/wasm/web.wasm.gz';
/** A shipped module's raw bytes, inflated. */
const moduleBytes = (rel: string): Uint8Array => gunzipSync(readFileSync(ROOT + rel));

/**
 * Every entry the wasm entry files define, as far as any link or host asks for
 * one: what a link of the bots objects may export (c/Makefile
 * WASM_BOTS_ALL_EXPORTS) and what the native table host offers (its generated
 * interface, sdk/ts/gen/exports.table_host.ts).
 */
function kernelEntries(): Set<string> {
    const rel = 'sdk/ts/gen/exports.table_host.ts';
    const [host] = bindersIn(readFileSync(ROOT + rel, 'utf8'), rel);
    return new Set([...makefileExportNames(ROOT + 'c/Makefile', 'WASM_BOTS_ALL_EXPORTS'), ...host.members.map((m) => m.name)]);
}

/** A module's exported function names, out of its own export section. */
function exportNames(rel: string): Set<string> {
    const w = moduleBytes(rel);
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

// A raw instance of one link: no bridge, no wrapper, so the comparison below is
// of the MODULES and not of the TypeScript that usually stands in front of them.
function instantiate(rel: string): WebExports {
    const m = new WebAssembly.Module(moduleBytes(rel) as BufferSource);
    const ex = new WebAssembly.Instance(m, {}).exports as unknown as WebExports;
    ex.wasm_init();
    return ex;
}

const bytesOf = (ex: WebExports) => new Uint8Array(ex.memory.buffer);

test('web.wasm carries the generated layout hash', () => {
    const ex = instantiate(WEB);
    assert.equal(ex.wasm_layout_hash() >>> 0, LAYOUT_HASH >>> 0,
        `${WEB} was built from a different tree than sdk/ts/gen/layout_hash.bots.ts.\n`
        + 'Rebuild the group: scripts/wasm_build.sh web');
});

test('web.wasm and the native kernel answer the same replay code identically, step by step', () => {
    const web = instantiate(WEB);
    const host = startTableHost();
    const native = host.ex;

    // The tutorial's real 3-player game (src/components/tutorialGame.ts), cut by
    // the engine. It exercises the lot: lead, cover, trump cover, throw-in,
    // perevod, pickup, round end, refills, the deck running out and a seat
    // going out.
    const code = new TextEncoder().encode(TUTORIAL_MOVES_CODE);

    // Each side's IO, as bytes: web.wasm's memory, the host's through its pipe.
    const sides = {
        web: {
            ex: web,
            put: (at: number, b: Uint8Array) => bytesOf(web).set(b, at),
            get: (at: number, n: number) => bytesOf(web).slice(at, at + n),
        },
        native: {
            ex: native,
            put: (at: number, b: Uint8Array) => host.write(at, b),
            get: (at: number, n: number) => host.read(at, n),
        },
    };
    type Side = typeof sides.web | typeof sides.native;

    const decode = (s: Side): Uint8Array => {
        s.put(s.ex.wasm_replay_io_ptr(), code);
        const w = s.ex.wasm_replay_b32_decode(code.length);
        assert.ok(w > 0, 'the tutorial code did not decode - it may need re-cutting (e2e/gen_tutorial_game.ts)');
        return s.get(s.ex.wasm_io_ptr(), w);
    };
    const moves = decode(sides.native);
    assert.deepEqual([...decode(sides.web)], [...moves], 'web.wasm and the native kernel disagree about base32');

    const put = (s: Side) => s.put(s.ex.wasm_replay_io_ptr(), moves);
    const io = (s: Side, n: number) => s.get(s.ex.wasm_io_ptr(), n);

    put(sides.native); const steps = native.wasm_replay_step_count(moves.length);
    put(sides.web); assert.equal(web.wasm_replay_step_count(moves.length), steps, 'different step counts');
    assert.ok(steps > 20, `the tutorial game should have tens of steps, got ${steps}`);

    // The steps as a replay screen plays them: what every step IS, in one
    // buffer (no extras either side, so no clock).
    put(sides.native); const ia = native.wasm_replay_step_play(moves.length) * RS_PLAY_STRIDE;
    put(sides.web); const ib = web.wasm_replay_step_play(moves.length) * RS_PLAY_STRIDE;
    assert.equal(ib, ia, 'different step-list lengths');
    assert.equal(ia, steps * RS_PLAY_STRIDE, 'the step list did not decode');
    assert.deepEqual([...io(sides.web, ib)], [...io(sides.native, ia)],
        'web.wasm and the native kernel disagree about what the steps of one code are');

    let masked = 0, logged = 0;
    for (let step = 0; step < steps; step++) {
        for (let viewer = 0; viewer < 3; viewer++) {
            put(sides.native); const la = native.wasm_replay_step_masked_state(moves.length, step, viewer);
            put(sides.web); const lb = web.wasm_replay_step_masked_state(moves.length, step, viewer);
            assert.equal(lb, la, `step ${step} viewer ${viewer}: different masked-state length`);
            if (la <= 0) continue;
            masked++;
            assert.deepEqual([...io(sides.web, lb)], [...io(sides.native, la)],
                `step ${step} viewer ${viewer}: web.wasm and the native kernel masked the board differently`);
        }

        put(sides.native); const ga = native.wasm_replay_step_logs(moves.length, step);
        put(sides.web); const gb = web.wasm_replay_step_logs(moves.length, step);
        assert.equal(gb, ga, `step ${step}: different log length`);
        if (ga <= 0) continue;
        logged++;
        assert.deepEqual([...io(sides.web, gb)], [...io(sides.native, ga)],
            `step ${step}: web.wasm and the native kernel wrote a different public log`);
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
    const serverOnly = [...kernelEntries()].filter((n) => n.startsWith('wasm_table_') || n === 'wasm_choose_move' || n === 'wasm_bot_drive');
    assert.ok(serverOnly.length > 30, `expected the kernel's entries to include the C Table, found ${serverOnly.length} names`);
    assert.deepEqual(serverOnly.filter((n) => web.has(n)), [], 'the browser module carries server-side entry points');
});

test('web.wasm exports exactly the kernel entries the browser bundle reaches', async () => {
    const reach = await browserWasmReach();
    const web = exportNames('sdk/ts/wasm/web.wasm.gz');
    const kernel = kernelEntries();
    // The scan is by name over the whole bundle, so it also sees what the
    // Oracle's code (src/oracle) calls on the Oracle's own modules: choose_move,
    // the state and log imports. Those names are the Oracle's business, not this
    // link's, unless web.wasm exports them too. Read from the Oracle's source
    // rather than from oracle.wasm's export table, which carries every anim and
    // replay entry as well and would hide a missing one here.
    const oracleCalls = calledExports(ROOT + 'src/oracle');
    assert.ok(oracleCalls.has('wasm_choose_move'), 'the Oracle\'s calls were read');
    const wanted = [...reach].filter((n) => kernel.has(n) && !(oracleCalls.has(n) && !web.has(n))).sort();
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
