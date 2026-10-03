// The rematch through the SAME wasm kernel the web reads with: New game on a
// finished table seals a lobby of the same game, naming the finished chain as
// its parent, seeded from the moment of the tap, as an ordinary format 6 lobby
// (c/src/msg_wire.h msg_rematch_lobby). Nothing on the wire is new.
//
// The fixtures were sealed by the NATIVE kernel (`msg_wire_test
// --rematch-fixture`), so the first test holds the two engines to one answer:
// wasm must build, byte for byte, the lobby the phone builds.
//
// Run: npx tsx --test e2e/msg_rematch.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import {
    kernelMsgDecode, kernelMsgRematch, kernelMsgRuleP, kernelB32Encode,
} from '../sdk/ts/wasm/bots.ts';

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

const T0 = 1790000000000;   // 2026-09, in unix ms
const sameBytes = (a: Uint8Array, b: Uint8Array) => a.length === b.length && a.every((v, i) => v === b[i]);

test('wasm builds the rematch lobby the native kernel built, byte for byte', () => {
    const lobby = kernelMsgRematch(FINISHED, TAPPED_AT_MS, 2);
    assert.deepEqual(Array.from(lobby), Array.from(NATIVE_LOBBY));
});

test('the rematch is an ordinary format 6 lobby of the same game, seated as it finished', () => {
    const fin = kernelMsgDecode(FINISHED);
    const lob = kernelMsgDecode(kernelMsgRematch(FINISHED, TAPPED_AT_MS, 2));
    assert.equal(fin.phase, 3);
    assert.equal(lob.format, 6, 'a rematch lobby is format 6, the wire every shipped build reads');
    assert.equal(lob.phase, 0, 'a rematch is a lobby');
    assert.equal(lob.game_id, fin.game_id, 'the rematch left the game');
    assert.deepEqual(Array.from(lob.parent8), Array.from(fin.digest.subarray(0, 8)),
        'the rematch does not descend from the finished chain');
    assert.equal(lob.last_actor_seat, 2, 'the lobby does not say its tapper sealed it');
    assert.equal(lob.sent_at, 0x1234, "the lobby's send clock is not the tap's");
    assert.notDeepEqual(Array.from(lob.seed), Array.from(fin.seed), 'the rematch re-deals the old deal');
    assert.deepEqual(lob.joins.slice().sort((a, b) => a.seat - b.seat),
                     fin.joins.slice().sort((a, b) => a.seat - b.seat));
    assert.equal(lob.n_players, 4, 'the rematch is the finished table, full');
    assert.notEqual(lob.carry_key, 0, "the fool's penalty did not carry over");
});

test('three taps are three lobbies of one game, and every one outranks the finished game', () => {
    const taps = [0, 1, 2].map((s) => kernelMsgRematch(FINISHED, T0 + s * 1337, s));
    const envs = taps.map((t) => kernelMsgDecode(t));
    const fin = kernelMsgDecode(FINISHED);
    for (const [s, e] of envs.entries()) {
        assert.equal(e.game_id, fin.game_id, `seat ${s}: the tap left the game`);
        assert.deepEqual(Array.from(e.parent8), Array.from(fin.digest.subarray(0, 8)),
            `seat ${s}: the tap does not name the finished chain`);
        assert.equal(e.last_actor_seat, s, `seat ${s}: the tap is not its tapper's`);
    }
    for (let a = 0; a < 3; a++) for (let b = a + 1; b < 3; b++) {
        assert.notDeepEqual(Array.from(envs[a].seed), Array.from(envs[b].seed),
            `seats ${a} and ${b} tapped at different moments and got one deal`);
    }
    for (const t of taps) {
        assert.ok(kernelMsgRuleP(FINISHED, t) > 0, 'Rule P kept the finished game over its rematch');
        assert.ok(kernelMsgRuleP(t, FINISHED) < 0, '(reversed) Rule P kept the finished game');
    }
    for (const a of taps) for (const b of taps) {
        if (a === b) continue;
        assert.notEqual(kernelMsgRuleP(a, b), 0, 'two different lobbies tied');
        assert.equal(kernelMsgRuleP(a, b), -kernelMsgRuleP(b, a), 'two lobbies order differently by who asks');
    }
});

test('one moment is one deal, whoever taps', () => {
    const a = kernelMsgDecode(kernelMsgRematch(FINISHED, T0 + 42, 0));
    const b = kernelMsgDecode(kernelMsgRematch(FINISHED, T0 + 42, 3));
    const c = kernelMsgDecode(kernelMsgRematch(FINISHED, T0 + 43, 0));
    assert.deepEqual(Array.from(a.seed), Array.from(b.seed), 'two seats at one moment got two deals');
    assert.notDeepEqual(Array.from(a.seed), Array.from(c.seed), 'one millisecond apart is one deal');
    assert.ok(sameBytes(kernelMsgRematch(FINISHED, T0 + 42, 0), kernelMsgRematch(FINISHED, T0 + 42, 0)),
        'one tap built twice is two bubbles');
});

test('only a finished game can be rematched, and only from a seat at its table', () => {
    const lobby = kernelMsgRematch(FINISHED, TAPPED_AT_MS, 2);
    assert.throws(() => kernelMsgRematch(lobby, TAPPED_AT_MS, 2), /bad phase/);
    assert.throws(() => kernelMsgRematch(FINISHED, TAPPED_AT_MS, 4));
    assert.throws(() => kernelMsgRematch(FINISHED, TAPPED_AT_MS, -1));
});

test('a shared rematch link unfurls as a rematch, not as turn 0', async () => {
    const { generateMetadata } = await import('../src/app/m/[payload]/layout.tsx');
    const lobby = kernelMsgRematch(FINISHED, TAPPED_AT_MS, 2);
    const m = await generateMetadata({ params: Promise.resolve({ payload: '1' + kernelB32Encode(lobby) }) });
    assert.match(String(m.title), /Alex vs Mira vs Jonas vs Priya/);
    assert.match(String(m.title), /a rematch, waiting to start/);
    const f = await generateMetadata({ params: Promise.resolve({ payload: '1' + kernelB32Encode(FINISHED) }) });
    assert.match(String(f.title), /a finished Durak game/);
});
