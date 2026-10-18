// The action wire ("awire" v1) as TypeScript reaches it: sdk/ts/table/client_table.ts
// hands a move to the kernel (client_move_wire, c/src/client_table.h) and
// awire.c writes the bytes. That file used to BE an encoder and a decoder of
// its own, and this one fuzzed the two TypeScript halves against each other.
// There is one implementation now, so what is held here is that it did not
// move: the frames below were captured from the TypeScript encoder before it
// was deleted, and the kernel has to write every one of them byte for byte.
// The decoder's strictness and the round trip of every stateable move are
// asked of awire.c itself (c/tests/tests.c
// test_awire_decode_refuses_every_malformed_shape,
// test_client_move_wire_is_the_old_encoders_bytes), and the server's end of the
// action bodies by test_action_request_door. What stays here is the TypeScript
// that wraps the client's halves (sdk/ts/table/client_table.ts) over web.wasm,
// the module a visitor runs. Needs no Postgres and no test build.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { AwireKindName, AwireMove, encodeAction, clientTable } from '../sdk/ts/table/client_table.ts';
import { TABLE_STATUS_APPLIED, TABLE_STATUS_MOOT, TABLE_STATUS_REJECTED, TABLE_REJECT_STALE_ROUND, AWIRE_MAX_CARDS } from '../sdk/ts/gen/view_layout.bots.ts';

type Card = NonNullable<AwireMove['cards']>[number];

// Deterministic RNG so a failure reproduces from the printed seed.
let seed = Number(process.env.FUZZ_SEED || 0xa11ce) >>> 0;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);

const randCard = (): Card => ({ suit: ri(4), value: 1 + ri(13) });
const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');

// Every frame the TypeScript encoder wrote for these moves, as hex, captured
// before it was deleted. The deck in id order names the long ones.
const c = (suit: number, value: number): Card => ({ suit, value });
const DECK = Array.from({ length: 52 }, (_, i) => c(Math.floor(i / 13), (i % 13) + 1));
const GOLDEN: [string, AwireMove, string][] = [
    ['pickup', { kind: 'pickup' }, '0300'],
    ['good', { kind: 'good' }, '0400'],
    ['an attack of no card', { kind: 'attack', cards: [] }, '0000'],
    ['the first card', { kind: 'attack', cards: [c(0, 1)] }, '000100'],
    ['the ace of diamonds', { kind: 'attack', cards: [c(3, 13)] }, '000133'],
    ['three attacks', { kind: 'attack', cards: [c(2, 9), c(1, 9), c(0, 9)] }, '0003221508'],
    ['an attack of 28', { kind: 'attack', cards: DECK.slice(0, 28) }, '001c000102030405060708090a0b0c0d0e0f101112131415161718191a1b'],
    ['a pass of two', { kind: 'pass', cards: [c(1, 6), c(3, 6)] }, '0202122c'],
    ['a pass of 28', { kind: 'pass', cards: DECK.slice(24, 52) }, '021c18191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233'],
    ['a cover', { kind: 'cover', cards: [c(0, 13)], attack_cards: [c(0, 12)] }, '01010c0b'],
    ['three covers', { kind: 'cover', cards: [c(3, 2), c(1, 11), c(2, 5)], attack_cards: [c(0, 8), c(1, 10), c(2, 4)] }, '010328171e07161d'],
    ['a cover of 26', { kind: 'cover', cards: DECK.slice(0, 26), attack_cards: DECK.slice(26, 52) },
        '011a000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233'],
    ['a cover of 28', { kind: 'cover', cards: DECK.slice(0, 28), attack_cards: DECK.slice(24, 52) },
        '011c000102030405060708090a0b0c0d0e0f101112131415161718191a1b18191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233'],
    ['attacks named beside an attack are not read', { kind: 'attack', cards: [c(1, 1)], attack_cards: [c(2, 2)] }, '00010d'],
];

test('awire: the kernel writes every frame the TypeScript encoder wrote, byte for byte', () => {
    for (const [label, move, want] of GOLDEN) {
        assert.equal(hex(encodeAction(move)), want, label);
    }
    // A move written twice is the same bytes, and a second move does not disturb
    // the first one's: each call hands back its own copy out of the module.
    const first = encodeAction(GOLDEN[10][1]);
    const second = encodeAction(GOLDEN[5][1]);
    assert.equal(hex(first), GOLDEN[10][2], 'the first wire is still its own after the next move was written');
    assert.equal(hex(second), GOLDEN[5][2]);
});

test('awire: encodeAction refuses unencodable moves (client bugs, not race conditions)', () => {
    assert.throws(() => encodeAction({ kind: 'bogus' as AwireKindName, cards: [] }), /unknown kind/);
    assert.throws(() => encodeAction({ kind: 'attack', cards: Array.from({ length: AWIRE_MAX_CARDS + 1 }, randCard) }), /do not fit/);
    assert.throws(() => encodeAction({ kind: 'pickup', cards: [randCard()] }), /not a move/);
    assert.throws(() => encodeAction({ kind: 'good', cards: [randCard()] }), /not a move/);
    assert.throws(() => encodeAction({ kind: 'cover', cards: [randCard(), randCard()], attack_cards: [randCard()] }), /not a move/);
    assert.throws(() => encodeAction({ kind: 'cover', cards: [randCard()], attack_cards: [randCard(), randCard()] }), /not a move/);
    // A card that is not a card is refused, never written as some other card:
    // the hidden card, no card, a value below the deck and one above it.
    for (const bad of [c(-1, -1), c(-2, -2), c(0, 0), c(0, 14)]) {
        assert.throws(() => encodeAction({ kind: 'attack', cards: [c(0, 5), bad] }), /not a move/, JSON.stringify(bad));
        assert.throws(() => encodeAction({ kind: 'cover', cards: [c(0, 5)], attack_cards: [bad] }), /not a move/, JSON.stringify(bad));
    }
    // A refusal leaves nothing behind: the next move is written whole.
    assert.equal(hex(encodeAction({ kind: 'pass', cards: [c(1, 6), c(3, 6)] })), '0202122c');
});

// ---------------------------------------------------------------------------
// The `action` endpoint's bodies, as the web reaches them: the client's writer
// of the request and reader of the response (client_table.ts actionRequest,
// readActionResponse over web.wasm). The bytes are the ones pinned natively
// (c/tests/tests.c test_table_request_encode_and_response_decode), where the
// server's halves read and write them too (test_table_request_and_response,
// test_action_request_door).
// ---------------------------------------------------------------------------
test('action request: the client writes the pinned bytes, with and without an intent', () => {
    const cover = Uint8Array.of(0x01, 0x02, 0x28, 0x17, 0x07, 0x16);
    for (const [gid, intent, head] of [
        ['a', undefined, '010161'],
        ['a', 0, '02016100000000'],
        ['a', 5, '02016105000000'],
        ['a', 0x01020304, '02016104030201'],
        ['a', 0x7fffffff, '020161ffffff7f'],
        ['a', 0xffffffff, '020161ffffffff'],
        ['d79ae3', undefined, '0106643739616533'],
        ['d79ae3', 0x01020304, '020664373961653304030201'],
        ['\u00e9\u2603', undefined, '0105c3a9e29883'],
        ['\u00e9\u2603', 5, '0205c3a9e2988305000000'],
    ] as const) {
        assert.equal(hex(clientTable().actionRequest(gid, cover, intent)), head + hex(cover), `${gid} / ${intent}`);
    }
    // A game id its length byte cannot say is a client bug, and thrown as one.
    assert.throws(() => clientTable().actionRequest('x'.repeat(256), Uint8Array.of(3, 0)), /action request: refused/);
    assert.equal(clientTable().actionRequest('x'.repeat(255), Uint8Array.of(3, 0), 1).length, 2 + 255 + 4 + 2, 'the longest id still fits');
});

test('action response: the client reads the pinned bytes, and nothing that is not a response', () => {
    for (const [bytes, want] of [
        ['01000000000000', { status: TABLE_STATUS_APPLIED, rejectCode: 0, version: 0 }],
        ['01010d29000000', { status: TABLE_STATUS_REJECTED, rejectCode: 13, version: 41 }],
        ['010200ffffff7f', { status: TABLE_STATUS_MOOT, rejectCode: 0, version: 0x7fffffff }],
        ['010000ffffffff', { status: TABLE_STATUS_APPLIED, rejectCode: 0, version: 0xffffffff }],
        ['01016404030201', { status: TABLE_STATUS_REJECTED, rejectCode: TABLE_REJECT_STALE_ROUND, version: 0x01020304 }],
    ] as const) {
        assert.deepEqual(clientTable().readActionResponse(Buffer.from(bytes, 'hex')), want, bytes);
    }
    assert.equal(clientTable().readActionResponse(Uint8Array.of()), null, 'an empty body is not a response');
    assert.equal(clientTable().readActionResponse(Buffer.from('010000000000', 'hex')), null, 'a short body is not a response');
    assert.equal(clientTable().readActionResponse(Buffer.from('09000007000000', 'hex')), null, 'a body of another format is not a response');
});
