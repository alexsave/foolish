// The layout handshake: a kernel module and the generated TS that reads it must
// come from the same C headers, or the host refuses the module at load.
//
// c/Makefile bakes tools/structgen's layout hash into every wasm module
// (wasm_layout_hash); sdk/ts/gen/layout_hash.<build>.ts carries the same number
// as LAYOUT_HASH; bots.ts, server_table.ts and client_table.ts compare the two
// once per bots.wasm instance, before wasm_init (sdk/ts/wasm/layout_hash.ts).
//
// A doctored LAYOUT_HASH stands in for "the headers moved and only one side was
// rebuilt": the load must throw, name the module and the generated file, and
// leave the host retryable, so the real hash then loads normally.
//
// THE THIRD TEST CARRIES MORE WEIGHT THAN IT USED TO. sdk/ts/gen is no longer
// committed - every build regenerates it - while bots.wasm.gz and the two
// oracle modules still are. So the two halves no longer go stale together: a
// header edit moves the generated side immediately and leaves the committed
// modules where they were, and this is the test that says so. It was measured,
// not assumed: widening one field of Game (`int8_t num_battles` -> int32_t) and
// regenerating moved LAYOUT_HASH from 0xd61d98ee to 0x9b485bb8, and the load of
// the committed bots.wasm.gz threw
//
//   bots.wasm was built for Game layout 0xd61d98ee, but
//   sdk/ts/gen/layout_hash.bots.ts describes 0x…: the module and the generated
//   accessors come from different C headers.
//
// before wasm_init, not a wrong offset afterwards (sdk/ts/wasm/layout_hash.ts
// is called before the first kernel call, by construction).
//
// Pure kernel test - needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { gunzip } from '../sdk/ts/wasm/gunzip.ts';
import { __overrideLayoutHash, assertLayoutHash } from '../sdk/ts/wasm/layout_hash.ts';
import { __ensureBots, kernelBotRoster } from '../sdk/ts/wasm/bots.ts';
import { LAYOUT_HASH as BOTS_LAYOUT_HASH } from '../sdk/ts/gen/layout_hash.bots.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; }

const doctored = (h: number) => (h ^ 0x5a5a5a5a) >>> 0;
const hex = (h: number) => `0x${(h >>> 0).toString(16).padStart(8, '0')}`;

test('bots.wasm: a doctored LAYOUT_HASH refuses the module at load; the real one loads', () => {
    __overrideLayoutHash(doctored(BOTS_LAYOUT_HASH));
    try {
        assert.throws(() => __ensureBots(), (e: Error) => {
            assert.match(e.message, /bots\.wasm/);
            assert.match(e.message, /sdk\/ts\/gen\/layout_hash\.bots\.ts/);
            assert.ok(e.message.includes(hex(BOTS_LAYOUT_HASH)), `names the module's hash: ${e.message}`);
            return true;
        });
    } finally {
        __overrideLayoutHash(null);
    }
    __ensureBots();
    assert.ok(kernelBotRoster().length > 0, 'the real hash loads a working bots.wasm');
});

test('a module that exports no wasm_layout_hash (built before the handshake) is refused', () => {
    assert.throws(() => assertLayoutHash('old.wasm', {}, BOTS_LAYOUT_HASH, 'sdk/ts/gen/layout_hash.bots.ts'),
        /old\.wasm exports no wasm_layout_hash/);
});

// The committed module files themselves, not only the instance the host made.
// oracle.wasm and oracle-mt.wasm link wasm_api.c under their own flags
// (c/Makefile computes their hash separately); no TS reads their Game through
// generated accessors yet, so they are held to the bots layout they share today.
// If this fails because an oracle flag really moved the Game layout, generate a
// game_layout.<build>.ts and layout_hash.<build>.ts for that module rather than loosening the assertion.
test('every shipped module reports the layout of the generated module it is read with', () => {
    const bytes = (path: string) => gunzip(new Uint8Array(readFileSync(path)));
    const plain = (path: string) =>
        (new WebAssembly.Instance(new WebAssembly.Module(bytes(path) as BufferSource), {}).exports as
            unknown as { wasm_layout_hash(): number }).wasm_layout_hash() >>> 0;
    assert.equal(hex(plain('sdk/ts/wasm/bots.wasm.gz')), hex(BOTS_LAYOUT_HASH), 'bots.wasm.gz');
    assert.equal(hex(plain('public/oracle.wasm.gz')), hex(BOTS_LAYOUT_HASH), 'oracle.wasm.gz');
    const mt = new WebAssembly.Module(bytes('public/oracle-mt.wasm.gz') as BufferSource);
    const memory = new WebAssembly.Memory({ initial: 64, maximum: 2048, shared: true });
    const mtHash = (new WebAssembly.Instance(mt, { env: { memory } }).exports as
        unknown as { wasm_layout_hash(): number }).wasm_layout_hash() >>> 0;
    assert.equal(hex(mtHash), hex(BOTS_LAYOUT_HASH), 'oracle-mt.wasm.gz');
});
