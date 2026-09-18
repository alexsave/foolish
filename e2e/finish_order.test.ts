/* =============================================================================
 * THE FINISH ORDER: one answer, the kernel's.
 * =============================================================================
 * The end screen's ranking used to be derived in three places - c/src/anim_plan.c
 * anim_finish_rows, common_utils.calculateGameRankings and WinScreen - and the
 * two TS copies disagreed with C, because both laundered elimination_order
 * through a Set() "to handle backend bugs". The answer is now the kernel's:
 * anim_finish_rows (sdk/ts/wasm/bots.ts animFinishRows) for the end screen, and
 * table_rankings (sdk/ts/table/server_table.ts rankings) for the server's ELO.
 *
 * This drives real finished games (seeded bot tables played to completion by
 * the kernel's bot cycle, e2e/helpers/bot_table.ts) and asserts the ranking is a
 * permutation of the seats, first-out first, fool last at place == seat count,
 * and that the server ranks the table the same way.
 *
 * The absence of a dedup is the point, so the input invariant is guarded where
 * it is produced rather than laundered here: c/tests/tests.c
 * test_elimination_order_never_repeats_a_seat plays 300 random 2..6 player
 * games and insists elimination_order holds exactly the OUT seats, once each.
 *
 * Pure kernel/wasm test - needs no Postgres.
 *
 * MUTATION-CHECKED (2026-09-06), each applied, run, and reverted:
 *   animFinishRows reads the row stride as 2 bytes instead of 3
 *       -> all four tests fail
 *   the fool's place derived from the row count instead of the seat count
 *       -> "a short elimination list still puts the fool at the seat count"
 *          fails. Worth recording WHY that case exists: in a finished game the
 *          row count and the seat count are equal and every real game ranks the
 *          same either way. The short-list case was added to separate them.
 *   a dedup reinstated ahead of the kernel
 *       -> "a duplicate entry is NOT laundered" fails
 * (Phase 8 moved these off server/api/common/finish_order.ts, which took a
 * TypeScript game, onto the kernel's seats; the stride mutation was re-run.)
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { animFinishRows } from '../sdk/ts/wasm/bots.ts';
import { fixtureTable } from './helpers/table_fixture.ts';
import { dealBotTable, driveBotTable, seedBytes, type BotTableRow } from './helpers/bot_table.ts';
import { residentBoard } from './helpers/table_mem.ts';

if (!process.env.E2E_VERBOSE) {
    console.log = () => {}; console.warn = () => {}; console.error = () => {};
}

test('the kernel ranks every finished game: first out first, fool last', () => {
    let played = 0;
    for (let np = 2; np <= 5; np++) {
        for (let attempt = 0; attempt < 6; attempt++) {
            // A seeded game of random bots, dealt and played to its end by the kernel's bot cycle.
            // Random play can circle forever (attack, take, attack, take): such a game is skipped.
            let g: BotTableRow;
            try {
                g = driveBotTable(dealBotTable(Array.from({ length: np }, () => 'random'), seedBytes(np, attempt)), { maxCycles: 2000 });
            } catch (e) {
                if (/did not end/.test(String(e))) continue;
                throw e;
            }
            assert.equal(g.status, L.GAME_STATUS_GAME_OVER, `${np}p #${attempt}: the game ended`);
            played++;
            const table = fixtureTable();
            assert.equal(table.load(g.state, g.roster), L.TABLE_OK);
            const eliminated = residentBoard().eliminated;

            const places = animFinishRows(eliminated, g.fool, np, -1);
            const seats = Array.from({ length: np }, (_, i) => i);

            // Every seat gets exactly one row.
            assert.equal(places.length, np, `${np}p: a row per seat`);
            assert.deepEqual(places.map((r) => r.seat).sort((a, b) => a - b), seats,
                `${np}p: the rows are a permutation of the seats`);

            // Places are 1..n, each once - the kernel's, not a TS index.
            assert.deepEqual(places.map((r) => r.place).sort((a, b) => a - b), seats.map((s) => s + 1),
                `${np}p: kernel places are 1..n, each once`);

            // The elimination order is honoured, first out first, and the server's
            // ELO pass (table_rankings) ranks the table the same way.
            assert.deepEqual(places.slice(0, eliminated.length).map((r) => r.seat), eliminated,
                `${np}p: winners in elimination order`);
            assert.deepEqual(table.rankings(), places.map((r) => r.seat), `${np}p: table_rankings is the end screen's order`);

            // The fool - the seat the commit names - takes the last place, and
            // the last place is the SEAT count.
            const fool = places[places.length - 1];
            assert.equal(fool.seat, g.fool, `${np}p: the fool is last`);
            assert.equal(fool.place, np, `${np}p: the fool takes the last place`);
        }
    }
    assert.ok(played >= 8, `enough finished games (${played})`);
});

test('a duplicate entry is NOT laundered - the kernel is the answer', () => {
    // The old TS rankers ran Array.from(new Set(...)) here. That workaround is
    // gone, because the bug it papered over is fixed in the kernel (the was_in
    // guard at the round-end append in c/src/game.c). If a duplicate ever did
    // reach the ranking it must show, not be quietly absorbed, or the next
    // producer bug hides for another year.
    const rows = animFinishRows([1, 1], 0, 3, -1);
    // Seat 2 never appears and seat 1 appears twice: a duplicate now produces a
    // visibly wrong board instead of a quietly plausible one.
    assert.deepEqual(rows.map((r) => r.seat), [1, 1, 0], 'a repeated seat is reported as given, not deduped');
});

test("a short elimination list still puts the fool at the seat count", () => {
    // In a FINISHED game the row count and the seat count are equal, so every
    // real game ranks the same whether the fool's place comes from one or the
    // other. This is the case that separates them: four seats, only two
    // recorded out, and the fool must still take place 4 rather than 3.
    const places = animFinishRows([1, 2], 0, 4, -1);
    assert.equal(places.length, 3, 'only the ranked seats get a row');
    assert.deepEqual(places.map((r) => r.seat), [1, 2, 0], 'first out first, then the fool');
    assert.deepEqual(places.map((r) => r.place), [1, 2, 4],
        "the fool's place is the seat count, not the row count");
});

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
