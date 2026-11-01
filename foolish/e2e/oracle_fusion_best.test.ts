/* =============================================================================
 * Infinite Oracle - Best is fusion's pick and every row tied with it
 * (docs/INFINITE_ORACLE_DESIGN.md "Ties, Best and the classes")
 * =============================================================================
 * The rows show the noise-free race's expected finish (octogen's own numbers).
 * The kernel (c/src/oracle_rank.c, which states the whole rule) ranks them for
 * both modes: fusion's pick is octogen's pick (the top row) unless the noisy
 * race's pick is better, mean plus trump tax, by more than FUSION_SHADOW tenths
 * of the two rows' combined standard error; every row whose number ties the
 * pick's within two combined standard errors reads Best; and a class is the gap
 * to the Best group, not to the lowest number. This file drives it through the
 * shipped oracle.wasm, the way the replay screen's main thread does.
 *
 * REWRITTEN on 2026-11-01, not kept green: this file used to assert "exactly one
 * row is Best", and that assertion was the prod bug (step 51 of the replay in
 * the design doc, eleven rows printing "EF 3.76 +-0.00", one BEST, ten
 * BLUNDER). Exactly one row is fusion's PICK; Best is the pick's tied group.
 *
 * Mutation-checked (2026-11-01; each seen red by the named assertion, then
 * restored): oracle_rank.c's tie rule removed ("every 3.76 twin reads Best");
 * the Best block not kept together ("the Best rows sit together, the pick
 * first"); the class from the lowest number ("3.86 is good, not blunder");
 * fusion's bar at 0 ("within the bar, octogen's pick stands"); the tax left out
 * of the noisy gap ("the trump tax picks the noisy race's row"); and the
 * accumulator returning its rows unranked ("the accumulator ranks through the
 * kernel"). Each C mutant was rebuilt into public/oracle.wasm.gz first. The
 * kernel's own cases, with more of these, are c/tests/oracle_rank_test.c.
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
    best: false, pick: false, cls: null, bar: 0, verdict: 'none', forcedLoss: false, pruned: false,
    chosen: false, played: false,
});
const ranked = async (list: OracleCandidate[], deckAlive = false) => (await kernel).rank(list, false, deckAlive);
const pickOf = async (list: OracleCandidate[], deckAlive = false) => {
    const r = await ranked(list, deckAlive);
    const p = r.filter((c) => c.pick);
    assert.equal(p.length, 1, 'exactly one row is fusion\'s pick');
    assert.ok(p[0].best, 'and it reads Best');
    return p[0].key;
};

test('the prod decision: every row tied with fusion\'s pick reads Best', async () => {
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
    assert.deepEqual(['f', 'twin1', 'twin2'].map((k) => by.get(k)!.best), [true, true, true], 'every 3.76 twin reads Best');
    assert.deepEqual(['f', 'twin1', 'twin2'].map((k) => by.get(k)!.cls), ['best', 'best', 'best'], 'and is class best');
    assert.deepEqual(r.map((c) => c.key), ['low', 'mid', 'f', 'twin1', 'twin2', 'played'], 'the Best rows sit together, the pick first');
    assert.equal(by.get('played')!.cls, 'good', '3.86 is good, not blunder: 0.10 behind the Best group');
    assert.equal(by.get('low')!.cls, 'best', 'the lowest number still reads best');
    assert.equal(by.get('low')!.best, false, 'but it is not Best: fusion does not play it');
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
    const acc = new OracleAccumulator({ deckAlive: false, recordedKey: '', ranker });
    // 6S is octogen's (1.40 against 1.45) and the noisy race prefers 9H by 0.10
    // with batch-to-batch wobble of 0.01: overruled.
    for (let b = 0; b < 40; b++) {
        const w = (b % 2 ? 1 : -1) * 0.01;
        acc.add(rec([['6S', 1.40 + w, 1.60 + w], ['9H', 1.45 - w, 1.50 - w]]));
    }
    const list = acc.candidates(false);
    assert.equal(list[0].cards[0], '6S', 'the rows sort by the noise-free expected finish');
    assert.ok(Math.abs(list[0].shadowMean! - 1.60) < 1e-9 && list[0].shadowSe < 0.01, 'the noisy mean and its error');
    assert.deepEqual(list.map((c) => [c.best, c.pick]), [[false, false], [true, true]], 'the accumulator ranks through the kernel');
    assert.deepEqual(list.map((c) => c.cls), ['best', 'best'], 'the lowest number reads best, and so does fusion\'s pick');

    // The same with the noisy race in octogen's favour: Best stays on top.
    const keep = new OracleAccumulator({ deckAlive: false, recordedKey: '', ranker });
    for (let b = 0; b < 40; b++) keep.add(rec([['6S', 1.40, 1.50 + (b % 2) * 0.01], ['9H', 1.45, 1.55]]));
    assert.deepEqual(keep.candidates(false).map((c) => c.pick), [true, false], 'octogen\'s pick stands');
});
