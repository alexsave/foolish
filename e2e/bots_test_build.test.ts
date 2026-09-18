// The shipped bots.wasm carries no test-only export, and the test build is the
// shipped module plus exactly those (c/Makefile WASM_TEST_EXPORTS,
// e2e/helpers/bots_test_wasm.ts).
//
// An export roots its code: while the fixture sealer, the card-list parser and
// the durable roster codec were exported from bots.wasm.gz, every browser and
// edge cold start downloaded and compiled them. The export lists are read out of
// c/Makefile itself, so a test-only entry added there is held out automatically.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { botsTestWasm } from './helpers/bots_test_wasm.ts';
import { LAYOUT_HASH } from '../sdk/ts/gen/layout_hash.bots.ts';

const ROOT = new URL('../', import.meta.url);
const SHIPPED = process.env.BOTS_WASM_GZ ?? new URL('sdk/ts/wasm/bots.wasm.gz', ROOT);

function makeVar(name: string): string[] {
    const mk = readFileSync(new URL('c/Makefile', ROOT), 'utf8').replace(/\\\n/g, ' ');
    const line = mk.split('\n').find((l) => l.startsWith(`${name} :=`) || l.startsWith(`${name} =`));
    assert.ok(line, `c/Makefile defines ${name}`);
    const refs = [...line.matchAll(/\$\((WASM_[A-Z_]+)\)/g)].map((m) => m[1]);
    const own = [...line.matchAll(/--export=([a-z0-9_]+)/g)].map((m) => m[1]);
    return [...own, ...refs.flatMap(makeVar)];
}

const exportsOf = (bytes: Uint8Array) =>
    new Set(WebAssembly.Module.exports(new WebAssembly.Module(bytes as BufferSource)).map((e) => e.name));

test('the shipped bots.wasm exports no test-only entry; the test build exports them and everything shipped', () => {
    const testOnly = makeVar('WASM_TEST_EXPORTS');
    assert.ok(testOnly.includes('wasm_fixture_seal') && testOnly.includes('wasm_roster_encode'), `the test list was read: ${testOnly}`);
    const shipped = exportsOf(gunzipSync(readFileSync(SHIPPED)));
    const leaked = testOnly.filter((e) => shipped.has(e));
    assert.deepEqual(leaked, [], 'test-only exports in the shipped bots.wasm.gz');
    const testBuild = exportsOf(botsTestWasm());
    assert.deepEqual(testOnly.filter((e) => !testBuild.has(e)), [], 'test exports missing from the test build');
    assert.deepEqual([...shipped].filter((e) => !testBuild.has(e)), [], 'shipped exports missing from the test build');
    assert.deepEqual([...testBuild].filter((e) => !shipped.has(e) && !testOnly.includes(e)), [], 'the test build exports only the shipped entries and the test list');
});

test('the test build reports the layout the shipped module and the generated readers agree on', () => {
    const hashOf = (bytes: Uint8Array) =>
        ((new WebAssembly.Instance(new WebAssembly.Module(bytes as BufferSource), {}).exports as { wasm_layout_hash(): number }).wasm_layout_hash() >>> 0);
    assert.equal(hashOf(botsTestWasm()), LAYOUT_HASH >>> 0);
    assert.equal(hashOf(gunzipSync(readFileSync(SHIPPED))), LAYOUT_HASH >>> 0);
});
