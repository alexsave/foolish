// FMSG — the iMessage envelope, through the real kernel wasm.
//
// The native suite (cnitro/tests/msg_wire_test.c) is where the codec is proven;
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
import { kernelMsgDecode } from '../supabase/functions/_shared/wasm/bots.ts';

// Sealed by the NATIVE producer (`cnitro/build/msg_wire_test --fixture`) from
// seeded games, cut mid-game — a turn bubble, which is what actually ships.
// Regenerate with that command if the envelope layout ever changes; a diff here
// is a wire break, and every shipped device would have to agree with it.
const FIXTURES = [
    { n_players: 2, turn: 7, round: 1, hex:
      'f7020002efcdab89674523010700000200010000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e31070003a9cc795118a16a9edd28d516' },
    { n_players: 3, turn: 10, round: 1, hex:
      'f7020002efcdab89674523010a0000030001000000000000000079d87206410d37d302c19dfb6cacbc8bebf879d242622082315709cc0f183788030004416e6e300104416e6e310204416e6e320a00012cb4fce6acbe29ba5d0adae18a66b4fdc7f6' },
    { n_players: 4, turn: 5, round: 1, hex:
      'f7020002efcdab89674523010500000400010000000000000000449bbad52d5dfb1bdb68d87a09fe591b9419f9f39b0ec35e9f2b75c5a359a138040004416e6e300104416e6e310204416e6e320304416e6e33050003b7ddc3ef88a264acb5183fbe413a46' },
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
    bad(b => { b[1] = 3; }, /unsupported format/);
    bad(b => { b[2] = 0x01; }, /unsupported flags/);    // fair-deal: spec'd, unbuilt
    bad(b => { b[2] = 0x04; }, /unsupported flags/);    // reserved bit
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

test('the size guardrail holds on the wire, not just in theory', () => {
    // §4.4: P95 of a FULL game < 1,000 base32 chars. These are mid-game turns,
    // so they must sit well inside that.
    for (const f of FIXTURES) {
        const chars = Math.ceil(f.hex.length / 2 / 5) * 8;
        assert.ok(chars < 1000, `${f.n_players}p envelope is ${chars} base32 chars`);
    }
});
