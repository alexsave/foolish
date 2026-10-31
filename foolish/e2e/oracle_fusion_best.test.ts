/* =============================================================================
 * Infinite Oracle - Best is fusion's pick (docs/INFINITE_ORACLE_DESIGN.md
 * "The brain")
 * =============================================================================
 * The rows show the noise-free race's expected finish (octogen's own numbers),
 * and src/oracle/accumulator.ts fusionBest marks fusion's pick over everything
 * sampled so far: octogen's pick (the top row) unless the noisy race's pick is
 * better, mean plus trump tax, by more than FUSION_SHADOW tenths of the two
 * rows' combined standard error. Pure: no module, no worker.
 *
 * Mutation-checked (2026-10-31; each seen red, then restored), in fusionBest:
 * the bar at 0 and the combined error replaced by f's alone (both "within the
 * bar, octogen's pick stays Best"); the tax left out of the gap ("the trump tax
 * counts in the gap", added because the tax case before it stayed green: there
 * the tax already keeps f from being picked); and the accumulator not calling
 * it ("the accumulator marks fusion's pick"). Two mutants are equivalent and
 * stay green: `gap < 0` dropped, and `<=` for `<` in the noisy pick's search,
 * because f is the lowest noisy row, so its gap is never positive and a tie
 * gives a gap of 0, which no bar clears. In oracleMtSession.ts, not calling it
 * is caught by e2e/oracle_mode_b.test.ts.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { OracleAccumulator, fusionBest } from '../src/oracle/accumulator.ts';
import type { OracleCandidate, OracleRecord, OracleRecordCandidate } from '../src/oracle/types.ts';
import { FUSION_SHADOW } from '../sdk/ts/gen/oracle_answer.oracle.ts';

/** A row as the accumulator hands it over: mean (noise-free), its noisy mean
 *  and SE, and its trump tax. */
const row = (key: string, mean: number, shadowMean: number | null, shadowSe: number, tax = 0): OracleCandidate => ({
    key, type: 'attack', label: key, cards: [], n: 1000, mean, se: 0.01, adjusted: mean + tax,
    shadowMean, shadowSe, best: false, verdict: 'none', forcedLoss: false, pruned: false,
    chosen: false, played: false,
});
const bestOf = (list: OracleCandidate[]) => {
    fusionBest(list);
    const b = list.filter((c) => c.best);
    assert.equal(b.length, 1, 'exactly one row is Best');
    return b[0].key;
};

test('the bar is fusion\'s own, read from the kernel', () => {
    assert.equal(FUSION_SHADOW, 20, 'two standard errors (c/src/octogen_strategy.h)');
});

test('octogen\'s pick is Best unless the noisy race says otherwise beyond the bar', () => {
    // se 0.01 each: the combined error is 0.01414, so the bar is 0.0283 places.
    assert.equal(bestOf([row('o', 2.00, 2.10, 0.01), row('f', 2.05, 2.06, 0.01)]), 'f',
        'a noisy pick 0.04 better than the bar overrules');
    assert.equal(bestOf([row('o', 2.00, 2.10, 0.01), row('f', 2.05, 2.08, 0.01)]), 'o',
        'within the bar, octogen\'s pick stays Best');
    assert.equal(bestOf([row('o', 2.00, 2.10, 0.01), row('f', 2.05, 2.10, 0.01)]), 'o',
        'an exact tie keeps octogen\'s pick');
    assert.equal(bestOf([row('o', 2.00, 2.10, 0.01), row('f', 2.05, 2.20, 0.01)]), 'o',
        'a worse noisy pick never overrules');
    assert.equal(bestOf([row('o', 2.00, 2.10, 0.02), row('f', 2.05, 2.06, 0.001)]), 'o',
        'both rows\' error counts: 0.04 is inside two of their combined 0.020');
    assert.equal(bestOf([row('o', 2.00, 2.10, 0.01), row('f', 2.05, 2.03, 0.01, 0.08)]), 'o',
        'the trump tax picks the noisy race\'s row: 2.03 + 0.08 is not better than 2.10');
    assert.equal(bestOf([row('o', 2.00, 2.10, 0.01), row('f', 2.05, 2.05, 0.01, 0.03)]), 'o',
        'the trump tax counts in the gap: 2.05 + 0.03 is 0.02 better, inside the bar');
    assert.equal(bestOf([row('o', 2.00, 2.10, Infinity), row('f', 2.05, 1.50, Infinity)]), 'o',
        'one batch is not evidence');
    assert.equal(bestOf([row('o', 2.00, null, Infinity), row('f', 2.05, null, Infinity)]), 'o',
        'with no noisy race octogen\'s pick is Best');
    assert.equal(bestOf([row('o', 2.00, 2.30, 0.01), row('x', 2.02, 2.20, 0.01), row('f', 2.05, 2.10, 0.01)]), 'f',
        'the noisy race\'s pick is its lowest row, wherever it sorts');
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

test('the accumulator marks fusion\'s pick over every batch it merged', () => {
    const acc = new OracleAccumulator({ deckAlive: false, recordedKey: '' });
    // 6S is octogen's (1.40 against 1.45) and the noisy race prefers 9H by 0.10
    // with batch-to-batch wobble of 0.01: overruled.
    for (let b = 0; b < 40; b++) {
        const w = (b % 2 ? 1 : -1) * 0.01;
        acc.add(rec([['6S', 1.40 + w, 1.60 + w], ['9H', 1.45 - w, 1.50 - w]]));
    }
    const list = acc.candidates(false);
    assert.equal(list[0].cards[0], '6S', 'the rows sort by the noise-free expected finish');
    assert.ok(Math.abs(list[0].shadowMean! - 1.60) < 1e-9 && list[0].shadowSe < 0.01, 'the noisy mean and its error');
    assert.deepEqual(list.map((c) => c.best), [false, true], 'the accumulator marks fusion\'s pick');

    // The same with the noisy race in octogen's favour: Best stays on top.
    const keep = new OracleAccumulator({ deckAlive: false, recordedKey: '' });
    for (let b = 0; b < 40; b++) keep.add(rec([['6S', 1.40, 1.50 + (b % 2) * 0.01], ['9H', 1.45, 1.55]]));
    assert.deepEqual(keep.candidates(false).map((c) => c.best), [true, false], 'octogen\'s pick stands');
});
