/* =============================================================================
 * The kernel's export table and the TypeScript that binds it are one contract
 * =============================================================================
 * c/Makefile declares the exports, c/wasm/*.c defines them, and twelve
 * hand-written TypeScript interfaces declare the other side of every one. Until
 * this gate, nothing compared the three: measured over the repo's history,
 * c/wasm/wasm_api.c and sdk/ts/wasm/bots.ts changed together in fifteen commits
 * - 46% of one file's changes and 32% of the other's - and in none of them
 * could either side have failed because of the other. A missing export was
 * caught (e2e/wasm_artifact.test.ts, by name, for sdk/ts only); a WRONG one
 * never was.
 *
 * WHY THE MODULE IS THE TRUTH. The linked .wasm is the only artifact where the
 * Makefile's list and the C definitions have already met. Reading it needs no
 * compiler - the modules are committed - which is what lets this sit in the
 * fast lane and run on every pull request, and it cannot drift from what ships
 * because it IS what ships. The Makefile is read for exactly one thing, the
 * test build, which is deliberately never committed.
 *
 * WHAT DRIFT LOOKS LIKE, and which test here catches it:
 *
 *   an entry point is renamed in C           -> "declared, but bots.wasm does
 *                                               not export it"
 *   an entry point GROWS a parameter         -> "declared with 3 parameter(s),
 *                                               bots.wasm takes 4"; without
 *                                               this the call site passes three
 *                                               and the fourth is whatever the
 *                                               stack held
 *   an entry point starts returning a code   -> "declared to return void,
 *                                               bots.wasm returns i32"; without
 *                                               this the refusal is discarded
 *   an export is added and never bound       -> "bots.wasm exports ... that no
 *                                               TypeScript binds"; an export
 *                                               ROOTS its code, and this module
 *                                               is capped at 80 KiB gzip
 *                                               (e2e/mem/wasm_memory.test.ts)
 *   a new binder appears                     -> "says which module it binds";
 *                                               it must name its module here
 *
 * NOT A GENERATOR, and deliberately. tools/structgen earns its libclang for
 * facts only a compiler knows - a field's offset, a struct's size. An export's
 * arity is not one of those: it is in the module's own type section, already
 * compiled, and structgen could not answer the question anyway, because which
 * of the ~200 `wasm_*` definitions a given module exports is not in any header
 * - it is in the link. Generating the interfaces alone would also move the
 * drift rather than remove it: the ~120 declaration lines are a twentieth of
 * the files that hold them, and the marshalling around them stays hand-written
 * either way.
 *
 * Pure test - no Postgres, no network, no compiler.
 * ========================================================================== */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';

import {
    BINDER_MODULES, binderMismatches, makefileExportNames, wasmBinders, wasmExportSignatures,
    type ModuleId, type WasmSig,
} from '../helpers/wasm_exports.ts';

const ROOT = fileURLToPath(new URL('../../', import.meta.url));

/** The committed modules, by the id BINDER_MODULES uses. bots-test is not one: it is built on demand. */
const COMMITTED: Record<string, string> = {
    'bots': 'sdk/ts/wasm/bots.wasm.gz',
    'oracle': 'public/oracle.wasm.gz',
    'oracle-mt': 'public/oracle-mt.wasm.gz',
};

const sigsOf = (rel: string): Map<string, WasmSig> =>
    wasmExportSignatures(gunzipSync(readFileSync(ROOT + rel)));

const binders = wasmBinders(ROOT);

test('every interface that declares a kernel entry point says which module it binds', () => {
    // Found by shape - an interface with a `wasm_*` member - so a binder cannot
    // hide behind a new naming convention. Both directions: an undeclared
    // binder is unchecked, and a key for an interface that no longer exists
    // means one was renamed or moved and quietly left the check.
    const found = binders.map((b) => b.key).sort();
    const declared = Object.keys(BINDER_MODULES).sort();
    assert.deepEqual(found.filter((k) => !declared.includes(k)), [],
        'add these to BINDER_MODULES in e2e/helpers/wasm_exports.ts, naming the module each one binds');
    assert.deepEqual(declared.filter((k) => !found.includes(k)), [],
        'BINDER_MODULES names interfaces that no longer exist - drop or re-point them');
    // A count this small would mean the scanner stopped finding things.
    assert.ok(found.length >= 12, `expected the tree to hold at least a dozen binders, found ${found.length}`);
});

for (const [id, rel] of Object.entries(COMMITTED)) {
    test(`${id}.wasm exports what the TypeScript binding it declares`, () => {
        const sigs = sigsOf(rel);
        assert.ok(sigs.size > 50, `${rel} should export a whole kernel, saw ${sigs.size} functions`);
        const problems: string[] = [];
        let bound = 0;
        for (const b of binders) {
            if (BINDER_MODULES[b.key] !== (id as ModuleId)) continue;
            bound++;
            problems.push(...binderMismatches(b, id as ModuleId, sigs));
        }
        assert.ok(bound > 0, `no binder is bound to ${id}`);
        assert.deepEqual(problems, [],
            `\n  ${problems.join('\n  ')}\n\nRebuild after a C change: cd c && make wasm-bots WASM_CC=/opt/homebrew/opt/llvm/bin/clang`);
    });
}

test('the shipped bots.wasm exports nothing the TypeScript leaves unbound', () => {
    // The other direction, and the one the history is full of: an entry point
    // is exported, the TS that would call it lands later or never, and the
    // module carries the code in the meantime. Only bots.wasm is held to this -
    // it is the module every browser downloads, and the one with a byte cap.
    // The oracle modules deliberately export the whole kernel so the Oracle
    // page can drive any of it, and the test build is checked in
    // e2e/bots_test_build.test.ts.
    const shipped = [...sigsOf(COMMITTED.bots).keys()];
    const boundToBots = new Set(binders
        .filter((b) => BINDER_MODULES[b.key] === 'bots' || BINDER_MODULES[b.key] === 'bots-test')
        .flatMap((b) => b.members.map((m) => m.name)));

    const unbound = shipped.filter((n) => !boundToBots.has(n)).sort();
    assert.deepEqual(unbound, [], `bots.wasm exports ${unbound.join(', ')} that no TypeScript binds - `
        + 'either bind it, or add it to WASM_BOTS_UNSHIPPED_API in c/Makefile so the shipped link drops it');
});

test('the test build binders name entries the kernel exports', () => {
    // c/build/bots_test.wasm is never committed, so in a lane with no compiler
    // its surface is the shipped module plus c/Makefile's WASM_TEST_EXPORTS -
    // an equality e2e/bots_test_build.test.ts proves against the built module,
    // and where the same file also checks these binders' signatures.
    const shipped = sigsOf(COMMITTED.bots);
    const testOnly = makefileExportNames(ROOT + 'c/Makefile', 'WASM_TEST_EXPORTS');
    assert.ok(testOnly.includes('wasm_fixture_seal'), `the test list was read: ${testOnly.length} names`);

    const missing: string[] = [];
    for (const b of binders) {
        if (BINDER_MODULES[b.key] !== 'bots-test') continue;
        for (const m of b.members) {
            if (!shipped.has(m.name) && !testOnly.includes(m.name)) missing.push(`${b.file}:${m.line} ${m.name}`);
        }
    }
    assert.deepEqual(missing, [], 'declared against the test build, but neither shipped nor in WASM_TEST_EXPORTS');
});
