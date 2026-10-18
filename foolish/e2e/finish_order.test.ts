/* =============================================================================
 * THE FINISH ORDER through the web's shim: the kernel's rows, unchanged.
 * =============================================================================
 * The end screen's ranking is the kernel's (c/src/anim_plan.c anim_finish_rows),
 * reached from the browser through sdk/ts/wasm/bots.ts animFinishRows over
 * web.wasm. What the ranking IS - a permutation of the seats, first out first,
 * the fool last at place == the seat count, the server's table_rankings the
 * same order, a duplicate never laundered, a short list's fool still at the
 * seat count - is the kernel's, and is held natively: c/tests/tests.c
 * test_table_ranks_every_finished_game and
 * test_board_finish_rows_rank_first_out_to_the_fool. What stays here is the
 * marshalling: that the shim carries the C row layout across the boundary
 * without a slip a game that happens to rank the same either way could hide.
 *
 * MUTATION-CHECKED (2026-09-06): animFinishRows reading the row stride as 2
 * bytes instead of 3 fails this test.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { animFinishRows } from '../sdk/ts/wasm/bots.ts';

test('the shim carries the kernel row layout unchanged', () => {
    // Straight against the C contract in anim_plan.h, so a marshalling slip in
    // sdk/ts/wasm/bots.ts animFinishRows cannot hide behind a game that happens
    // to rank the same either way.
    const rows = animFinishRows([2, 0, 3], 1, 4, 3);
    assert.deepEqual(rows, [
        { place: 1, seat: 2, isYou: false },
        { place: 2, seat: 0, isYou: false },
        { place: 3, seat: 3, isYou: true },
        { place: 4, seat: 1, isYou: false },
    ], 'places, seats and the viewer row survive the boundary');

    // A running game (negative game_over) emits no last-place row.
    assert.equal(animFinishRows([2, 0], -1, 4, 0).length, 2, 'no fool row while the game runs');

    // A spectator owns no row.
    assert.ok(animFinishRows([2, 0, 3], 1, 4, -1).every((r) => !r.isYou),
        'a spectator owns no row');

    // The fool's place is the SEAT count, not the row count.
    const short = animFinishRows([0], 1, 4, -1);
    assert.equal(short.length, 2, 'a short list still ranks the fool');
    assert.equal(short[1].place, 4, "the fool's place is the seat count");
});
