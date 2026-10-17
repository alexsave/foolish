/* =============================================================================
 * The generated exports interfaces are the modules the build links
 * =============================================================================
 * `structgen --link` writes one TypeScript interface per link of the kernel
 * (sdk/ts/gen/exports.<link>.ts, tools/structgen/gen.sh): WHICH functions from
 * the `-Wl,--export=` lists in c/Makefile, WHAT they take from the C that
 * defines each one. Both of those are sources, and a module is a third thing -
 * what a recipe actually linked - so an interface derived from the sources is a
 * claim about the module until something reads the module. This does, three
 * ways, against all five:
 *
 *   1. BY THE GENERATOR ITSELF. `gen.sh --check-exports` re-derives every
 *      interface and holds it against the built module's own type, import,
 *      function and export sections, in C: every name in both directions, every
 *      kind, and every wasm value type of every parameter and result. That is
 *      the only check that can tell an f64 from an i32, since both are a
 *      `number` here.
 *   2. BY A SECOND READER, OF THE TEXT. The check above compares structgen's
 *      MODEL with the module. What a host imports is the TypeScript that model
 *      was printed as, so the printed file is scanned by the rules that have
 *      always held the hand-written binders (e2e/helpers/wasm_exports.ts) and
 *      compared with what an independent reader of the binary finds: the same
 *      names, the same arity, `bigint` exactly where the module takes an i64,
 *      `void` exactly where it returns nothing.
 *   3. BY THE COMPILER, AND BY CALLING ONE. The interfaces are type-checked
 *      with this file, an instance is cast to its interface and called through
 *      it, and a call with the wrong arity is a compile error rather than an
 *      `any`.
 *
 * WHAT IT NEEDS. The five modules under c/build. `npm run wasm` (the pre-hook of
 * every lane that runs this) builds three; the test build and the iMessage
 * extension's module are built here, on demand, the way every other suite that
 * loads the test build gets it. A module older than its sources fails case 1,
 * which is the right answer: it is not the module this tree's interfaces type.
 *
 * Pure test - no Postgres, no network.
 * ========================================================================== */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { botsTestWasm } from './helpers/bots_test_wasm.ts';
import { bindersIn, binderMismatches, wasmExportSignatures, type ModuleId } from './helpers/wasm_exports.ts';
import { LAYOUT_HASH } from '../sdk/ts/gen/layout_hash.bots.ts';
import type { BotsTestExports } from '../sdk/ts/gen/exports.bots_test.ts';
import type { MsgExports } from '../sdk/ts/gen/exports.msg.ts';
import type { OracleExports } from '../sdk/ts/gen/exports.oracle.ts';
import type { OracleMtExports } from '../sdk/ts/gen/exports.oracle_mt.ts';
import type { WebExports } from '../sdk/ts/gen/exports.web.ts';

const ROOT = fileURLToPath(new URL('../', import.meta.url));

/** Each generated interface, the module its link builds, and the scanner's name for it. */
const LINKS: readonly { iface: string; file: string; module: string; id: ModuleId }[] = [
    { iface: 'WebExports', file: 'exports.web.ts', module: 'web.wasm', id: 'web' },
    { iface: 'BotsTestExports', file: 'exports.bots_test.ts', module: 'bots_test.wasm', id: 'bots-test' },
    { iface: 'MsgExports', file: 'exports.msg.ts', module: 'msg.wasm', id: 'msg' },
    { iface: 'OracleExports', file: 'exports.oracle.ts', module: 'oracle.wasm', id: 'oracle' },
    { iface: 'OracleMtExports', file: 'exports.oracle_mt.ts', module: 'oracle-mt.wasm', id: 'oracle-mt' },
];

const wasmCc = (): string => {
    if (process.env.WASM_CC) return process.env.WASM_CC;
    const brew = '/opt/homebrew/opt/llvm/bin/clang';
    return existsSync(brew) ? brew : 'clang';
};

// The two modules no pre-hook builds. make decides whether there is anything
// to do: the test build under the helper's own lock and stamp, and msg.wasm in
// its own object directory, which no other suite builds into.
let built = false;
function modules(): void {
    if (built) return;
    botsTestWasm();
    execFileSync('make', ['-C', `${ROOT}c`, `WASM_CC=${wasmCc()}`, 'wasm-msg'], { stdio: ['ignore', 'ignore', 'inherit'] });
    for (const l of LINKS) {
        assert.ok(existsSync(`${ROOT}c/build/${l.module}`),
            `c/build/${l.module} is not built - run \`npm run wasm\` (it builds web and the Oracle pair)`);
    }
    built = true;
}

test('every generated exports interface is the module its link built (structgen --check-wasm)', () => {
    modules();
    const r = spawnSync('bash', [`${ROOT}tools/structgen/gen.sh`, '--check-exports'], { encoding: 'utf8' });
    assert.equal(r.status, 0, `gen.sh --check-exports refused:\n${r.stdout}${r.stderr}\n`
        + 'An interface and a module disagree. If the C or an export list changed, rebuild the modules '
        + '(npm run wasm; make -C c wasm-msg wasm-bots-test); if a recipe stopped using the variables '
        + 'tools/structgen/gen.sh names for its link, name the ones it uses now.');
    assert.match(r.stdout, /every exports interface is the module its link built/);
});

for (const l of LINKS) {
    test(`${l.file}: the printed interface declares exactly ${l.module}'s kernel exports, with their arity`, () => {
        modules();
        const sigs = wasmExportSignatures(readFileSync(`${ROOT}c/build/${l.module}`));
        const rel = `sdk/ts/gen/${l.file}`;
        const binders = bindersIn(readFileSync(ROOT + rel, 'utf8'), rel);
        assert.deepEqual(binders.map((b) => b.key), [`${rel}#${l.iface}`], 'one interface per generated file, named for its link');
        const [binder] = binders;
        assert.ok(binder.members.length > 50, `${rel} should type a whole kernel, saw ${binder.members.length} members`);
        // No member is optional: a link's exports are known, which is the point
        // of an interface per link.
        assert.deepEqual(binder.members.filter((m) => m.optional).map((m) => m.name), [], 'a generated interface has no optional member');
        assert.deepEqual(binderMismatches(binder, l.id, sigs), []);
        // ...and nothing the module exports is left out. `wasm_` is the scanner's
        // idea of a kernel entry; the linker's own (__wasm_init_tls, the TLS
        // globals) are case 1's, which compares every export of every kind.
        const declared = new Set(binder.members.map((m) => m.name));
        const exported = [...sigs.keys()].filter((n) => n.startsWith('wasm_'));
        assert.deepEqual(exported.filter((n) => !declared.has(n)), [], `${l.module} exports these and ${l.iface} does not declare them`);
        assert.equal(declared.size, binder.members.length, 'no export is declared twice');
        assert.equal(declared.size, exported.length);
    });
}

test('an instance cast to its generated interface is called through it, and a wrong call does not compile', () => {
    modules();
    const instance = (module: string) =>
        new WebAssembly.Instance(new WebAssembly.Module(readFileSync(`${ROOT}c/build/${module}`) as BufferSource), {}).exports;
    const web = instance('web.wasm') as unknown as WebExports;
    const testBuild = instance('bots_test.wasm') as unknown as BotsTestExports;
    // The same question of both links, each through its own interface: an i32
    // result arrives signed, as the generated banner says.
    for (const ex of [web, testBuild]) assert.equal(ex.wasm_layout_hash() >>> 0, LAYOUT_HASH >>> 0);
    assert.ok(web.memory.buffer.byteLength > 0, 'the memory is a member, typed as one');
    // The other three, so all five are type-checked with this file. The
    // extension's module and the Oracle's answer for their own layouts, and the
    // threaded Oracle imports its memory, so it is only named: what the linker
    // makes for it is typed too, and it has no `memory` to offer.
    assert.equal(typeof (instance('msg.wasm') as unknown as MsgExports).wasm_layout_hash(), 'number');
    assert.equal(typeof (instance('oracle.wasm') as unknown as OracleExports).wasm_layout_hash(), 'number');
    const threaded: (keyof OracleMtExports)[] = ['__wasm_init_tls', '__stack_pointer', 'wasm_mt_thread_main'];
    assert.equal(threaded.length, 3);
    // A function that takes arguments, called as typed, in both links that
    // carry it: a seven beats a six of its own suit and a six does not beat a
    // seven (suit 0, with suit 1 the power suit).
    for (const ex of [testBuild, web]) {
        assert.equal(ex.wasm_can_cover(0, 6, 0, 7, 1), 1);
        assert.equal(ex.wasm_can_cover(0, 7, 0, 6, 1), 0);
    }
    // The interface is a real type, not `any`: these are refused by tsc (the
    // type-check of this file is the test), and never run.
    const never = (): void => {
        // @ts-expect-error wasm_layout_hash takes no argument
        web.wasm_layout_hash(1);
        // @ts-expect-error wasm_can_cover takes five
        testBuild.wasm_can_cover(0, 6, 0, 7);
        // @ts-expect-error the browser's link does not export the table
        web.wasm_table_act(0, 0, 0, 0);
        // @ts-expect-error a test-only door is not in the shipped interface
        web.wasm_fixture_seal();
        // @ts-expect-error a module that imports its memory does not export one
        (null as unknown as OracleMtExports).memory;
    };
    assert.equal(typeof never, 'function');
    assert.equal(typeof testBuild.wasm_fixture_seal, 'function');
});
