/* =============================================================================
 * bots.wasm's loader must work in the runtime that ships, not just the one that tests
 * =============================================================================
 * One tracked binary reaches three runtimes through three different doors:
 *
 *   Deno edge  -> Deno.readFileSync
 *   Node CJS   -> require('node:fs')        <- what every test here runs under
 *   Node ESM   -> await import('node:fs/promises')
 *   browser    -> fetch()
 *
 * This test exists because the whole suite runs under tsx, which is CJS — so the
 * CJS door is the ONLY one the tests exercised, and it hid a real production
 * break: Next's server bundle is ESM, where `require` is undefined. The sync
 * probe returned null, the async path fell through to `fetch('file:///…')`,
 * which Node does not support, and /m/'s unfurl threw in production while every
 * test stayed green.
 *
 * So this spawns a real ESM child. Testing the ESM door from inside CJS is not
 * possible, and asserting it in the runtime that already works proves nothing.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { writeFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';

import { loadWasmGz, loadWasmGzAsync } from '../sdk/ts/wasm/wasm_asset.ts';

const ASSET = new URL('../sdk/ts/wasm/wasm_asset.ts', import.meta.url).pathname;

test('the CJS door (what the suite itself runs on) reads the asset', async () => {
    const sync = loadWasmGz('bots');
    const async_ = await loadWasmGzAsync('bots');
    assert.ok(sync.length > 0, 'sync read produced bytes');
    assert.deepEqual(Array.from(sync.slice(0, 4)), [0x00, 0x61, 0x73, 0x6d], 'is wasm');
    assert.equal(async_.length, sync.length, 'both doors agree');
});

test('the ESM door reads the asset — the runtime Next actually serves from', () => {
    // A real ESM child: no `require`, exactly like Next's server bundle.
    const probe = join(tmpdir(), `foolish_esm_door_${process.pid}.mjs`);
    writeFileSync(probe, `
const { loadWasmGzAsync } = await import(${JSON.stringify(ASSET)});
const b = await loadWasmGzAsync('bots');
if (typeof require !== 'undefined') { console.log('NOT_ESM'); process.exit(1); }
console.log('OK ' + b.length + ' ' + Array.from(b.slice(0, 4)).join(','));
`);
    try {
        const out = execFileSync(process.execPath, ['--experimental-strip-types', probe], {
            encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'],
        }).trim();
        assert.ok(out.startsWith('OK '), `ESM door failed: ${out}`);
        const [, len, magic] = out.split(' ');
        assert.ok(Number(len) > 0, 'ESM read produced bytes');
        assert.equal(magic, '0,97,115,109', 'ESM read produced wasm, not a gzip or a fetch error');
    } catch (e: unknown) {
        const err = e as { stderr?: string; message?: string };
        assert.fail(
            'the ESM door is broken — this is what Next\'s server bundle uses, and it ' +
            'fails in production while every tsx (CJS) test passes:\n' +
            (err.stderr || err.message || String(e)).slice(0, 400));
    } finally {
        try { unlinkSync(probe); } catch { /* best effort */ }
    }
});
