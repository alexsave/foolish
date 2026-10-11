/* =============================================================================
 * Infinite Oracle - the answer is a struct, and it says what the text said
 * =============================================================================
 * Mode A's bridge used to read each batch as a line of JSON the kernel printed
 * (og_ex_emit, through a freestanding snprintf) plus a packed blob ("OGP1")
 * decoded with DataView at hand-typed offsets. Both are gone: the kernel fills
 * c/src/oracle_answer.h OgAnswer and the bridge reads it through the reader
 * structgen writes from that header.
 *
 * WHAT THIS FILE PROVES is that the change of transport changed nothing else.
 * e2e/fixtures/oracle_answer/text_path.json is what the TEXT path returned -
 * `JSON.parse` of the kernel's line, and the old decoder's reading of the blob -
 * for the decisions listed in it, captured from the commit before the struct
 * existed (88b9681b) by driving the same games through the same bridge calls.
 * Each decision is deliberated again here and the struct's answer is held to
 * the captured one field by field.
 *
 * The fixture is thirteen decisions chosen for what they carry: Monte Carlo
 * with and without memory, a single legal move, the exact regime with pruned
 * rows, a forced win, an unproven probe, voids, floors and pins, multi-card
 * covers over a part-covered table, passes, an 8-seat table, a 28-candidate
 * hand, storylines that resolve early. The capture they were cut from was every
 * decision of six games with memory on and off - 1,306 answers, 17,213 rows -
 * and this same comparison passed on all of it before the text path was
 * deleted. One thing no captured answer carried is a row with forced_loss set
 * (octogen drops a proven loss from candidacy before it would be a row), so
 * that flag is held equal here only at 0.
 *
 * The fields that are NOT compared byte for byte, and why:
 *
 *   score       the text carried a mean as "%.4f"; the struct carries the mean.
 *               So the assertion is that the struct's mean ROUNDS to the text's,
 *               by the rule the kernel's formatter used (half up at the fourth
 *               decimal). Nothing downstream is worse off for the extra digits.
 *   pool        the text listed the unknown cards; nothing ever read more than
 *               their count, so the struct carries the count.
 *   hand_count, chosen (the label), ply, nlogs
 *               restated what the record already said (the hand's length, the
 *               chosen row's label) or were read by nobody; they are not in the
 *               record any more, and the first two are checked to be derivable.
 *
 * WHEN THIS GOES RED FOR A REASON THAT IS NOT THE BRIDGE: the fixture is an
 * answer, so it moves whenever octogen's deliberation, the bots that play the
 * fixture games, or the legal-move menu deliberately change. The text path no
 * longer exists to re-capture from. `ORACLE_ANSWER_REPIN=1` rewrites the fixture
 * from the struct path as it stands, after which this file is a pin of the
 * answer rather than a proof against the text - say so in the commit that does
 * it. The second and third tests do not depend on the fixture at all.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, writeFileSync } from 'node:fs';

import { gunzip } from '../sdk/ts/wasm/gunzip.ts';
import { buildReplayFrames } from '../src/replay/frames.ts';
import { buildOracleJob } from '../src/oracle/replayOracleInput.ts';
import { OracleInstance } from '../src/oracle/oracleBridge.ts';
import { ORACLE_KNOBS, type OracleJob, type OracleKnobs, type OracleRecord, type OracleCandPaths } from '../src/oracle/types.ts';
import { oracleSeedBase } from '../src/oracle/oracleMtSession.ts';
import { openMtRig } from './helpers/oracle_mt_node.ts';
import { playBotTable, seedBytes } from './helpers/bot_table.ts';

const ORACLE_BYTES = gunzip(new Uint8Array(readFileSync('public/oracle.wasm.gz')));
const FIXTURE = 'e2e/fixtures/oracle_answer/text_path.json';
const KNOBS: OracleKnobs = { ...ORACLE_KNOBS, w1: 8 };

/** One captured answer: the decision it is of, and what the text path returned. */
interface Captured {
    label: string; brains: string[]; seed: number;
    idx: number; memoryOn: boolean; analyzeSeed: number;
    record: TextRecord;
    /** The old decoder's reading of the blob: [candidate index, its paths]. */
    paths: [number, OracleCandPaths][] | null;
}
/** The record as the kernel's JSON line spelled it. */
interface TextRecord {
    seat: number; deck: number; defender: number; trump: number;
    belief: { pinned: string[][]; pool: string[]; voids: string[][]; floor: number[] };
    hand: string[]; hand_count: number; opp_counts: number[];
    table: { attack: string; defense: string | null }[];
    solver: { applied: number; result: string };
    candidates: (Record<string, unknown> & { label: string; score: number | null; chosen: number })[];
    chosen: string;
}

type Frames = ReturnType<typeof buildReplayFrames>;
const games = new Map<string, { code: Uint8Array; frames: Frames }>();
type Played = Pick<Captured, 'label' | 'brains' | 'seed'>;
function gameOf(c: Played): { code: Uint8Array; frames: Frames } {
    let g = games.get(c.label);
    if (!g) {
        const code = playBotTable(c.brains, seedBytes(c.brains.length, c.seed)).code;
        g = { code, frames: buildReplayFrames(code, 'g', null) };
        games.set(c.label, g);
    }
    return g;
}
function jobOf(c: Played & Pick<Captured, 'idx' | 'memoryOn'>): OracleJob {
    const g = gameOf(c);
    const job = buildOracleJob(g.frames, g.code, c.idx, c.memoryOn, c.label);
    assert.ok(job, `${c.label} step ${c.idx} is still a decision`);
    return job!;
}

/** The kernel's "%.4f": half up at the fourth decimal (wasm_vsnprintf did
 *  `d * 1e4 + 0.5`, truncated), read back the way JSON.parse read the digits. */
const asText4 = (x: number) => Math.floor(x * 1e4 + 0.5) / 1e4;

/** Hold a struct-path record to the text path's, naming the decision on a miss. */
function assertSameAnswer(got: OracleRecord, c: Captured): void {
    const at = `${c.label} step ${c.idx} memory ${c.memoryOn ? 'on' : 'off'}`;
    const want = c.record;
    const why = new Map(c.paths ?? []);

    const { candidates: gotRows, belief: gotBelief, ...gotHead } = got;
    assert.deepEqual(gotHead, {
        seat: want.seat, deck: want.deck, defender: want.defender, trump: want.trump,
        hand: want.hand, opp_counts: want.opp_counts, table: want.table, solver: want.solver,
    }, `${at}: the position and the solver`);
    assert.deepEqual(gotBelief, {
        pinned: want.belief.pinned, poolCount: want.belief.pool.length,
        voids: want.belief.voids, floor: want.belief.floor,
    }, `${at}: the belief`);
    // The two fields the record no longer restates are still what it says.
    assert.equal(got.hand.length, want.hand_count, `${at}: hand_count was the hand's length`);
    assert.equal(gotRows.find((r) => r.chosen)?.label ?? '?', want.chosen, `${at}: chosen was the chosen row's label`);

    assert.equal(gotRows.length, want.candidates.length, `${at}: the number of rows`);
    for (let k = 0; k < gotRows.length; k++) {
        const { why: gotWhy, score: gotScore, ...gotRow } = gotRows[k];
        const { score: wantScore, ...wantRow } = want.candidates[k];
        assert.deepEqual(gotRow, wantRow, `${at}: row ${k} (${wantRow.label})`);
        if (wantScore === null) assert.equal(gotScore, null, `${at}: row ${k} is unscored`);
        else assert.equal(asText4(gotScore!), wantScore, `${at}: row ${k}'s mean ${gotScore} prints as ${wantScore}`);
        assert.deepEqual(gotWhy, why.get(k), `${at}: row ${k}'s storylines`);
    }
}

test('the struct answers what the JSON line and the OGP1 blob answered', async () => {
    const captured = JSON.parse(readFileSync(FIXTURE, 'utf8')) as Captured[];
    const inst = new OracleInstance();
    await inst.init(ORACLE_BYTES);
    inst.setKnobs(KNOBS);

    const seen = { rows: 0, scored: 0, why: 0, pruned: 0, verdicts: 0, covers: 0, pinned: 0, voids: 0, floors: 0, covered: 0 };
    for (const c of captured) {
        const r = inst.analyzeOnce(jobOf(c), c.analyzeSeed);
        assert.ok('record' in r, `${c.label} step ${c.idx}: the choose answered`);
        if (process.env.ORACLE_ANSWER_REPIN) { repin(c, r.record); continue; }
        assertSameAnswer(r.record, c);
        for (const row of r.record.candidates) {
            seen.rows++;
            if (row.score !== null) seen.scored++;
            if (row.why) seen.why++;
            if (row.pruned) seen.pruned++;
            if (row.verdict_val !== undefined) seen.verdicts++;
            if (row.target && row.target.length > 1) seen.covers++;
        }
        if (r.record.belief.pinned.some((p) => p.length)) seen.pinned++;
        if (r.record.belief.voids.some((p) => p.length)) seen.voids++;
        if (r.record.belief.floor.some((v) => v > 0)) seen.floors++;
        if (r.record.table.some((b) => b.defense !== null)) seen.covered++;
    }
    if (process.env.ORACLE_ANSWER_REPIN) {
        writeFileSync(FIXTURE, JSON.stringify(captured));
        console.log(`  re-pinned ${captured.length} answers from the struct path into ${FIXTURE}`);
        return;
    }
    // An equality nothing exercised is not a proof: every kind of field the
    // struct carries has to have been non-trivial somewhere in the fixture.
    for (const [what, n] of Object.entries(seen)) assert.ok(n > 0, `the fixture exercises ${what}`);
    console.log(`  ${captured.length} decisions, ${seen.rows} rows (${seen.scored} scored, ${seen.why} with storylines, `
        + `${seen.pruned} pruned, ${seen.verdicts} proven) identical to the text path`);
});

/** Rewrite one captured answer from the struct path, in the text path's
 *  spelling (see the header: after this the fixture is a pin, not a proof). */
function repin(c: Captured, rec: OracleRecord): void {
    const paths: [number, OracleCandPaths][] = [];
    c.record = {
        seat: rec.seat, deck: rec.deck, defender: rec.defender, trump: rec.trump,
        belief: { pinned: rec.belief.pinned, pool: Array(rec.belief.poolCount).fill('?'), voids: rec.belief.voids, floor: rec.belief.floor },
        hand: rec.hand, hand_count: rec.hand.length, opp_counts: rec.opp_counts,
        table: rec.table, solver: rec.solver,
        candidates: rec.candidates.map(({ why, score, ...row }, k) => {
            if (why) paths.push([k, why]);
            return { ...row, score: score === null ? null : asText4(score) };
        }),
        chosen: rec.candidates.find((r) => r.chosen)?.label ?? '?',
    };
    c.paths = paths;
}

// The decisions the knob tests deliberate are FOUND by walking a played game,
// not pinned to a step, so a change in how the bots play moves them instead of
// breaking the test.
//
// A Monte Carlo decision with a handful of candidates. The width matters:
// keep1 / keep2 at 26 only turn racing off while there are at most 26
// candidates to keep, so a wider board is cut whatever the knobs say and its
// counts are not uniform.
const MC = { label: 'knobs-4p', brains: Array(4).fill('handwritten'), seed: 42 };
async function narrowDecision(): Promise<OracleJob> {
    const inst = new OracleInstance();
    await inst.init(ORACLE_BYTES);
    inst.setKnobs(KNOBS);
    const g = gameOf(MC);
    for (let idx = 1; idx < g.frames.length; idx++) {
        const job = buildOracleJob(g.frames, g.code, idx, true, MC.label);
        if (!job) continue;
        const r = inst.analyzeOnce(job, 5);
        if (!('record' in r) || r.record.solver.applied) continue;
        const n = r.record.candidates.length;
        if (n >= 5 && n <= 12 && r.record.candidates.every((c) => c.nsim > 0)) return job;
    }
    throw new Error('no narrow Monte Carlo decision in the knob game');
}

// A 2-seat endgame the exact probe proves at its full budget.
const ENDGAME = { label: 'knobs-2p', brains: ['handwritten', 'handwritten'], seed: 40 };
async function provenEndgame(inst: OracleInstance): Promise<OracleJob> {
    const g = gameOf(ENDGAME);
    for (let idx = g.frames.length - 1; idx > 0; idx--) {
        const job = buildOracleJob(g.frames, g.code, idx, true, ENDGAME.label);
        if (!job) continue;
        const r = inst.analyzeOnce(job, 77);
        if ('record' in r && r.record.solver.applied
            && r.record.candidates.some((c) => c.verdict === 'win' || c.verdict === 'loss')
            && r.record.candidates.length > 1) return job;
    }
    throw new Error('no endgame in the knob game is proven at the full budget');
}

test('every knob in the struct reaches octogen, and an unnamed one goes back to its default', async () => {
    const inst = new OracleInstance();
    await inst.init(ORACLE_BYTES);
    const mc = await narrowDecision();
    const nsims = (knobs: OracleKnobs, job: OracleJob = mc): number[] => {
        inst.setKnobs(knobs);
        const r = inst.analyzeOnce(job, 5);
        assert.ok('record' in r, 'the choose answered');
        return r.record.candidates.filter((c) => !c.pruned).map((c) => c.nsim);
    };

    // w1, w2, w3: with racing off every candidate is sampled in every stage, so
    // a batch is exactly the three stages' worlds. Each knob moves it alone.
    const flat = (n: number[]) => [...new Set(n)];
    assert.deepEqual(flat(nsims({ ...KNOBS, w1: 8, w2: 1, w3: 0 })), [9], 'w1 8 + w2 1');
    assert.deepEqual(flat(nsims({ ...KNOBS, w1: 20, w2: 1, w3: 0 })), [21], 'w1 moved the batch');
    assert.deepEqual(flat(nsims({ ...KNOBS, w1: 8, w2: 5, w3: 0 })), [13], 'w2 moved the batch');
    assert.deepEqual(flat(nsims({ ...KNOBS, w1: 8, w2: 1, w3: 7 })), [16], 'w3 moved the batch');

    // keep1, keep2: how many candidates survive into stage two and three. At 26
    // nobody here is cut; at 3 and 1 the worlds stop being uniform, one stage
    // each. (Three, because octogen never keeps fewer past stage one.)
    const cut1 = nsims({ ...KNOBS, w1: 8, w2: 5, w3: 0, keep1: 3 });
    assert.equal(cut1.filter((n) => n === 13).length, 3, `keep1 3 lets three candidates into stage two (${cut1})`);
    assert.equal(cut1.filter((n) => n === 8).length, cut1.length - 3, 'and stops the rest after stage one');
    const cut2 = nsims({ ...KNOBS, w1: 8, w2: 5, w3: 7, keep2: 1 });
    assert.equal(cut2.filter((n) => n === 20).length, 1, `keep2 1 lets one candidate into stage three (${cut2})`);
    assert.equal(cut2.filter((n) => n === 13).length, cut2.length - 1, 'and stops the rest after stage two');

    // …and applying REPLACES the table: the cut above is gone the moment the
    // next set does not ask for it, with no knob left over from the last batch.
    assert.deepEqual(flat(nsims(KNOBS)), [9], 'the earlier keep1 / keep2 / w2 / w3 did not survive');

    // solve_budget: the exact probe's nodes per root move. At the full budget
    // this endgame is proven; at zero the probe still runs and proves nothing.
    inst.setKnobs(KNOBS);
    const endgame = await provenEndgame(inst);
    const verdicts = (budget: number) => {
        inst.setKnobs({ ...KNOBS, solveBudget: budget });
        const r = inst.analyzeOnce(endgame, 77);
        assert.ok('record' in r, 'the choose answered');
        assert.equal(r.record.solver.applied, 1, 'the probe ran');
        return r.record.candidates.map((c) => c.verdict);
    };
    assert.ok(verdicts(ORACLE_KNOBS.solveBudget).some((v) => v === 'win' || v === 'loss'), 'proven at the full budget');
    assert.ok(verdicts(0).every((v) => v === 'unknown'), 'nothing is proven on a budget of zero');
});

test('Mode B is set by the same struct: its batch is the knobs it was handed', async () => {
    const job = await narrowDecision();
    for (const w1 of [8, 20]) {
        const rig = await openMtRig(job, 2, { ...ORACLE_KNOBS, w1 });
        try {
            await rig.run(job, oracleSeedBase(job.decisionId), 400);
            const batches = rig.session.batches();
            const rows = rig.session.readCandidates(job, false);
            assert.ok(batches > 0 && rows.length >= 5, 'the threads deliberated a decision with several candidates');
            // Racing off (keep1 / keep2) makes every candidate's count the same,
            // and w1 / w2 / w3 make it w1 + 1 worlds a batch.
            for (const c of rows) assert.equal(c.n, batches * (w1 + 1), `${c.label}: ${c.n} worlds over ${batches} batches at w1 ${w1}`);
        } finally {
            await rig.close();
        }
    }
});
