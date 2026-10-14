// The rematch lobby, held to one answer in two engines: New game on a finished
// table seals a lobby of the same game, built wholly by the kernel
// (c/src/msg_wire.h msg_rematch_lobby), and the wasm kernel must build, byte for
// byte, the lobby the phone's native kernel builds.
//
// The two strings below are pinned on the native side too
// (c/tests/msg_wire_test.c test_rematch_golden; `msg_wire_test
// --rematch-fixture` prints them), so each engine is held to the bytes rather
// than to the other. What a rematch IS - the same game, a new deal drawn from
// the moment of the tap, who may start it, how Rule P ranks it - is the
// kernel's and is tested there (test_rematch_same_chain, test_rematch_taps,
// test_rematch_creator_gate).
//
// No shipped host of bots.wasm builds a rematch (the phone does), so the entry
// is the TEST build's (c/Makefile WASM_BOTS_UNSHIPPED_API).
//
// How a shared rematch link unfurls is no longer the web's: /m/<payload> is a
// c_base route (c_base/docs/VERCEL.md section 4a), golden-tested there.
//
// Run: npx tsx --test e2e/msg_rematch.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import { fixtureExports } from './helpers/table_fixture.ts';

const hex = (h: string) => Uint8Array.from(h.match(/../g)!.map((b) => parseInt(b, 16)));

// A finished 4-seat game (format 5)...
const FINISHED = hex(
    'f7050003606000000000000064000004011100000000000000006d380f1cae7638124850ee2cbae14e7f'
    + 'c19e018780f568cd99f0b18252d0fe21e43400040004416c657801044d69726102054a6f6e6173030550'
    + '726979616400' + '0370d96bdfca53aaf159b08e7238ee50564efe1f1864a345b71d69ef4147da284b47f32f'
    + '0cdca05a1cc4661a827ad6cfc3e8d7bafa');
// ...and the rematch lobby the native kernel built when seat 2 tapped New game
// on it at unix 0x1234 seconds and 567 ms.
const TAPPED_AT_MS = 0x1234 * 1000 + 567;
const NATIVE_LOBBY = hex(
    'f70600006060000000000000000002040100f887535ca3ce410d5792a42bb5bb707687ad8735658d6b93'
    + '11cd750e096921e6a88a4dea8f3814aa34120000ff44c9ed0100040004416c657801044d69726102054a'
    + '6f6e6173030550726979610000');

/** The test build's msg_rematch_lobby: the finished chain in, the lobby out, both in the replay-io buffer. */
function rematch(finished: Uint8Array, tappedAtMs: number, creatorSeat: number): Uint8Array {
    const ex = fixtureExports();
    const at = ex.wasm_replay_io_ptr();
    new Uint8Array(ex.memory.buffer).set(finished, at);
    const n = ex.wasm_msg_rematch(finished.length, tappedAtMs, creatorSeat);
    assert.ok(n > 0, `the kernel refused the rematch (${n})`);
    return new Uint8Array(ex.memory.buffer).slice(at, at + n);
}

test('wasm builds the rematch lobby the native kernel built, byte for byte', () => {
    assert.deepEqual(Array.from(rematch(FINISHED, TAPPED_AT_MS, 2)), Array.from(NATIVE_LOBBY));
});
