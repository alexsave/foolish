/* =============================================================================
 * What hand-written TypeScript says about the kernel's export table
 * =============================================================================
 * Every host casts `instance.exports` to the interface structgen writes for its
 * link (sdk/ts/gen/exports.<link>.ts), which the compiler holds each call to
 * and e2e/wasm_exports.test.ts holds to the built module. Three things are left
 * that a generated interface cannot say:
 *
 *   a hand-typed binder appears              -> it is not in the closed list
 *                                               (BINDER_MODULES): cast to the
 *                                               generated interface instead
 *   a listed binder drifts from its module   -> "declared with 3 parameter(s),
 *                                               web.wasm takes 4"
 *
 * An export nothing calls is web.wasm's question, and e2e/wasm_web_link.test.ts
 * answers it both ways against the browser's measured reach set. (This file used
 * to ask it of bots.wasm, the server's module, which is gone with the server.)
 *
 * Pure test - no Postgres, no network, no compiler: the modules are built by the
 * pre-hook (npm run wasm).
 * ========================================================================== */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';

import {
    BINDER_MODULES, binderMismatches, wasmBinders, wasmExportSignatures, type WasmSig,
} from '../helpers/wasm_exports.ts';

const ROOT = fileURLToPath(new URL('../../', import.meta.url));

/** The committed modules a listed binder can bind, by the id BINDER_MODULES uses. */
const COMMITTED: Record<string, string> = {
    'web': 'sdk/ts/wasm/web.wasm.gz',
    'oracle': 'public/oracle.wasm.gz',
    'oracle-mt': 'public/oracle-mt.wasm.gz',
};

const sigsOf = (rel: string): Map<string, WasmSig> =>
    wasmExportSignatures(gunzipSync(readFileSync(ROOT + rel)));

const binders = wasmBinders(ROOT);

test('the hand-typed interfaces that declare a kernel entry point are the listed ones', () => {
    // Found by shape - an interface with a `wasm_*` member - so a binder cannot
    // hide behind a naming convention. Both directions: an unlisted binder is
    // unchecked, and a key for an interface that is gone must leave the list.
    const found = binders.map((b) => b.key).sort();
    const declared = Object.keys(BINDER_MODULES).sort();
    assert.deepEqual(found.filter((k) => !declared.includes(k)), [],
        'cast to the generated interface of the link (sdk/ts/gen/exports.<link>.ts) instead of typing its exports by hand');
    assert.deepEqual(declared.filter((k) => !found.includes(k)), [],
        'BINDER_MODULES names interfaces that no longer exist - drop them');
});

test('each listed binder declares what its module exports', () => {
    const problems = binders.flatMap((b) => {
        const id = BINDER_MODULES[b.key];
        return id ? binderMismatches(b, id, sigsOf(COMMITTED[id])) : [];
    });
    assert.deepEqual(problems, [],
        `\n  ${problems.join('\n  ')}\n\nRebuild after a C change: npm run wasm`);
});
