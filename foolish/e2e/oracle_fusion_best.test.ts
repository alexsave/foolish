/* =============================================================================
 * Infinite Oracle - the panel shows fusion's noisy race, and Best is fusion's
 * pick and every row tied with it (docs/INFINITE_ORACLE_DESIGN.md "Ties, Best
 * and the classes")
 * =============================================================================
 * The kernel (c/src/oracle_rank.c, which states the whole rule) ranks the rows
 * for both modes. Every number it hands back - the EF, its error, the bar, the
 * class, the ties and the order - is fusion's NOISY race, the race fusion's
 * pick comes from, so Best, the classes and the numbers cannot disagree.
 * Fusion's pick is octogen's (the lowest noise-free mean) unless the noisy
 * race's pick is better, mean plus trump tax, by more than FUSION_SHADOW tenths
 * of the two rows' combined standard error; every row whose noisy number ties
 * the pick's within two combined standard errors reads Best; a class is the
 * gap to the Best group. This file drives it through the shipped oracle.wasm,
 * the way the replay screen's main thread does.
 *
 * REWRITTEN twice, not kept green. On 2026-11-01 it stopped asserting "exactly
 * one row is Best" (the prod tie bug: one BEST among eleven rows printing "EF
 * 3.76"). Later the same day Alex decided the panel shows the noisy race's
 * numbers, so the prod decision now reads: fusion's pick is the lowest number
 * (3.17), octogen's pick shows its own noisy 3.48 and is an inaccuracy.
 *
 * Mutation-checked (2026-11-01; each seen red by the named assertion, then
 * restored, every C mutant rebuilt into public/oracle.wasm.gz first): the
 * noise-free numbers shown ("the rows sort by the noisy race"); the tie
 * rule removed and the Best block not kept together (both "the pick leads its
 * block, its ties follow"); fusion's bar at 0 ("within the
 * bar, octogen's pick stands"); the tax left out of the noisy gap ("the trump
 * tax picks the noisy race's row"); and the accumulator returning its rows
 * unranked ("the rows sort by the noisy race's expected finish"); the
 * classes on raw EF gaps instead of expected points ("chess.com's classes on
 * expected points"). The kernel's own
 * cases, with more of these, are c/tests/oracle_rank_test.c.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

import { OracleAccumulator } from '../src/oracle/accumulator.ts';
import { OracleInstance } from '../src/oracle/oracleBridge.ts';
import { gunzip } from '../sdk/ts/wasm/gunzip.ts';
import type { OracleCandidate, OracleRecord, OracleRecordCandidate } from '../src/oracle/types.ts';

const ORACLE_BYTES = gunzip(new Uint8Array(readFileSync('public/oracle.wasm.gz')));
const kernel = (async () => { const k = new OracleInstance(); await k.init(ORACLE_BYTES); return k; })();

/** A row as an accumulator hands it over: its noise-free mean and SE, its
 *  noisy mean and SE, and the trumps it leads (an attack). */
const row = (key: string, mean: number, se: number, shadowMean: number | null, shadowSe: number, trumps = 0): OracleCandidate => ({
    key, type: 'attack', label: key, cards: Array.from({ length: trumps }, () => 'AS*'), n: 1000, mean, se,
    shadowN: shadowMean == null ? 0 : 500, shadowMean, shadowSe,
    ef: null, best: false, pick: false, cls: null, bar: 0, verdict: 'none', forcedLoss: false, pruned: false,
    chosen: false, played: false,
});
const ranked = async (list: OracleCandidate[], deckAlive = false, numPlayers = 4) => (await kernel).rank(list, false, deckAlive, numPlayers).rows;
const pickOf = async (list: OracleCandidate[], deckAlive = false) => {
    const r = await ranked(list, deckAlive);
    const p = r.filter((c) => c.pick);
    assert.equal(p.length, 1, 'exactly one row is fusion\'s pick');
    assert.ok(p[0].best, 'and it reads Best');
    return p[0].key;
};

test('the prod decision: the panel shows the noisy race, so fusion\'s pick is the lowest number', async () => {
    // Step 51, trimmed to one row of each kind (rows.txt of the repro).
    const list = [
        row('played', 3.8583, 0.0016, 3.3116, 0.004),
        row('low', 2.9975, 0.0025, 3.4824, 0.004),
        row('f', 3.7569, 0.0027, 3.1732, 0.004),
        row('twin1', 3.7559, 0.0027, 3.2325, 0.004),
        row('twin2', 3.7602, 0.0027, 3.3439, 0.004),
        row('mid', 3.3179, 0.0021, 3.6025, 0.004),
    ];
    const r = await ranked(list);
    const by = new Map(r.map((c) => [c.key, c]));
    assert.deepEqual(r.filter((c) => c.pick).map((c) => c.key), ['f'], 'fusion plays f');
    assert.deepEqual(r.map((c) => c.key), ['f', 'twin1', 'played', 'twin2', 'low', 'mid'], 'the rows sort by the noisy race');
    assert.deepEqual([by.get('f')!.ef, by.get('f')!.se], [3.1732, 0.004], 'the pick is the lowest number shown, with the noisy race\'s error');
    assert.equal(by.get('low')!.ef, 3.4824, 'octogen\'s pick shows its noisy number, not its noise-free 2.9975');
    assert.deepEqual(r.map((c) => c.cls), ['best', 'excellent', 'good', 'inaccuracy', 'mistake', 'mistake'],
        'chess.com\'s classes on expected points: the EF gap to the Best group over 3 on this 4-seat table (the played 3.31 loses 0.046: good)');
    assert.deepEqual(r.map((c) => c.best), [true, false, false, false, false, false], 'the noisy race separates the noise-free twins');
});

test('every row tied in the noisy race reads Best, the pick leads its block', async () => {
    // x is the noisy race's lowest, 0.004 under octogen's o: inside fusion's
    // bar (2 * sqrt(2) * 0.003 = 0.0085), so o stays the pick and x ties it.
    const r = await ranked([
        row('x', 3.30, 0.003, 3.096, 0.003),
        row('o', 3.00, 0.003, 3.100, 0.003),
        row('y', 3.30, 0.003, 3.106, 0.003),
        row('z', 3.30, 0.003, 3.140, 0.003),
    ]);
    assert.deepEqual(r.map((c) => [c.key, c.best, c.pick]), [['o', true, true], ['x', true, false], ['y', true, false], ['z', false, false]],
        'the pick leads its block, its ties follow, 0.04 behind is not Best');
    assert.equal(r[3].cls, 'excellent');
});

test('octogen\'s pick is fusion\'s unless the noisy race says otherwise beyond the bar', async () => {
    // se 0.01 each: the combined error is 0.01414, so the bar is 0.0283 places.
    assert.equal(await pickOf([row('o', 2.00, 0.001, 2.10, 0.01), row('f', 2.05, 0.001, 2.06, 0.01)]), 'f',
        'a noisy pick 0.04 better than the bar overrules');
    assert.equal(await pickOf([row('o', 2.00, 0.001, 2.10, 0.01), row('f', 2.05, 0.001, 2.08, 0.01)]), 'o',
        'within the bar, octogen\'s pick stands');
    assert.equal(await pickOf([row('o', 2.00, 0.001, 2.10, 0.02), row('f', 2.05, 0.001, 2.06, 0.001)]), 'o',
        'both rows\' error counts: 0.04 is inside two of their combined 0.020');
    assert.equal(await pickOf([row('o', 2.00, 0.001, 2.10, 0.01), row('f', 2.05, 0.001, 2.03, 0.01, 2)], true), 'o',
        'the trump tax picks the noisy race\'s row: 2.03 + 0.08 is not better than 2.10');
    assert.equal(await pickOf([row('o', 2.00, 0.001, 2.10, 0.01), row('f', 2.05, 0.001, 2.04, 0.01, 1)], true), 'o',
        'the trump tax counts in the gap: 2.04 + 0.04 is 0.02 better, inside the bar');
    assert.equal(await pickOf([row('o', 2.00, 0.001, 2.10, Infinity), row('f', 2.05, 0.001, 1.50, Infinity)]), 'o',
        'one batch is not evidence');
    assert.equal(await pickOf([row('o', 2.00, 0.001, null, Infinity), row('f', 2.05, 0.001, null, Infinity)]), 'o',
        'with no noisy race octogen\'s pick is fusion\'s');
    assert.equal(await pickOf([row('o', 2.00, 0.001, 2.30, 0.01), row('x', 2.02, 0.001, 2.20, 0.01), row('f', 2.05, 0.001, 2.10, 0.01)]), 'f',
        'the noisy race\'s pick is its lowest row, wherever it sorts');
});

test('an exact tie: octogen\'s pick stays the pick and both rows read Best', async () => {
    const r = await ranked([row('o', 2.00, 0.01, 2.10, 0.01), row('t', 2.00, 0.01, 2.10, 0.01)]);
    assert.deepEqual(r.map((c) => [c.key, c.best, c.pick]), [['o', true, true], ['t', true, false]]);
    assert.equal(r[0].bar, r[1].bar, 'and the same bar');
});

/** One batch's record: per row its noise-free and noisy mean over `n` worlds. */
const rec = (rows: [string, number, number][], n = 50): OracleRecord => ({
    seat: 0, deck: 10, defender: 1, trump: 3,
    belief: { pinned: [[], []], poolCount: 10, voids: [[], []], floor: [0, 0] },
    hand: [], opp_counts: [6, 6], table: [], solver: { applied: 0, result: 'none' },
    candidates: rows.map(([card, score, shadow]): OracleRecordCandidate => ({
        type: 'attack', label: `attack ${card}`, cards: [card], score, nsim: n,
        shadow_score: shadow, shadow_nsim: n, alive: 1, forced_loss: 0, verdict: 'none', chosen: 0,
    })),
});

test('the accumulator ranks through the kernel over every batch it merged', async () => {
    const ranker = await kernel;
    const acc = new OracleAccumulator({ deckAlive: false, recordedKey: '', numPlayers: 4, ranker });
    // 6S is octogen's (1.40 against 1.45) and the noisy race prefers 9H by 0.10
    // with batch-to-batch wobble of 0.01: overruled.
    for (let b = 0; b < 40; b++) {
        const w = (b % 2 ? 1 : -1) * 0.01;
        acc.add(rec([['6S', 1.40 + w, 1.60 + w], ['9H', 1.45 - w, 1.50 - w]]));
    }
    const list = acc.candidates(false);
    assert.equal(list[0].cards[0], '9H', 'the rows sort by the noisy race\'s expected finish');
    assert.ok(Math.abs(list[0].ef! - 1.50) < 1e-9 && list[0].se < 0.01, 'the number shown is the noisy mean, with its error');
    assert.deepEqual(list.map((c) => [c.best, c.pick]), [[true, true], [false, false]], 'the accumulator ranks through the kernel');
    assert.deepEqual(list.map((c) => c.cls), ['best', 'good'], 'octogen\'s 6S is 0.10 behind in the noisy race: 0.033 points on 4 seats');

    // The same with the noisy race in octogen's favour: Best stays on top.
    const keep = new OracleAccumulator({ deckAlive: false, recordedKey: '', numPlayers: 4, ranker });
    for (let b = 0; b < 40; b++) keep.add(rec([['6S', 1.40, 1.50 + (b % 2) * 0.01], ['9H', 1.45, 1.55]]));
    assert.deepEqual(keep.candidates(false).map((c) => c.pick), [true, false], 'octogen\'s pick stands');
});
