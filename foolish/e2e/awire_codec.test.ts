// The action wire ("awire" v1) as TypeScript reaches it: sdk/ts/wire/awire.ts
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

import {
    AWIRE_KIND, AWIRE_MAX_CARDS, AwireKindName, AwireMove,
    ACTION_REQ_FORMAT, ACTION_REQ_FORMAT_V1,
    encodeAction, encodeActionRequest, decodeActionRequest,
    encodeActionResponse, ACTION_STATUS, REJECT_STALE_ROUND,
} from '../sdk/ts/wire/awire.ts';
import { createServerTable } from '../sdk/ts/table/server_table.ts';
import { TABLE_STALE_ROUND } from '../sdk/ts/gen/game_layout.bots.ts';

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
// Request envelope (the `action` edge function's binary body). v1 =
// [fmt|gid_len|gid|wire]; v2 adds the client-intent round guard's u32
// intent_version. An old client sends v1 (unguarded); a new client sends v2.
// ---------------------------------------------------------------------------
test('awire request envelope: v1 (no intent) and v2 (intent version) round-trip; decoder reads both', () => {
    for (let i = 0; i < 500; i++) {
        const gid = `g${ri(1e9).toString(36)}`;
        const wire = encodeAction(randValidMove());

        // v1 — no intent version.
        const v1 = encodeActionRequest(gid, wire);
        assert.equal(v1[0], ACTION_REQ_FORMAT_V1, 'v1 format byte');
        const d1 = decodeActionRequest(v1);
        assert.ok(d1, 'v1 decodes');
        assert.equal(d1!.gameId, gid, 'v1 game id');
        assert.deepEqual([...d1!.wire], [...wire], 'v1 wire preserved');
        assert.equal(d1!.intentVersion, undefined, 'v1 carries no intent version (unguarded)');

        // v2 — with a u32 intent version (exercise the full u32 range incl. > 2^31).
        const iv = (ri(0xffff) * 0x10001) >>> 0; // spread across all 4 bytes
        const v2 = encodeActionRequest(gid, wire, iv);
        assert.equal(v2[0], ACTION_REQ_FORMAT, 'v2 format byte');
        const d2 = decodeActionRequest(v2);
        assert.ok(d2, 'v2 decodes');
        assert.equal(d2!.gameId, gid, 'v2 game id');
        assert.deepEqual([...d2!.wire], [...wire], 'v2 wire preserved after the intent field');
        assert.equal(d2!.intentVersion, iv, 'v2 intent version round-trips exactly (u32)');
    }
    // intent_version 0 is a real value, not "absent" — the u32 must still ride.
    const z = decodeActionRequest(encodeActionRequest('gg', encodeAction({ kind: 'good' }), 0));
    assert.equal(z!.intentVersion, 0, 'intent version 0 is preserved as 0, not dropped');
});

test('awire request envelope: malformed/truncated buffers decode to null (never throw, never partial)', () => {
    assert.equal(decodeActionRequest(new Uint8Array([])), null, 'empty');
    assert.equal(decodeActionRequest(new Uint8Array([9, 0])), null, 'unknown format byte');
    // v2 header claiming a gid + intent but truncated before the 2-byte wire.
    assert.equal(decodeActionRequest(new Uint8Array([ACTION_REQ_FORMAT, 2, 65, 66, 0, 0, 0, 0])), null, 'v2 missing wire');
    // v1 with a gid but no wire.
    assert.equal(decodeActionRequest(new Uint8Array([ACTION_REQ_FORMAT_V1, 2, 65, 66])), null, 'v1 missing wire');
});

test('the client\'s request is what the server\'s C codec reads, and its stale-round response is the client\'s bytes', () => {
    const table = createServerTable();
    const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');
    const wires = [Uint8Array.of(3, 0), Uint8Array.of(0, 1, 7), Uint8Array.of(1, 2, 8, 9, 10, 11)];
    for (const gid of ['a', 'd79ae3', 'x'.repeat(64)]) {
        for (const wire of wires) {
            for (const intent of [undefined, 0, 5, 0x7fffffff, 0xffffffff]) {
                const body = encodeActionRequest(gid, wire, intent);
                const c = table.requestDecode(body);
                assert.ok(typeof c !== 'number', `${gid}/${hex(wire)}/${intent}: the server decodes it (${c})`);
                assert.deepEqual({ gameId: c.gameId, wire: hex(c.wire), intent: c.intent },
                    { gameId: gid, wire: hex(wire), intent: intent ?? null }, `${gid}/${hex(wire)}/${intent}`);
            }
        }
    }
    for (const bad of [Uint8Array.of(), Uint8Array.of(1), Uint8Array.of(2, 1, 0x67, 1, 0, 0), Uint8Array.of(9, 0, 3, 0)]) {
        assert.equal(decodeActionRequest(bad), null, `the client's reader refuses ${hex(bad)}`);
        assert.equal(typeof table.requestDecode(bad), 'number', `the server refuses ${hex(bad)}`);
    }
    for (const version of [0, 1, 256, 0x01020304, 0x7fffffff]) {
        assert.equal(hex(table.actionResponse(TABLE_STALE_ROUND, 0, version)),
            hex(encodeActionResponse(ACTION_STATUS.REJECTED, REJECT_STALE_ROUND, version)), `stale round at ${version}`);
    }
});
