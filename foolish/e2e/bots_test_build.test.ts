// The shipped web.wasm carries no test-only export, and the test build exports
// every entry the kernel's objects have (c/Makefile WASM_BOTS_ALL_EXPORTS,
// e2e/helpers/bots_test_wasm.ts) - web.wasm's among them.
//
// An export roots its code: while the fixture sealer, the card-list parser and
// the durable roster codec were exported from a shipped module, every browser
// cold start downloaded and compiled them. The export lists are read out of
// c/Makefile itself, so a test-only entry added there is held out automatically.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { gunzipSync } from 'node:zlib';
import { botsTestWasm } from './helpers/bots_test_wasm.ts';
import { LAYOUT_HASH } from '../sdk/ts/gen/layout_hash.bots.ts';
import { makefileExportNames } from './helpers/wasm_exports.ts';

const ROOT = new URL('../', import.meta.url);
const SHIPPED = new URL('sdk/ts/wasm/web.wasm.gz', ROOT);

const makeVar = (name: string) => makefileExportNames(fileURLToPath(new URL('c/Makefile', ROOT)), name);

const exportsOf = (bytes: Uint8Array) =>
    new Set(WebAssembly.Module.exports(new WebAssembly.Module(bytes as BufferSource)).map((e) => e.name));

test('the shipped web.wasm exports no test-only entry; the test build exports them and everything shipped', () => {
    const testOnly = ['WASM_ROSTER_EXPORTS', 'WASM_FIXTURE_EXPORTS', 'WASM_REPLAY_DECODE_EXPORTS'].flatMap(makeVar);
    assert.ok(testOnly.includes('wasm_fixture_seal') && testOnly.includes('wasm_roster_encode'), `the test lists were read: ${testOnly}`);
    const all = makeVar('WASM_BOTS_ALL_EXPORTS');
    assert.deepEqual(testOnly.filter((e) => !all.includes(e)), [], 'the test-only lists are in the whole list');
    const shipped = exportsOf(gunzipSync(readFileSync(SHIPPED)));
    assert.deepEqual(testOnly.filter((e) => shipped.has(e)), [], 'test-only exports in the shipped web.wasm.gz');
    const testBuild = exportsOf(botsTestWasm());
    assert.deepEqual(all.filter((e) => !testBuild.has(e)), [], 'entries of WASM_BOTS_ALL_EXPORTS missing from the test build');
    assert.deepEqual([...shipped].filter((e) => !testBuild.has(e)), [], 'shipped exports missing from the test build');
});

test('the test build reports the layout the shipped module and the generated readers agree on', () => {
    const hashOf = (bytes: Uint8Array) =>
        ((new WebAssembly.Instance(new WebAssembly.Module(bytes as BufferSource), {}).exports as { wasm_layout_hash(): number }).wasm_layout_hash() >>> 0);
    assert.equal(hashOf(botsTestWasm()), LAYOUT_HASH >>> 0);
    assert.equal(hashOf(gunzipSync(readFileSync(SHIPPED))), LAYOUT_HASH >>> 0);
});
