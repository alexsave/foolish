/* =============================================================================
 * THE FINISH ORDER: one answer, the kernel's.
 * =============================================================================
 * The end screen's ranking used to be derived in three places - c/src/anim_plan.c
 * anim_finish_rows, common_utils.calculateGameRankings and WinScreen - and the
 * two TS copies disagreed with C, because both laundered elimination_order
 * through a Set() "to handle backend bugs". server/api/common/finish_order.ts
 * is now the only TS side and it asks the kernel.
 *
 * This drives real finished games (seeded, played to completion through the
 * kernel) and asserts the ranking is a permutation of the seats, first-out
 * first, fool last at place == seat count.
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
 *   finish_order.ts derives the fool's place from the row count instead of the
 *   seat count (passing elimSeats.length + 1 as n_players)
 *       -> "a short elimination list still puts the fool at the seat count"
 *          fails. Worth recording WHY that case exists: a first attempt at this
 *          mutation passed everything, because in a finished game the row count
 *          and the seat count are equal and every real game ranks the same
 *          either way. The short-list case was added to separate them.
 *   finish_order.ts reinstates the Array.from(new Set(...)) dedup
 *       -> "a duplicate entry is NOT laundered" fails
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { start_game } from '../server/api/common/game_lifecycle.ts';
import { __setDealSeedOverride } from '../sdk/ts/wasm/engine.ts';
import { game_done } from '../server/api/common/common_utils.ts';
import { calculateLegalMoves } from '../server/api/common/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../server/api/common/pure_bot_actions.ts';
import { gameFinishPlaces, calculateGameRankings } from '../server/api/common/finish_order.ts';
import { animFinishRows } from '../sdk/ts/wasm/bots.ts';
import {
    Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../server/api/core/types.ts';

if (!process.env.E2E_VERBOSE) {
    console.log = () => {}; console.warn = () => {}; console.error = () => {};
}

const mkPlayer = (i: number): PrivatePlayer => ({
    player_id: `seat-${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
    hand: [], awaiting_attack: false, hand_length: 0,
    strategy_key: STRATEGY_KEY.RANDOM,
});

const mkGame = (np: number, gameSeed?: string): Game => ({
    players: Array.from({ length: np }, (_, i) => mkPlayer(i)),
    deck: [], logs: [], id: 'fo', name: 'fo', status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
    first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [], game_seed: gameSeed ?? null,
} as unknown as Game);

const seedBytes = (np: number, s: number): Uint8Array =>
    new Uint8Array(Array.from({ length: 32 }, (_, i) => (i * 31 + s * 13 + np) & 0xff));
const seedHex = (b: Uint8Array) =>
    Array.from(b).map((x) => x.toString(16).padStart(2, '0')).join('');

/** Play a seeded game to completion through the kernel, or null if it stalls.
 *  Mirrors e2e/helpers/seeded_game.ts, minus the replay encode this does not
 *  need - including the deal-seed override, without which the deal and the
 *  kernel disagree. */
async function playToEnd(np: number, s: number): Promise<Game | null> {
    const seed = seedBytes(np, s);
    const game = mkGame(np, seedHex(seed));
    __setDealSeedOverride(seed);
    try {
        start_game(game);
        for (let guard = 0; guard < 20000 && game_done(game) === null; guard++) {
            let acted = false;
            for (let i = 0; i < game.players.length && !acted; i++) {
                const p = game.players[i];
                if (!shouldBotActCore(game, p, i)) continue;
                if (calculateLegalMoves(game, p.player_id).length === 0) continue;
                acted = await processBotAction(game, p);
            }
            if (!acted) return null;
        }
    } finally {
        __setDealSeedOverride(null);
    }
    return game_done(game) !== null ? game : null;
}

test('the kernel ranks every finished game: first out first, fool last', async () => {
    let played = 0;
    for (let np = 2; np <= 5; np++) {
        for (let attempt = 0; attempt < 6; attempt++) {
            const g = await playToEnd(np, attempt);
            if (!g) continue;
            played++;

            const places = gameFinishPlaces(g);
            const ids = g.players.map((p) => p.player_id);

            // Every seat gets exactly one row.
            assert.equal(places.length, np, `${np}p: a row per seat`);
            assert.deepEqual(
                [...places.map((r) => r.player_id)].sort(),
                [...ids].sort(),
                `${np}p: the rows are a permutation of the seats`);

            // Places are 1..n, each once - the kernel's, not a TS index.
            assert.deepEqual(
                places.map((r) => r.place).sort((a, b) => a - b),
                Array.from({ length: np }, (_, i) => i + 1),
                `${np}p: kernel places are 1..n, each once`);

            // The elimination order is honoured, first out first.
            const rankings = calculateGameRankings(g);
            assert.deepEqual(
                rankings.slice(0, g.elimination_order.length),
                g.elimination_order,
                `${np}p: winners in elimination order`);

            // The fool - the seat game_done names - takes the last place, and
            // the last place is the SEAT count.
            const fool = places[places.length - 1];
            assert.equal(fool.player_id, game_done(g), `${np}p: the fool is last`);
            assert.equal(fool.place, np, `${np}p: the fool takes the last place`);
        }
    }
    assert.ok(played >= 8, `enough finished games (${played})`);
});

test('a duplicate entry is NOT laundered - the kernel is the answer', () => {
    // The old TS rankers ran Array.from(new Set(...)) here. That workaround is
    // gone, because the bug it papered over is fixed in the kernel (the was_in
    // guard at the round-end append in c/src/game.c). If a duplicate ever did
    // reach this function it must show, not be quietly absorbed, or the next
    // producer bug hides for another year.
    const g = mkGame(3);
    g.elimination_order = ['seat-1', 'seat-1'];
    const rankings = calculateGameRankings(g);
    // Seat 2 never appears and seat 1 appears twice: a duplicate now produces a
    // visibly wrong board instead of a quietly plausible one.
    assert.deepEqual(rankings, ['seat-1', 'seat-1', 'seat-0'],
        'a repeated seat is reported as given, not deduped');
});

test("a short elimination list still puts the fool at the seat count", () => {
    // In a FINISHED game the row count and the seat count are equal, so every
    // real game ranks the same whether the fool's place comes from one or the
    // other. This is the case that separates them: four seats, only two
    // recorded out, and the fool must still take place 4 rather than 3.
    const g = mkGame(4);
    g.elimination_order = ['seat-1', 'seat-2'];
    const places = gameFinishPlaces(g);
    assert.equal(places.length, 3, 'only the ranked seats get a row');
    assert.deepEqual(places.map((r) => r.player_id), ['seat-1', 'seat-2', 'seat-0'],
        'first out first, then the fool');
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
