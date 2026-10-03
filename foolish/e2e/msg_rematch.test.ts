// The rematch through the SAME wasm kernel the web reads with: a finished game
// is rematched as the same game dealt again (c/src/msg_wire.h
// msg_rematch_lobby), sealed as an ordinary format 6 lobby.
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
// ...and the rematch lobby the native kernel built for it at sent_at 0x1234.
const NATIVE_LOBBY = hex(
    'f70600006060000000000000000000040100f887535ca3ce410d4c6221b3f1f8523435d2ba568b0261ea'
    + 'dba97adc530f5e4b78e7f56d1ddda9bd34120000ff44c9ed0100040004416c657801044d69726102054a'
    + '6f6e6173030550726979610000');

const SENT_AT_OFF = 58;   // msg_wire.h MSG_CLOCK_OFF

test('wasm builds the rematch lobby the native kernel built, byte for byte', () => {
    const lobby = kernelMsgRematch(FINISHED, 0x1234);
    assert.deepEqual(Array.from(lobby), Array.from(NATIVE_LOBBY));
});

test('the rematch is the same game, dealt again, seated as it finished', () => {
    const fin = kernelMsgDecode(FINISHED);
    const lob = kernelMsgDecode(kernelMsgRematch(FINISHED, 0x1234));
    assert.equal(fin.phase, 3);
    assert.equal(lob.format, 6, 'a rematch is an ordinary format-6 lobby');
    assert.equal(lob.phase, 0, 'a rematch is a lobby');
    assert.equal(lob.game_id, fin.game_id, 'the rematch left the game');
    assert.deepEqual(Array.from(lob.parent8), Array.from(fin.digest.subarray(0, 8)),
        'the rematch does not descend from the finished chain');
    assert.notDeepEqual(Array.from(lob.seed), Array.from(fin.seed), 'the rematch re-deals the old deal');
    assert.deepEqual(lob.joins.slice().sort((a, b) => a.seat - b.seat),
                     fin.joins.slice().sort((a, b) => a.seat - b.seat));
    assert.equal(lob.n_players, 4, 'the rematch is the finished table, full');
    assert.notEqual(lob.carry_key, 0, "the fool's penalty did not carry over");
});

test('every tap is the same lobby but for its send clock, and every one outranks the finished game', () => {
    const taps = [0x1111, 0x2222, 0x3333].map((t) => kernelMsgRematch(FINISHED, t));
    for (const t of taps) {
        assert.equal(t.length, taps[0].length);
        for (let i = 0; i < t.length; i++) {
            if (i === SENT_AT_OFF || i === SENT_AT_OFF + 1) continue;
            assert.equal(t[i], taps[0][i], `byte ${i} depends on who tapped`);
        }
        assert.ok(kernelMsgRuleP(FINISHED, t) > 0, 'Rule P kept the finished game over its rematch');
        assert.ok(kernelMsgRuleP(t, FINISHED) < 0, '(reversed) Rule P kept the finished game');
    }
    for (const a of taps) for (const b of taps) {
        if (a === b) continue;
        assert.equal(kernelMsgRuleP(a, b), -kernelMsgRuleP(b, a), 'two lobbies order differently by who asks');
    }
});

test('only a finished game can be rematched', () => {
    const lobby = kernelMsgRematch(FINISHED, 0x1234);
    assert.throws(() => kernelMsgRematch(lobby, 0x1234), /bad phase/);
});

test('a shared rematch link unfurls as a rematch, not as turn 0', async () => {
    const { generateMetadata } = await import('../src/app/m/[payload]/layout.tsx');
    const lobby = kernelMsgRematch(FINISHED, 0x1234);
    const m = await generateMetadata({ params: Promise.resolve({ payload: '1' + kernelB32Encode(lobby) }) });
    assert.match(String(m.title), /Alex vs Mira vs Jonas vs Priya/);
    assert.match(String(m.title), /a rematch, waiting to start/);
    const f = await generateMetadata({ params: Promise.resolve({ payload: '1' + kernelB32Encode(FINISHED) }) });
    assert.match(String(f.title), /a finished Durak game/);
});
