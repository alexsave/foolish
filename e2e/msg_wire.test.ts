// FMSG — the iMessage envelope, through the real kernel wasm.
//
// The native suite (c/tests/msg_wire_test.c) is where the codec is proven;
// this is the WASM PARITY half: the same bytes must mean the same game on the
// web as in libfoolish.a, or an iMessage game forks between a phone and a
// browser. Fixtures here are hex so a Swift port (M3) can be held to them too.
//
// Imported from bots.ts, not engine.ts: FMSG lives on the one big module (only
// it can hold the session log a seal reads), and calling in instantiates it.
//
// Run: npx tsx --test e2e/msg_wire.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import { kernelMsgDecode } from '../sdk/ts/wasm/bots.ts';

// Sealed by the NATIVE producer (`c/build/msg_wire_test --fixture`) from
// seeded games, cut mid-game — a turn bubble, which is what actually ships.
// Regenerate with that command if the envelope layout ever changes; a diff here
// is a wire break, and every shipped device would have to agree with it.
const FIXTURES = [
    { n_players: 2, turn: 8, round: 2, hex:
      'f7020002efcdab89674523010800000200020000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e310800f72719e90cb7ee031bd6af74a3a23a' },
    { n_players: 3, turn: 10, round: 1, hex:
      'f7020002efcdab89674523010a0000030001000000000000000079d87206410d37d302c19dfb6cacbc8bebf879d242622082315709cc0f183788030004416e6e300104416e6e310204416e6e320a00012951da5bef3096f9f7bf2cfb58d013f6d7fa' },
    { n_players: 4, turn: 7, round: 1, hex:
      'f7020002efcdab89674523010700000400010000000000000000449bbad52d5dfb1bdb68d87a09fe591b9419f9f39b0ec35e9f2b75c5a359a138040004416e6e300104416e6e310204416e6e320304416e6e33070001c32dd6c13bd1e53963f945fef906649a' },
];

function hexToBytes(h: string): Uint8Array {
    const out = new Uint8Array(h.length / 2);
    for (let i = 0; i < out.length; i++) out[i] = parseInt(h.slice(i * 2, i * 2 + 2), 16);
    return out;
}

test('a hand-built envelope is refused unless the body is a code for its seed', () => {
    // magic F7, format 2, flags 0, phase LIVE(2), game_id, turn 0, ...
    const e = new Uint8Array(59 + 2 + 2 + 1);
    e[0] = 0xf7; e[1] = 2; e[2] = 0; e[3] = 2;
    e[15] = 2;                       // n_players
    e[26] = 1;                       // a non-zero seed byte (all-zero is refused)
    e[58] = 1;                       // n_joins
    e[59] = 0; e[60] = 0;            // join: seat 0, name_len 0
    e[61] = 0; e[62] = 0;            // n_actions = 0
    e[63] = 0;                       // a one-byte body: not a real code
    assert.throws(() => kernelMsgDecode(e), /iMessage payload/);
});

test('the envelope is rejected before it can be replayed: magic, format, seed', () => {
    const bad = (mutate: (b: Uint8Array) => void, re: RegExp) => {
        const e = new Uint8Array(59 + 2 + 2);
        e[0] = 0xf7; e[1] = 2; e[3] = 2; e[15] = 2; e[26] = 1; e[58] = 1;
        mutate(e);
        assert.throws(() => kernelMsgDecode(e), re);
    };
    bad(b => { b[0] = 0xf6; }, /not an FMSG envelope/);
    bad(b => { b[1] = 1; }, /unsupported format/);      // the raw format was cut
    // 3 is the CLOCK format and 4 the REMATCH format since round 16, and both
    // decode, so the first byte above the wire is 5. (Flipping this buffer to 3
    // or 4 shifts n_joins along, so those fail as bad-joins/truncated payloads
    // rather than as unknown formats - which the truncation test below covers.)
    bad(b => { b[1] = 5; }, /unsupported format/);
    bad(b => { b[2] = 0x01; }, /unsupported flags/);    // fair-deal: spec'd, unbuilt
    bad(b => { b[2] = 0x08; }, /unsupported flags/);    // reserved bit (0x04 = legacy passing-allowed, tolerated for 1.0(3) msgs)
    bad(b => { b[15] = 1; }, /bad player count/);
    bad(b => { b[15] = 9; }, /bad player count/);
    bad(b => { b[16] = 1; }, /unknown variant/);
    bad(b => { b[26] = 0; }, /dead deal seed/);         // all-zero seed
});

test('a truncated envelope never crashes the kernel', () => {
    const e = new Uint8Array(59 + 2 + 2);
    e[0] = 0xf7; e[1] = 2; e[3] = 2; e[15] = 2; e[26] = 1; e[58] = 1;
    for (let cut = 0; cut < e.length; cut++) {
        // Either it throws or it decodes; it must never take the process down.
        try { kernelMsgDecode(e.subarray(0, cut)); } catch { /* expected */ }
    }
});

// ROUND 16's header, sealed by the same native producer from the same states:
// format 3, a send clock, and a BUBBLE DELTA (n_new) saying the last two atoms
// are the ones this bubble added. The web and the phone parse this header in
// different languages, so a fixture is the only thing that keeps them honest -
// a silent disagreement about where n_joins starts would put a browser and a
// phone on different games, and the delta decides what a bubble ANIMATES.
// Regenerate with `c/build/msg_wire_test --fixture` (the odd lines).
const FIXTURES3 = [
    { n_players: 2, turn: 8, round: 2, n_new: 5, sent_at: 0x1234, hex:
      'f7030002efcdab89674523010800000200020000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7341205020004416e6e300104416e6e310800f72719e90cb7ee031bd6af74a3a23a' },
    { n_players: 3, turn: 10, round: 1, n_new: 4, sent_at: 0x1234, hex:
      'f7030002efcdab89674523010a0000030001000000000000000079d87206410d37d302c19dfb6cacbc8bebf879d242622082315709cc0f183788341204030004416e6e300104416e6e310204416e6e320a00012951da5bef3096f9f7bf2cfb58d013f6d7fa' },
    { n_players: 4, turn: 7, round: 1, n_new: 5, sent_at: 0x1234, hex:
      'f7030002efcdab89674523010700000400010000000000000000449bbad52d5dfb1bdb68d87a09fe591b9419f9f39b0ec35e9f2b75c5a359a138341205040004416e6e300104416e6e310204416e6e320304416e6e33070001c32dd6c13bd1e53963f945fef906649a' },
];

// THE FOOL'S PENALTY (c/src/msg_wire.h format 4), sealed by the native kernel
// off a deal that was PINNED to a seat the lowest-trump rule would not choose.
// `opening` is the seat the penalty imposed and `derived` the one the deal
// would otherwise have produced; they differ in every row, which is what makes
// the row worth having.
//
// This is the strongest cross-engine check in the file. The wasm kernel does
// not merely have to READ the six new header bytes at the right offsets - it
// has to re-deal from the seed with the same pin, or the body's atoms land on a
// board where they are not legal and the decode throws outright. Regenerate
// with `./build/msg_wire_test --fixture4`.
const FIXTURES4 = [
    { n_players: 2, turn: 8, round: 1, opening: 1, derived: 0, sent_at: 0x1234, hex:
      'f7040002efcdab89674523010800010200010000000000000000a5c70f6c592443dfe1944f71313133e2dd4ce13f3c125c7d892adc1dfd54e8c7341200000100000000ff020004416e6e300104416e6e31080006a02599350e83da1f57276ada' },
    { n_players: 3, turn: 7, round: 0, opening: 0, derived: 1, sent_at: 0x1234, hex:
      'f7040002efcdab89674523010700000300000000000000000000708a573c45740727bb3c8aefcd83d072866d616095beff5af7fe481591965177341200000000000000ff030004416e6e300104416e6e310204416e6e3207000214e866d4c520b973a6d192a6dcba' },
    { n_players: 4, turn: 5, round: 1, opening: 2, derived: 3, sent_at: 0x1234, hex:
      'f7040002efcdab896745230105000004000100000000000000003c4da00b32c4ca6f94e3c56e6ad66d022f8ee180ee6ba23665d2b40d26d7bb28341200000200000000ff040004416e6e300104416e6e310204416e6e320304416e6e330500a0acd23870d94d3d5f1f4a12d84494db9a' },
];

// THE cross-engine check (design §8.2). The native kernel sealed these; the wasm
// kernel must read back the same game. Divergence is a release blocker: the two
// hosts would deal or replay the same payload differently, and an iMessage game
// would fork between a phone and a browser mid-bout.
for (const f of FIXTURES) {
    test(`wasm decodes what native sealed (${f.n_players}p)`, () => {
        const env = kernelMsgDecode(hexToBytes(f.hex));
        assert.equal(env.format, 2);
        assert.equal(env.phase, 2, 'LIVE');
        assert.equal(env.n_players, f.n_players);
        // turn and round are the chain's own claims, and msg_replay only
        // returns if the body backs them — so agreeing here means the wasm
        // kernel replayed the identical chain the native one did.
        assert.equal(env.turn, f.turn, 'atom count');
        assert.equal(env.round, f.round, 'completed bouts');
        assert.equal(env.seed.length, 32);
        assert.equal(env.joins.length, f.n_players);
        assert.deepEqual(env.joins.map(j => j.seat), [...Array(f.n_players).keys()]);
        // The digest is what Rule P breaks ties on; it must be over the bytes.
        assert.equal(env.digest.length, 32);
        assert.ok(env.digest.some(b => b !== 0), 'digest computed');
    });
}

for (const f of FIXTURES3) {
    test(`wasm reads the round-16 header native wrote (${f.n_players}p)`, () => {
        const env = kernelMsgDecode(hexToBytes(f.hex));
        assert.equal(env.format, 3);
        assert.equal(env.sent_at, f.sent_at, 'send clock');
        assert.equal(env.n_new, f.n_new, 'bubble delta');
        // The three extra header bytes sit BEFORE n_joins, so reading them
        // wrong shifts every join. That the joins still come back whole is the
        // real proof the two decoders agree on the layout.
        assert.equal(env.turn, f.turn, 'atom count');
        assert.equal(env.round, f.round, 'completed bouts');
        assert.equal(env.joins.length, f.n_players);
        assert.deepEqual(env.joins.map(j => j.seat), [...Array(f.n_players).keys()]);
        assert.ok(env.joins.every(j => /^Ann\d$/.test(j.name)), 'join names intact');
        // …and the delta names a real suffix: a bubble cannot have added more
        // atoms than the chain holds.
        assert.ok(env.n_new > 0 && env.n_new <= env.turn, 'delta within the chain');
    });
}

for (const f of FIXTURES4) {
    test(`wasm honours the fool's penalty native sealed (${f.n_players}p)`, () => {
        const env = kernelMsgDecode(hexToBytes(f.hex));
        assert.equal(env.format, 4);
        assert.equal(env.opening, f.opening, 'the opening seat the penalty imposed');
        assert.notEqual(f.opening, f.derived,
            'the fixture is worthless if the penalty agreed with the deal');
        assert.equal(env.carry_key, 0, 'a live chain carries no lobby question');
        assert.equal(env.carry_fool, 0xff);
        // A decode that returned at all already re-dealt with the pin: the body
        // is coded against that board's menus and would not decode otherwise.
        // turn/round agreeing means it replayed the identical chain.
        assert.equal(env.turn, f.turn, 'atom count');
        assert.equal(env.round, f.round, 'completed bouts');
        assert.equal(env.sent_at, f.sent_at);
        assert.equal(env.joins.length, f.n_players);
        assert.deepEqual(env.joins.map(j => j.seat), [...Array(f.n_players).keys()]);
    });
}

test('the size guardrail holds on the wire, not just in theory', () => {
    // §4.4: P95 of a FULL game < 1,000 base32 chars. These are mid-game turns,
    // so they must sit well inside that.
    for (const f of FIXTURES) {
        const chars = Math.ceil(f.hex.length / 2 / 5) * 8;
        assert.ok(chars < 1000, `${f.n_players}p envelope is ${chars} base32 chars`);
    }
});
