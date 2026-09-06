/* =============================================================================
 * Infinite Oracle - Mode B suite (docs/INFINITE_ORACLE_DESIGN.md §8b.7)
 * =============================================================================
 * Drives the REAL src/oracle Mode B objects (OracleMtSession + the C
 * accumulator in public/oracle-mt.wasm.gz) over node:worker_threads, against
 * games played here rather than frozen replay codes (helpers/seeded_game.ts -
 * a frozen code is only readable by the kernel that cut it).
 *
 * What each test is FOR, since a threaded suite can go green for the wrong
 * reason more easily than most:
 *
 *   §8b.7-0  the module under test is the threaded one and it really ran.
 *            Without this, every assertion below passes vacuously if the wasm
 *            failed to load or no thread ever entered octogen.
 *   §8b.7-1  Mode A and Mode B agree on the analysis. Different seed streams,
 *            so this is a tolerance, not equality.
 *   §8b.7-2  more threads means more sampled worlds.
 *   §8b.7-3  generation churn never wedges a parked thread.
 *   §8b.7-4  the candidate-set guard. Threads can legitimately disagree about
 *            the candidate SET (forced_loss comes from a budget-bounded solve
 *            off a per-thread table), and summing two different sets under one
 *            index would average two different moves into one bar. This asserts
 *            the guard is there and that a clean run does not trip it.
 *   §8b.7-5  the exact endgame regime, which is where the seat-perspective
 *            transposition-table bug lived. Mode B must reach the SAME verdicts
 *            as Mode A, off per-thread tables, under concurrency.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

import { decodeReplay } from '../server/api/common/replay/decode.ts';
import { bytesToBigint } from '../server/api/common/replay/codec.ts';
import { gunzip } from '../sdk/ts/wasm/gunzip.ts';
import { buildReplayFrames } from '../src/replay/frames.ts';
import { buildOracleJob } from '../src/oracle/replayOracleInput.ts';
import { OracleInstance } from '../src/oracle/oracleBridge.ts';
import { OracleAccumulator } from '../src/oracle/accumulator.ts';
import { OracleJob } from '../src/oracle/types.ts';
import { ORACLE_MT_ENV, oracleSeedBase } from '../src/oracle/oracleMtSession.ts';
import { openMtRig } from './helpers/oracle_mt_node.ts';
import { playSeededV6 } from './helpers/seeded_game.ts';

const ORACLE_BYTES = gunzip(new Uint8Array(readFileSync('public/oracle.wasm.gz')));

/* The two fixtures, pinned by step index rather than by "40% of the way
 * through". Both are asserted to have the SHAPE the test needs, so a kernel
 * change that moves them fails loudly instead of quietly leaving a
 * two-candidate decision that every assertion below would pass on. The indices
 * come from a sweep of the played game (candidates + solver-applied per step).
 *
 *   MC     np=2 seed=7 step 39 - deck ALIVE,  ~14 candidates, endgame solver OFF
 *   EXACT  np=2 seed=7 step 49 - deck DEAD,   ~14 candidates, endgame solver ON
 */
const FIX_MC = { np: 2, seed: 7, idx: 39 };
const FIX_EXACT = { np: 2, seed: 7, idx: 49 };

type Played = { decoded: Awaited<ReturnType<typeof decodeReplay>>; frames: ReturnType<typeof buildReplayFrames> };
const gameCache = new Map<string, Played>();

async function jobAt(np: number, seed: number, idx: number): Promise<OracleJob> {
    const key = `${np}:${seed}`;
    let hit = gameCache.get(key);
    if (!hit) {
        const played = await playSeededV6(np, seed);
        assert.ok(played, 'the seeded game finished');
        const decoded = await decodeReplay(bytesToBigint(played!.code));
        hit = { decoded, frames: buildReplayFrames(played!.code, 'g', null, { fool: decoded.fool }) };
        gameCache.set(key, hit);
    }
    const job = buildOracleJob(hit.frames, hit.decoded, idx, true, `mt-${np}-${seed}-${idx}`);
    assert.ok(job, `step ${idx} of the ${np}p seed-${seed} game is a decision`);
    return job!;
}
const mcJob = () => jobAt(FIX_MC.np, FIX_MC.seed, FIX_MC.idx);
const exactJob = () => jobAt(FIX_EXACT.np, FIX_EXACT.seed, FIX_EXACT.idx);

/** Mode A over one instance: batch until every candidate has `minN` worlds, or
 *  the batch / wall-clock cap runs out. Returns the live accumulator. */
async function modeA(job: OracleJob, minN: number, batchCap: number, msCap: number) {
    const inst = new OracleInstance();
    await inst.init(ORACLE_BYTES);
    inst.writeEnv({ ...ORACLE_MT_ENV });
    const acc = new OracleAccumulator({ deckAlive: job.deckAlive, recordedKey: job.recordedKey });
    const end = Date.now() + msCap;
    let batches = 0;
    for (let b = 0; b < batchCap && Date.now() < end; b++) {
        const r = inst.analyzeOnce(job.gameBlob, job.seat, job.logsWire, job.memoryOn, 1009 + b * 7919);
        if (!('record' in r)) continue;
        acc.add(r.record);
        batches++;
        const list = acc.candidates(false);
        if (list.length && Math.min(...list.map((c) => c.n)) >= minN) break;
    }
    assert.ok(batches > 0, 'Mode A produced at least one batch');
    return { acc, batches };
}

test('§8b.7-0 the threaded module is what ran, and threads really deliberated', async () => {
    const job = await mcJob();
    const rig = await openMtRig(job, 2);
    try {
        // A shared-memory module IMPORTS its memory and cannot instantiate
        // without a shared one. If this ever reads false, the suite is testing
        // the single-threaded artifact by mistake.
        assert.equal(rig.session.memory!.buffer instanceof SharedArrayBuffer, true,
            'oracle-mt.wasm.gz runs over a SharedArrayBuffer');
        assert.equal(rig.session.totalChooses(), 0, 'no batch before the generation is armed');

        await rig.run(job, oracleSeedBase(job.decisionId), 1500);
        assert.ok(rig.session.totalChooses() > 0, 'threads entered octogen');
        assert.ok(rig.session.batches() > 0, 'batches reached the C accumulator');
        assert.equal(rig.session.canaryTrips(), 0, 'no thread stack overflowed (§8b.6)');
        console.log(`  §8b.7-0: ${rig.session.totalChooses()} chooses, ${rig.session.batches()} batches, canary 0`);
    } finally { await rig.close(); }
});

test('§8b.7-1 Mode A and Mode B converge to the same per-candidate means', async () => {
    const job = await mcJob();
    const rig = await openMtRig(job, 3);
    try {
        await rig.run(job, oracleSeedBase(job.decisionId), 2500);
        const b = rig.session.readCandidates(job, false);
        // The fixture is chosen for its width: a two-row decision would let an
        // index-confusion bug slip through.
        assert.ok(b.length >= 8, `the MC fixture is a wide decision (${b.length} candidates)`);
        const minN = Math.min(...b.map((c) => c.n));
        assert.ok(minN > 500, `Mode B sampled enough worlds (min ${minN})`);

        // Mode A is single-instance here, so it is asked to match Mode B's world
        // count only up to a cap - past ~20k worlds the standard error is already
        // an order of magnitude below the tolerance and the rest is CI minutes.
        const { acc } = await modeA(job, Math.min(minN, 20000), 20000, 25000);
        const aRows = acc.candidates(false).filter((c) => c.mean != null);
        const aByKey = new Map(aRows.map((c) => [c.key, c]));
        const aMinN = Math.min(...aRows.map((c) => c.n));
        assert.ok(aMinN > 500, `Mode A sampled enough worlds (min ${aMinN})`);

        let matched = 0, worst = 0, worstKey = '';
        for (const c of b) {
            const a = aByKey.get(c.key);
            if (!a || a.mean == null || c.mean == null) continue;
            matched++;
            const d = Math.abs(a.mean - c.mean);
            if (d > worst) { worst = d; worstKey = c.key; }
        }
        assert.equal(matched, b.length, `every Mode B candidate has a Mode A twin (${matched}/${b.length})`);
        // Independent seed streams, so a tolerance rather than equality. 0.05
        // finish positions is far inside what a real difference in what is being
        // computed would produce: a wrong seat's verdict, a mis-indexed
        // accumulator or a lost batch class each move a row by >= 0.3.
        assert.ok(worst < 0.05, `worst |A-B| = ${worst.toFixed(4)} on ${worstKey}`);
        console.log(`  §8b.7-1: ${matched} candidates matched, worst |A-B| = ${worst.toFixed(4)} (A n>=${aMinN}, B n>=${minN})`);
    } finally { await rig.close(); }
});

test('§8b.7-2 more threads sample more worlds', async () => {
    const job = await mcJob();
    const one = await openMtRig(job, 1);
    let solo = 0;
    try {
        await one.run(job, oracleSeedBase(job.decisionId), 2000);
        solo = one.session.totalChooses();
    } finally { await one.close(); }

    const many = await openMtRig(job, 3);
    let trio = 0;
    try {
        await many.run(job, oracleSeedBase(job.decisionId), 2000);
        trio = many.session.totalChooses();
    } finally { await many.close(); }

    assert.ok(solo > 0 && trio > 0, 'both runs deliberated');
    // A shared CI runner may have fewer cores than threads, so this asserts the
    // direction, not a multiplier; the multiplier is the bench's job.
    assert.ok(trio > solo * 1.2, `3 threads out-sampled 1 (${solo} -> ${trio})`);
    console.log(`  §8b.7-2: 1 thread ${solo}, 3 threads ${trio} (${(trio / solo).toFixed(2)}x)`);
});

test('§8b.7-3 generation churn never wedges a parked thread', async () => {
    const job = await mcJob();
    const rig = await openMtRig(job, 3);
    try {
        for (let g = 0; g < 6; g++) {
            await rig.run(job, oracleSeedBase(`${job.decisionId}:${g}`), 250);
            assert.ok(rig.session.totalChooses() > 0, `generation ${g} deliberated`);
            assert.equal(rig.session.active(), 0, `generation ${g} drained`);
        }
        // and the threads are still alive after all that churn
        await rig.run(job, oracleSeedBase(job.decisionId), 1000);
        assert.ok(rig.session.totalChooses() > 0, 'still deliberating after 6 generations');
        assert.equal(rig.session.canaryTrips(), 0, 'no stack canary trip across the churn');
        console.log(`  §8b.7-3: 7 generations, final ${rig.session.totalChooses()} chooses`);
    } finally { await rig.close(); }
});

test('§8b.7-4 the candidate-set guard is armed and a clean run does not trip it', async () => {
    const job = await mcJob();
    const rig = await openMtRig(job, 4);
    try {
        await rig.run(job, oracleSeedBase(job.decisionId), 2500);
        const batches = rig.session.batches();
        const dropped = rig.session.mismatches();
        assert.ok(batches > 0, 'batches landed');
        // Every batch that contributed agreed with the published descriptor
        // table, so the accumulator holds one move per index, not a blend.
        assert.equal(dropped, 0, `no batch was dropped for a candidate-set mismatch (${dropped})`);
        const cands = rig.session.readCandidates(job, false);
        assert.ok(cands.length >= 8, 'descriptors published for the whole wide decision');
        assert.equal(new Set(cands.map((c) => c.key)).size, cands.length, 'candidate keys are distinct');
        // batches x per-batch worlds should show up as folded worlds; if the
        // guard dropped everything this would be 0 while `batches` stayed high.
        assert.ok(Math.min(...cands.map((c) => c.n)) > 0, 'every candidate carries worlds');
        console.log(`  §8b.7-4: ${batches} batches, ${dropped} dropped, ${cands.length} distinct candidates`);
    } finally { await rig.close(); }
});

test('§8b.7-5 exact endgame: Mode B reaches Mode A verdicts off per-thread tables', async () => {
    const job = await exactJob();
    assert.equal(job.deckAlive, false, 'the exact fixture is a deck-empty decision');
    const rig = await openMtRig(job, 3);
    try {
        await rig.run(job, oracleSeedBase(job.decisionId), 3000);
        assert.equal(rig.session.solverFired(), true, 'the root endgame solver engaged (MT6 probe ran)');
        const exact = rig.session.hasProof();
        const b = rig.session.readCandidates(job, exact);
        assert.ok(b.length >= 8, `the exact fixture is a wide decision (${b.length} candidates)`);

        const { acc } = await modeA(job, 1, 8, 30000);
        const aRows = acc.candidates(true);
        const aByKey = new Map(aRows.map((c) => [c.key, c]));

        let compared = 0, proofs = 0;
        for (const c of b) {
            if (c.verdict === 'win' || c.verdict === 'loss' || c.verdict === 'draw') proofs++;
            const ref = aByKey.get(c.key);
            if (!ref) continue;
            // Only PROOFS are compared: 'unknown' is a budget outcome and the two
            // modes do not spend identical budgets. A proof must agree, though -
            // a value read back in the wrong seat's perspective flips win to
            // loss, which is exactly the bug this regime hides.
            const proven = (v: string) => v === 'win' || v === 'loss' || v === 'draw';
            if (proven(c.verdict) && proven(ref.verdict)) {
                assert.equal(c.verdict, ref.verdict, `verdict agrees for ${c.key}`);
                compared++;
            }
        }
        assert.ok(proofs > 0, 'Mode B proved something in the endgame regime');
        assert.ok(compared > 0, 'at least one proof was comparable against Mode A');
        assert.equal(rig.session.canaryTrips(), 0, 'no stack canary trip in the endgame regime');
        assert.equal(rig.session.mismatches(), 0, 'no dropped batch in the endgame regime');
        console.log(`  §8b.7-5: ${proofs} proofs in Mode B, ${compared} agreed with Mode A, ${b.length} candidates`);
    } finally { await rig.close(); }
});
