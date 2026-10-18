/* =============================================================================
 * FOUR QUESTIONS ASKED AS VALUES: the boundary, from the TypeScript side.
 * =============================================================================
 * animStaleOptimisticOnTable, animReversalOrder, animHandLaidOut and
 * animFinishRows (sdk/ts/wasm/bots.ts) used to pack bytes into the kernel's io
 * buffer in a layout only a comment in c/wasm/wasm_api.c described. Each is now
 * a struct written through a generated writer and a struct read through a
 * generated reader (anim_plan.h AnimStaleAsk, AnimReversalAsk, AnimHandAsk,
 * AnimFinishAsk), with the converting in C.
 *
 * WHAT THE KERNEL ANSWERS is c/tests/tests.c's business (test_ask_*, which hold
 * all four to the byte entries over every value a card can take). This file
 * holds the part a native test cannot reach: that the values survive the
 * crossing - a null slot, the 64-bit deferred set as one bigint, a list at its
 * cap - and that the exported functions still answer in the shapes their
 * callers read. The reversal's and the finish order's rules have their own
 * suites (e2e/anim_plan_frames.test.ts, e2e/finish_order.test.ts and c/tests/tests.c
 * test_table_ranks_every_finished_game), and the
 * release rule has e2e/optimistic_animation.test.ts.
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
 *   animStaleOptimisticOnTable writes the table as the named list
 *       -> "the release indices..." fails
 *   animReversalOrder slices every step from 0
 *       -> "a reversal's steps..." fails
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { animHandLaidOut } from '../sdk/ts/wasm/bots.ts';
import { animReversalOrder, animStaleOptimisticOnTable } from './helpers/test_kernel_doors.ts';
import { ANIM_CONFLICT_KEEP, ANIM_CONFLICT_REVERT } from '../sdk/ts/gen/anim.bots.ts';

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

test('the release indices come back as indices into the pending cards', () => {
    const s7 = c(0, 7), h9 = c(1, 9), dA = c(3, 13), c5 = c(2, 5);
    assert.deepEqual(animStaleOptimisticOnTable([s7, h9, dA], [dA, c5, s7], [dA]), [0],
        'the 7 is on the table and unnamed; the ace is named; the 9 is not there');
    assert.deepEqual(animStaleOptimisticOnTable([s7, h9, dA], [dA, c5, s7], []), [0, 2]);
    assert.deepEqual(animStaleOptimisticOnTable([], [], []), []);
    // The lists at their caps, the last index one a byte still holds.
    const pending = Array.from({ length: 128 }, () => s7);
    const table = Array.from({ length: 160 }, () => s7);
    assert.equal(animStaleOptimisticOnTable(pending, table, table.map(() => h9)).at(-1), 127);
    assert.throws(() => animStaleOptimisticOnTable([...pending, s7], [], []), RangeError);
    assert.throws(() => animStaleOptimisticOnTable([], [...table, s7], []), RangeError);
    assert.throws(() => animStaleOptimisticOnTable([], [], [...table, s7]), RangeError);
});

test("a reversal's steps are cut from the plan's order by its step counts", () => {
    // Three groups of two, the middle one kept whole: two steps of two, last
    // group first, each holding its own motions and not the plan's first two.
    const R = ANIM_CONFLICT_REVERT, K = ANIM_CONFLICT_KEEP;
    assert.deepEqual(animReversalOrder([R, R, K, K, R, R], [2, 2, 2]), [[4, 5], [0, 1]]);
    // Every motion a plan can hold, flown as one step: 256 is a count, and a
    // group size, the byte format wrapped to nothing.
    const all = Array.from({ length: 256 }, () => R);
    const steps = animReversalOrder(all, [256]);
    assert.equal(steps.length, 1);
    assert.deepEqual(steps[0], all.map((_, i) => i));
    assert.throws(() => animReversalOrder([...all, R], [257]), RangeError);
    assert.throws(() => animReversalOrder([R], [1, -1]), /anim_reversal_order error/);
});
