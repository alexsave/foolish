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
// test_client_move_wire_is_the_old_encoders_bytes). The last test holds the
// client's request and the stale-round response to the server's own codec, the
// C Table's (table_request_decode, table_action_response), over the shipped
// bots.wasm. Needs no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { AWIRE_KIND, AwireKindName, AwireMove, encodeAction, clientTable } from '../sdk/ts/table/client_table.ts';
import { TABLE_STATUS_APPLIED, TABLE_STATUS_MOOT, TABLE_STATUS_REJECTED, TABLE_REJECT_STALE_ROUND, AWIRE_MAX_CARDS } from '../sdk/ts/gen/view_layout.bots.ts';
import { createServerTable } from './helpers/server_table.ts';
import { TABLE_APPLIED, TABLE_MOOT, TABLE_REJECTED, TABLE_STALE_ROUND } from '../sdk/ts/gen/game_layout.bots.ts';

type Card = NonNullable<AwireMove['cards']>[number];

// Deterministic RNG so a failure reproduces from the printed seed.
let seed = Number(process.env.FUZZ_SEED || 0xa11ce) >>> 0;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 0x100000000; };
const ri = (n: number) => Math.floor(rnd() * n);
const pick = <T>(a: T[]): T => a[ri(a.length)];

const KINDS = Object.keys(AWIRE_KIND) as AwireKindName[];
const randCard = (): Card => ({ suit: ri(4), value: 1 + ri(13) });
const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');

const randValidMove = (): AwireMove => {
    const kind = pick(KINDS);
    if (kind === 'pickup' || kind === 'good') return { kind, cards: [] };
    const n = 1 + ri(AWIRE_MAX_CARDS);
    const move: AwireMove = { kind, cards: Array.from({ length: n }, randCard) };
    if (kind === 'cover') move.attack_cards = Array.from({ length: n }, randCard);
    return move;
};

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
// The `action` endpoint's bodies. The request: v1 is the unguarded form, v2
// adds the client-intent round guard's u32 intent_version. Both ends of each
// body are the kernel's (c/src/table.h): the client's writer and reader are
// reached through the client slot, the server's through the C Table. The
// bytes themselves are pinned in c/tests/tests.c
// (test_table_request_encode_and_response_decode); here the two ends meet
// across the TypeScript that carries them.
// ---------------------------------------------------------------------------
test('action request: what the client writes, with and without an intent, the server reads back', () => {
    const table = createServerTable();
    for (let i = 0; i < 500; i++) {
        const gid = `g${ri(1e9).toString(36)}`;
        const wire = encodeAction(randValidMove());

        // v1 - no intent version.
        const d1 = table.requestDecode(clientTable().actionRequest(gid, wire));
        assert.ok(typeof d1 !== 'number', `v1 decodes (${d1})`);
        assert.equal(d1.gameId, gid, 'v1 game id');
        assert.equal(hex(d1.wire), hex(wire), 'v1 wire preserved');
        assert.equal(d1.intent, null, 'v1 carries no intent version (unguarded)');

        // v2 - with a u32 intent version (exercise the full u32 range incl. > 2^31).
        const iv = (ri(0xffff) * 0x10001) >>> 0; // spread across all 4 bytes
        const d2 = table.requestDecode(clientTable().actionRequest(gid, wire, iv));
        assert.ok(typeof d2 !== 'number', `v2 decodes (${d2})`);
        assert.equal(d2.gameId, gid, 'v2 game id');
        assert.equal(hex(d2.wire), hex(wire), 'v2 wire preserved after the intent field');
        assert.equal(d2.intent, iv, 'v2 intent version round-trips exactly (u32)');
    }
    // intent_version 0 is a real value, not "absent" - the u32 must still ride.
    const z = table.requestDecode(clientTable().actionRequest('gg', encodeAction({ kind: 'good' }), 0));
    assert.ok(typeof z !== 'number');
    assert.equal(z.intent, 0, 'intent version 0 is preserved as 0, not dropped');

    for (const gid of ['a', 'd79ae3', 'x'.repeat(64), '\u00e9\u2603']) {
        for (const wire of [Uint8Array.of(3, 0), Uint8Array.of(0, 1, 7), Uint8Array.of(1, 2, 8, 9, 10, 11)]) {
            for (const intent of [undefined, 0, 5, 0x7fffffff, 0xffffffff]) {
                const c = table.requestDecode(clientTable().actionRequest(gid, wire, intent));
                assert.ok(typeof c !== 'number', `${gid}/${hex(wire)}/${intent}: the server decodes it (${c})`);
                assert.deepEqual({ gameId: c.gameId, wire: hex(c.wire), intent: c.intent },
                    { gameId: gid, wire: hex(wire), intent: intent ?? null }, `${gid}/${hex(wire)}/${intent}`);
            }
        }
    }
    // A game id its length byte cannot say is a client bug, and thrown as one.
    assert.throws(() => clientTable().actionRequest('x'.repeat(256), Uint8Array.of(3, 0)), /action request: refused/);
    assert.equal(clientTable().actionRequest('x'.repeat(255), Uint8Array.of(3, 0), 1).length, 2 + 255 + 4 + 2, 'the longest id still fits');
});

test('action request: malformed and truncated bodies are refused by the server (never thrown, never partly read)', () => {
    const table = createServerTable();
    for (const [label, bad] of [
        ['empty', Uint8Array.of()],
        ['one byte', Uint8Array.of(1)],
        ['an unknown format byte', Uint8Array.of(9, 0, 3, 0)],
        ['v2 with a game id and an intent but no wire', Uint8Array.of(2, 2, 65, 66, 0, 0, 0, 0)],
        ['v2 cut inside its intent', Uint8Array.of(2, 1, 0x67, 1, 0, 0)],
        ['v1 with a game id but no wire', Uint8Array.of(1, 2, 65, 66)],
    ] as const) {
        const read = table.requestDecode(bad);
        assert.ok(typeof read === 'number' && read < 0, `${label} (${hex(bad)}) is refused`);
    }
});

test('action response: what the server writes for each result is what the client reads', () => {
    const table = createServerTable();
    for (const version of [0, 1, 256, 0x01020304, 0x7fffffff, 0xffffffff]) {
        for (const [result, reject, status, rejectCode] of [
            [TABLE_APPLIED, 0, TABLE_STATUS_APPLIED, 0],
            [TABLE_REJECTED, 13, TABLE_STATUS_REJECTED, 13],
            [TABLE_STALE_ROUND, 0, TABLE_STATUS_REJECTED, TABLE_REJECT_STALE_ROUND],
            [TABLE_MOOT, 0, TABLE_STATUS_MOOT, 0],
        ] as const) {
            assert.deepEqual(clientTable().readActionResponse(table.actionResponse(result, reject, version)),
                { status, rejectCode, version }, `result ${result} at ${version}`);
        }
    }
    assert.equal(clientTable().readActionResponse(Uint8Array.of()), null, 'an empty body is not a response');
    assert.equal(clientTable().readActionResponse(table.actionResponse(TABLE_APPLIED, 0, 7).subarray(0, 6)), null, 'a short body is not a response');
    const other = table.actionResponse(TABLE_APPLIED, 0, 7);
    other[0] = 9;
    assert.equal(clientTable().readActionResponse(other), null, 'a body of another format is not a response');
});
