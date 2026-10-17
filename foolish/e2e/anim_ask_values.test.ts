/* =============================================================================
 * THE HAND'S ORDER ASKED AS A VALUE: the boundary, from the TypeScript side.
 * =============================================================================
 * animHandLaidOut (sdk/ts/wasm/bots.ts) used to pack bytes into the kernel's io
 * buffer in a layout only a comment in c/wasm/wasm_api.c described. It is now a
 * struct written through a generated writer and a struct read through a
 * generated reader (anim_plan.h AnimHandAsk, AnimHandLaid), with the converting
 * in C.
 *
 * WHAT THE KERNEL ANSWERS is c/tests/tests.c's business (test_ask_*, which hold
 * the asks to the byte entries over every value a card can take, and
 * test_reversal_steps_are_whole_groups_last_first for the reversal's steps).
 * This file holds the part a native test cannot reach, on the module the
 * browser loads (web.wasm): that the values survive the crossing - a null slot,
 * the 64-bit deferred set as one bigint, a list at its cap - and that the
 * exported function still answers in the shape its callers read. The stale and
 * reversal asks were here too; the browser reaches that C through
 * client_push_settle, never through a door of its own, so they live in C only.
 *
 * Pure kernel/wasm test - needs no Postgres.
 *
 * MUTATION-CHECKED (2026-10-10), each applied to sdk/ts/wasm/bots.ts, run, and
 * put back:
 *   animHandLaidOut writes a null slot as the hidden card, not NO_CARD
 *       -> "a slot nobody can name..." and "a whole hand..." fail
 *   animHandLaidOut hands a back out as the card it was read as (no null)
 *       -> the same two fail
 *   animHandLaidOut writes deferred as 0n
 *       -> "the deferred set crosses as one 64-bit value" fails
 *   animHandLaidOut writes `cards` as the order too
 *       -> all three hand tests fail
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { animHandLaidOut } from '../sdk/ts/wasm/bots.ts';

interface Card { suit: number; value: number }
const c = (suit: number, value: number): Card => ({ suit, value });
// The kernel's dense id (card.h card_to_id), only to aim a bit of the deferred
// set at a card - the set is the one thing here that is spelled in ids.
const bit = (card: Card) => 1n << BigInt(card.suit * 13 + card.value - 1);

test('a slot nobody can name crosses as a back and comes home as null', () => {
    const hand = [c(1, 7), null, null, c(0, 9)];
    assert.deepEqual(animHandLaidOut(hand, []), hand, 'with no arrangement the hand is the kernel order');
    // Backs are reconciled by count, known cards by identity; the third back
    // and the queen the hand does not hold fall out, the 7 appends.
    assert.deepEqual(
        animHandLaidOut(hand, [null, c(0, 9), null, null, c(3, 11)]),
        [null, c(0, 9), null, c(1, 7)]);
    // The hidden card is not a back: it has no identity and no count either.
    assert.deepEqual(animHandLaidOut([c(-1, -1), c(1, 7)], []), [c(1, 7)]);
});

test('the deferred set crosses as one 64-bit value', () => {
    // The two of diamonds is id 39: a bit the low 32 do not have, which is the
    // half a narrowed set would lose.
    const d2 = c(3, 1), h7 = c(1, 7), s9 = c(0, 9);
    assert.ok(bit(d2) > 0xffffffffn, 'the fixture is in the high half');
    assert.deepEqual(animHandLaidOut([h7, d2, s9], [d2]), [d2, h7, s9], 'not deferred: the order leads with it');
    assert.deepEqual(animHandLaidOut([h7, d2, s9], [d2], bit(d2)), [h7, s9], 'deferred: it holds no slot');
    assert.deepEqual(animHandLaidOut([h7, d2, s9], [d2], bit(s9)), [d2, h7], 'and a low bit still does');
});

test('a whole hand is answered, and one card more is refused by the writer', () => {
    // Every card of the deck and a fill of backs: the kernel's own hand cap,
    // which is what the generated writer holds a hand to.
    const whole: (Card | null)[] = [];
    for (let s = 0; s < 4; s++) for (let v = 1; v <= 13; v++) whole.push(c(s, v));
    while (whole.length < 64) whole.push(null);
    const reversed = [...whole].reverse();
    assert.deepEqual(animHandLaidOut(whole, reversed), reversed);
    assert.throws(() => animHandLaidOut([...whole, null], []), RangeError);
    assert.throws(() => animHandLaidOut([], [...whole, null]), RangeError);
});
