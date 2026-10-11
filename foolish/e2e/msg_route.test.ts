// The /m/ route's link, minus React: base32 -> decode, through the same door
// the page's metadata opens (kernelMsgDecode), and what that door says of a
// link that was damaged on the way.
//
// What a stranger with the link is SHOWN - the table and no hand, at every
// bubble of a whole game - is the kernel's rule and is pinned beside it
// (c/tests/msg_wire_test.c test_send_accept_leg and test_damaged_link).
import test from 'node:test';
import assert from 'node:assert/strict';
import { kernelMsgDecode as decodeHeader, kernelB32Encode, kernelB32Decode } from '../sdk/ts/wasm/bots.ts';

// The envelope and its digest as one flat value: the generated header (msg_wire.h MsgHeader) holds them apart.
const kernelMsgDecode = (bytes: Uint8Array) => { const h = decodeHeader(bytes); return { ...h.e, digest: h.digest }; };

const FIXTURE = 'f7020002efcdab89674523010a0000030001000000000000000079d87206410d37d302c19dfb6cacbc8bebf879d242622082315709cc0f183788030004416e6e300104416e6e310204416e6e320a00012951da5bef3096f9f7bf2cfb58d013f6d7fa';
const bytes = Uint8Array.from(FIXTURE.match(/../g)!.map(b => parseInt(b, 16)));

test('the URL round-trips through base32 (the codec the whole product shares)', () => {
    const link = '1' + kernelB32Encode(bytes);
    assert.equal(link[0], '1', 'the text-level version dispatches before any binary');
    assert.deepEqual(kernelB32Decode(link.slice(1)), bytes);
    // QR-alphanumeric-safe and URL-safe: no padding, no case games.
    assert.match(link.slice(1), /^[A-Z2-7]+$/);
});

test('the page fits well inside MSMessage.url (5,000 chars)', () => {
    const link = `https://foolish.cards/m/1${kernelB32Encode(bytes)}`;
    assert.ok(link.length < 5000, `${link.length} chars`);
});

test('a damaged link is refused, and the refusal says why', () => {
    // A corrupted body is usually rejected outright. The body starts after the
    // 59-byte header, the joins, and the u16 count - derive it, never guess: at
    // a guessed offset this flipped a NICKNAME byte, which is legitimately valid
    // and made the test pass for the wrong reason.
    const nJoins = bytes[58];
    let bodyAt = 59;
    for (let i = 0; i < nJoins; i++) bodyAt += 2 + bytes[bodyAt + 1];
    bodyAt += 2;
    const broken = bytes.slice();
    broken[bodyAt + 1] ^= 0xff;
    assert.throws(() => kernelMsgDecode(broken), /iMessage payload/);
    // ...and the header, not the codec, is what catches most tampering.
    const badHeader = bytes.slice();
    badHeader[17] = 99;   // round: a lie the chain cannot back
    assert.throws(() => kernelMsgDecode(badHeader), /round does not match/);

    // A link cut short is refused or is a shorter chain of its own (an
    // entropy-coded body has no framing), and which of the two is the kernel's
    // to say (test_damaged_link). Here: the door throws or answers, the same
    // way twice.
    for (let cut = 1; cut <= 6; cut++) {
        const short = bytes.slice(0, bytes.length - cut);
        let turn: number;
        try { turn = kernelMsgDecode(short).turn; } catch { continue; }
        assert.equal(kernelMsgDecode(short).turn, turn, 'decode is deterministic');
    }
});
