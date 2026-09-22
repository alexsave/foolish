/* =============================================================================
 * Infinite Oracle — headless suite (docs/INFINITE_ORACLE_DESIGN.md §12.2)
 * Loads the committed public/oracle.wasm.gz and drives the REAL src/oracle
 * modules (replayOracleInput, accumulator) + the OracleInstance
 * bridge against three replays. No Postgres, no browser. Validates the hardest
 * reconstruction (defender/goods/elimination), batching, the memory toggle, the
 * exact endgame regime, and the env-reload hook.
 *
 * The replays are PLAYED here (a table of `handwritten` bots on the C Table,
 * helpers/bot_table.ts playBotTable) rather than frozen as
 * base32 constants, and they are v6 rather than v5. Two reasons, both learned
 * the hard way:
 *
 *   - The frozen codes rotted. A replay code is only readable by the kernel that
 *     cut it, so a legal-move menu change orphans it; octogen-4v4 had been dead
 *     ("leftover data after game end") with this suite red and unnoticed. A
 *     played game is cut by the kernel under test and cannot go stale.
 *
 *   - The Oracle's position now comes from the frames the engine really replayed
 *     (A5), which v5 cannot produce: it hides the deal, so its hands are
 *     retrodiction. The position used to be that guess. It is not any more.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

import { gunzip } from '../sdk/ts/wasm/gunzip.ts';
import { buildReplayFrames, REPLAY_STEP } from '../src/replay/frames.ts';
import { buildOracleJob, findDecisionIndex } from '../src/oracle/replayOracleInput.ts';
import { OracleInstance } from '../src/oracle/oracleBridge.ts';
import { OracleAccumulator } from '../src/oracle/accumulator.ts';
import { playBotTable, seedBytes } from './helpers/bot_table.ts';

const ORACLE_BYTES = gunzip(new Uint8Array(readFileSync('public/oracle.wasm.gz')));
const ENV_BASE = { OG_KEEP1: '26', OG_KEEP2: '26', OG_W2: '1', OG_W3: '0', OG_EXPLAIN_SOLVE_BUDGET: '2000000' };

// The three shapes that stress the marshal differently: heads-up-ish, the
// 4-player middle, and a full 8-way table (52-card deck, most eliminations).
const hw = (n: number): string[] => Array(n).fill('handwritten');
const SHAPES: [string, string[], number][] =
    [['3p', hw(3), 41], ['4p', hw(4), 42], ['8p', hw(8), 43]];
// THE SHAPES ABOVE ARE ALL HANDWRITTEN, AND A TABLE OF ONE BOT PLAYS ONE KIND
// OF BOARD. handwritten never picks up more than it must, so no seat in those
// three games ever holds the twenty-card hand that a run of pickups builds -
// and a rich hand is exactly where a candidate list runs out of places. Over
// the three of them the candidate check below was green while octogen could
// not offer a single-card attack from a hand holding four 3s, four 9s, four
// 10s and four Ks: nothing in the suite was ever dealt that hand.
//
// `random` is dealt it constantly, because it picks up for no reason. These
// two games are in the survey for their BOARDS, not their play: they are the
// cheapest way to hold octogen's candidate selection against hands it would
// otherwise only meet in front of a human.
const WIDE: [string, string[], number][] =
    [['5p-random', Array(5).fill('random'), 105], ['8p-random', Array(8).fill('random'), 101]];
// A MIXED TABLE, because a pass needs a defender who holds several of one value
// and an attacker willing to send it. Six seats of one bot rarely stage that;
// these six do, and §12.2-1c is what they are here for.
//
// THE SEED MOVED 750 -> 752 WHEN A GOOD BECAME A MOVE, and the case below is the
// one that noticed, exactly as its own closing comment said it would ("if the
// deal or the bot cycle ever stops producing one"). Nothing about the deal or
// the rules changed: classify() (c/src/bot_drive.c) stopped bundling a silent
// `good` into the cycle that follows it, so a good now ENDS its cycle, the next
// cycle re-collects and re-shuffles the eligible seats, and from the first good
// onward these six bots play a different (equally legal) game. Seed 750's board
// no longer stages a defender holding three of the attack's value; 752 does,
// with seven legal passes at its widest over five pass decisions - measured, not
// guessed, and the assertion at the bottom is what holds it there.
const PASS_BOARD = ['handwritten', 'octogen', 'random', 'handwritten', 'octogen', 'random'];
/** REPLAY_STEP id -> its name, so a failure names the move type it broke on. */
const STEP_NAME: Record<number, string> =
    Object.fromEntries(Object.entries(REPLAY_STEP).map(([k, v]) => [v as number, k]));

interface Fixture { code: Uint8Array; frames: ReturnType<typeof buildReplayFrames>; id: string }
const cache = new Map<string, Fixture>();

async function fixture(label: string, brains: string[], s: number): Promise<Fixture> {
    const hit = cache.get(label);
    if (hit) return hit;
    const played = playBotTable(brains, seedBytes(brains.length, s));
    const frames = buildReplayFrames(played.code, 'g', null);
    const f = { code: played.code, frames, id: label };
    cache.set(label, f);
    return f;
}
async function freshInstance() {
    const inst = new OracleInstance();
    await inst.init(ORACLE_BYTES);
    return inst;
}

test('§12.2-1 reconstruction: every decision imports to a well-formed deliberation', async () => {
    const inst = await freshInstance();
    let decisions = 0;
    for (const [label, brains, seed] of SHAPES) {
        const { code, frames, id } = await fixture(label, brains, seed);
        const seen = new Set<number>();
        for (let idx = 0; idx < frames.length; idx++) {
            const j = findDecisionIndex(frames, idx);
            if (j == null || seen.has(j)) continue;
            seen.add(j);
            const job = buildOracleJob(frames, code, idx, true, id);
            assert.ok(job, `job at step ${idx} of ${id}`);
            inst.writeEnv({ ...ENV_BASE, OG_W1: '8' });
            const r = inst.analyzeOnce(job!, 0x51 + j);
            assert.ok('record' in r, `dump parsed at j=${j} (${id})`);
            const rec = (r as { record: any }).record;
            assert.equal(rec.seat, job!.seat, 'dumped seat matches acting seat');
            assert.ok(Array.isArray(rec.candidates) && rec.candidates.length > 0, 'candidates present');
            // EF floor: after k eliminations, every finish is >= k+1, so no
            // candidate mean can be below k+1 (catches an empty elimination_order).
            for (const c of rec.candidates) {
                if (typeof c.score === 'number') {
                    assert.ok(c.score >= job!.eliminations + 1 - 1e-6,
                        `EF ${c.score} >= ${job!.eliminations + 1} at j=${j} (${id})`);
                }
            }
            decisions++;
        }
    }
    assert.ok(decisions > 30, `covered ${decisions} decisions`);
    console.log(`  §12.2-1: validated ${decisions} decisions across 3 replays`);
});

// THE PROMISE IS THAT WIDTH NEVER DECIDES, AND THE ONLY EXCUSE IS A FULL TABLE.
// og_pick_candidates ranks attacks and covers into one list PER WIDTH and takes
// them round-robin, so a move is never dropped for being wide or for being
// narrow. For an oracle build those per-width lists are as deep as the whole
// table and a pass keeps all fifteen a hand can ever offer, so neither is a cap
// any more - they order, they do not budget. The one thing that can still drop
// a recorded move is OG_MAX_CANDS itself, on a board that offered more replies
// than the table has places.
//
// It was three caps and three excuses, and each one hid a real defect behind a
// legitimate-looking budget. Sixteen places per width dropped a played double
// on a board that left eighteen of its sixty-four places EMPTY, because only
// the double bucket was full. Six pass places dropped a played single pass out
// of seven legal ones. Both read as "budget spent" to the rule this test used
// to apply. Walking 3,687 decisions of 38 bot games and two shared human
// replays, that rule excused 8 misses; under one cap there are none.
//
// This is the C constant for an oracle build (c/src/octogen_strategy.c,
// -DFOOLISH_ORACLE_BUILD). If it moves there it must move here, and the test
// failing is how you find out.
const ORACLE_MAX_CANDS = 128;   // OG_MAX_CANDS

/** How many candidates the table holds when the recorded move is not among
 *  them, or null when the table was not full and the miss is a defect rather
 *  than a budget. */
function tableFull(keys: string[]): number | null {
    return keys.length >= ORACLE_MAX_CANDS ? keys.length : null;
}

test('§12.2-1b the recorded move is one of the candidates, at every decision', async () => {
    // The panel names the move that was actually played by looking its canonical
    // key up among the candidates (accumulator: `played: a.key === recordedKey`),
    // and when the lookup misses it prints "<move> - not considered" under the
    // rows. So a key built on one side that the other side can never emit is not
    // a near miss - it is the panel confidently saying the opposite of the truth.
    //
    // That is what shipped for pickup. A replay frame for a pickup carries the
    // cards swept off the table, so the recorded key came out
    // `pickup|10H,10S*,7C,...|` while the dump only ever emits `pickup||`: the
    // pickup row sat at the top of the panel marked BEST while the footer said
    // it was never considered. 19 of 19 pickups, against 0 of 96 attacks, 0 of
    // 97 covers and 0 of 13 passes - which is why this walks EVERY decision and
    // counts per move type rather than sampling one.
    const inst = await freshInstance();
    const missing: Record<string, number> = {};
    const budgeted: Record<string, number> = {};
    const total: Record<string, number> = {};
    for (const [label, brains, seed] of [...SHAPES, ...WIDE]) {
        const { code, frames, id } = await fixture(label, brains, seed);
        for (let j = 1; j < frames.length; j++) {
            if (findDecisionIndex(frames, j) !== j) continue;   // the decision itself
            const job = buildOracleJob(frames, code, j, true, id);
            if (!job) continue;
            inst.writeEnv({ ...ENV_BASE, OG_W1: '8' });
            const r = inst.analyzeOnce(job, 0x9e37 + j);
            if (!('record' in r)) continue;
            const acc = new OracleAccumulator({ deckAlive: job.deckAlive, recordedKey: job.recordedKey });
            acc.add((r as { record: any }).record, (r as { paths?: ArrayBuffer }).paths);
            const kind = STEP_NAME[frames[j].kind] ?? String(frames[j].kind);
            total[kind] = (total[kind] ?? 0) + 1;
            if (!acc.hasKey(job.recordedKey)) {
                const keys = acc.candidates(false).map((c) => c.key);
                const full = tableFull(keys);
                if (full == null) {
                    missing[kind] = (missing[kind] ?? 0) + 1;
                    assert.fail(`${id} step ${j}: recorded ${kind} ${JSON.stringify(job.recordedKey)} `
                        + `is not among ${JSON.stringify(keys)}`);
                }
                budgeted[kind] = (budgeted[kind] ?? 0) + 1;
            }
        }
    }
    // Every decision kind the Oracle deliberates has to be exercised, or a
    // regression in the one that is missing passes unnoticed - which is exactly
    // how the pickup bug survived.
    for (const kind of ['ATTACK', 'COVER', 'PASS', 'PICKUP']) {
        assert.ok((total[kind] ?? 0) > 0, `no ${kind} decision covered - the fixtures stopped exercising it`);
    }
    const counts = Object.keys(total).sort().map((k) => `${k} ${total[k]}`).join(', ');
    const cut = Object.keys(budgeted).sort().map((k) => `${k} ${budgeted[k]}`).join(', ') || 'none';
    console.log(`  §12.2-1b: recorded move found at every decision (${counts}); cut by a full bucket: ${cut}`);
    assert.deepEqual(missing, {});
});

test('§12.2-1c every pass a hand can make is on the panel', async () => {
    // A PASS IS NOT RANKED AGAINST A BUDGET, because there is no budget it can
    // exceed. A pass spends cards of the attack's value, a hand holds at most
    // four of any value, so the legal passes are the non-empty subsets of those:
    // fifteen at the very most, and an oracle build keeps fifteen.
    //
    // Six places is what it kept, and passes rank width-first, so the singles
    // are what fell off. This 6p board is the case in miniature: a defender
    // holding 3C, 3H and 3S facing an attack of 3s has seven legal passes, the
    // panel listed the wider six, and the one it dropped - `pass|3C|` - is the
    // one that was played. It printed "not considered" under a move it had
    // just animated, and octogen, sharing this selection, could not have chosen
    // it either.
    const inst = await freshInstance();
    const { code, frames, id } = await fixture('6p-pass', PASS_BOARD, 752);
    let widest = 0, walked = 0;
    for (let j = 1; j < frames.length; j++) {
        if (findDecisionIndex(frames, j) !== j) continue;
        if (frames[j].kind !== REPLAY_STEP.PASS) continue;
        const job = buildOracleJob(frames, code, j, true, id);
        if (!job) continue;
        inst.writeEnv({ ...ENV_BASE, OG_W1: '8' });
        const r = inst.analyzeOnce(job, 0x9e37 + j);
        if (!('record' in r)) continue;
        walked++;
        const acc = new OracleAccumulator({ deckAlive: job.deckAlive, recordedKey: job.recordedKey });
        acc.add((r as { record: any }).record, (r as { paths?: ArrayBuffer }).paths);
        const passes = acc.candidates(false).filter((c) => String(c.key).startsWith('pass|')).length;
        widest = Math.max(widest, passes);
        assert.ok(acc.hasKey(job.recordedKey),
            `${id} step ${j}: recorded ${JSON.stringify(job.recordedKey)} is not among `
            + JSON.stringify(acc.candidates(false).map((c) => c.key)));
    }
    // AND THE BOARD IS STILL THE BOARD. Seven legal passes is what makes this
    // game worth walking; if the deal or the bot cycle ever stops producing one,
    // the assertion above would pass while proving nothing.
    assert.ok(widest >= 7, `the 6p pass board no longer offers more than six passes (widest ${widest} over ${walked} pass decisions)`);
    console.log(`  §12.2-1c: ${walked} pass decisions, widest board offered ${widest} passes`);
});

test('§12.2-2 batching: keys stable, n increases, worlds vary across seeds', async () => {
    const inst = await freshInstance();
    const { code, frames, id } = await fixture('4p', hw(4), 42);
    const idx = Math.floor(frames.length * 0.4);
    const job = buildOracleJob(frames, code, idx, true, id)!;
    assert.ok(job);
    const acc = new OracleAccumulator({ deckAlive: job.deckAlive, recordedKey: job.recordedKey });
    inst.writeEnv({ ...ENV_BASE, OG_W1: '24' });
    let prevKeys = '';
    const means: number[] = [];
    for (let b = 0; b < 5; b++) {
        const r = inst.analyzeOnce(job, 1009 + b * 7919);
        assert.ok('record' in r);
        const rec = (r as { record: any }).record;
        const keys = rec.candidates.map((c: any) => `${c.type}|${c.cards.join()}`).sort().join(';');
        if (b > 0) assert.equal(keys, prevKeys, 'candidate set stable across batches');
        prevKeys = keys;
        // nsim uniform across candidates within a batch (racing off)
        const scored = rec.candidates.filter((c: any) => c.nsim > 0);
        const nsims = new Set(scored.map((c: any) => c.nsim));
        assert.equal(nsims.size, 1, 'nsim uniform per batch');
        const before = acc.totalWorlds;
        acc.add(rec);
        assert.ok(acc.totalWorlds > before, 'cumulative worlds strictly increase');
        const best = acc.candidates(false)[0];
        if (best.mean != null) means.push(best.mean);
    }
    assert.ok(new Set(means.map((m) => m.toFixed(4))).size > 1, 'fresh seeds vary the estimate');
    console.log(`  §12.2-2: 5 batches, ${acc.totalWorlds} worlds, best EF ~${means.at(-1)?.toFixed(3)}`);
});

test('§12.2-4 memory toggle: ON proves an endgame verdict that OFF cannot', async () => {
    const inst = await freshInstance();
    const { code, frames, id } = await fixture('3p', hw(3), 41);
    // find a late heads-up decision where memory ON reaches the exact solver
    let found = false;
    for (let idx = frames.length - 1; idx >= frames.length - 12 && !found; idx--) {
        const j = findDecisionIndex(frames, idx);
        if (j == null || j !== idx) continue;
        const run = (memoryOn: boolean) => {
            const job = buildOracleJob(frames, code, idx, memoryOn, id)!;
            const acc = new OracleAccumulator({ deckAlive: job.deckAlive, recordedKey: job.recordedKey });
            inst.writeEnv({ ...ENV_BASE, OG_W1: '16' });
            for (let b = 0; b < 2; b++) {
                const r = inst.analyzeOnce(job, 31 + b);
                if ('record' in r) acc.add((r as any).record);
            }
            return acc.hasWinLoss();
        };
        const on = run(true);
        const off = run(false);
        if (on && !off) {
            found = true;
            console.log(`  §12.2-4: at j=${j} memory ON proves a verdict, OFF does not`);
        }
    }
    assert.ok(found, 'a late decision shows the memory-on-only endgame proof');
});

test('§12.2-5 exact regime: a proven win/loss verdict appears near the end', async () => {
    const inst = await freshInstance();
    const { code, frames, id } = await fixture('3p', hw(3), 41);
    let sawExact = false;
    for (let idx = frames.length - 1; idx >= frames.length - 8; idx--) {
        const j = findDecisionIndex(frames, idx);
        if (j == null || j !== idx) continue;
        const job = buildOracleJob(frames, code, idx, true, id)!;
        inst.writeEnv({ ...ENV_BASE, OG_W1: '8' });
        const r = inst.analyzeOnce(job, 77);
        if (!('record' in r)) continue;
        const rec = (r as any).record;
        if (rec.solver?.applied && rec.candidates.some((c: any) => c.verdict === 'win' || c.verdict === 'loss')) {
            sawExact = true;
            break;
        }
    }
    assert.ok(sawExact, 'the endgame solver proves a win/loss at some late decision');
    console.log('  §12.2-5: exact win/loss verdict observed');
});

test('§12.2-7 why sidecar: binary paths decode, merge, and template into a proof', async () => {
    const { decodePathsBlob } = await import('../src/oracle/pathsBlob.ts');
    const { explainCandidate } = await import('../src/oracle/explain.ts');
    // The English table, which is now generated from c/i18n rather than written
    // in strings.ts. `translate` is the site's own substitution, so this test
    // fills placeholders exactly the way the browser does.
    const { EN, translate } = await import('../src/localization/strings.ts');
    const t = (id: string, params?: Record<string, string | number>) =>
        translate(EN, id as never, Object.fromEntries(
            Object.entries(params ?? {}).map(([k, v]) => [k, String(v)])));

    const inst = await freshInstance();
    const { code, frames, id } = await fixture('4p', hw(4), 42);
    const idx = Math.floor(frames.length * 0.4);
    const job = buildOracleJob(frames, code, idx, true, id)!;
    const acc = new OracleAccumulator({ deckAlive: job.deckAlive, recordedKey: job.recordedKey });
    inst.writeEnv({ ...ENV_BASE, OG_W1: '24' });
    let sawSidecar = false;
    for (let b = 0; b < 3; b++) {
        const r = inst.analyzeOnce(job, 4242 + b * 101);
        assert.ok('record' in r, 'record parsed');
        const { record, paths } = r as { record: any; paths?: ArrayBuffer };
        // the sidecar must be present whenever the record carries MC scores
        const scored = record.candidates.filter((c: any) => c.nsim > 0);
        if (scored.length > 0) {
            assert.ok(paths && paths.byteLength >= 8, 'binary sidecar present');
            sawSidecar = true;
            const blob = decodePathsBlob(paths!);
            assert.ok(blob.size > 0, 'sidecar decodes to entries');
            // every entry indexes a real candidate and carries sane counts
            for (const [k, w] of blob) {
                assert.ok(k < record.candidates.length, 'entry indexes a candidate');
                assert.ok(w.agg.n > 0, 'entry has folded playouts');
                const pathN = w.paths.reduce((sum: number, pth) => sum + pth.n, 0);
                assert.ok(pathN <= w.agg.n, 'cluster counts bounded by total');
                for (const pth of w.paths) {
                    assert.ok(pth.seq.every((sym: number) => sym >= 1 && sym <= 4), 'symbols in 1..4');
                    assert.ok(pth.fin >= 1 && pth.fin <= job.numPlayers, `cluster fin ${pth.fin} in range`);
                }
            }
        }
        acc.add(record, paths);
    }
    assert.ok(sawSidecar, 'at least one batch carried a sidecar');
    // belief context captured for the panel
    assert.ok(acc.belief, 'belief block captured');
    // the merged candidates carry why-data, and the generator produces prose
    const cands = acc.candidates(false);
    const best = cands.find((c) => c.mean != null) ?? null;
    const withWhy = cands.filter((c) => c.why && c.why.agg.n > 0);
    assert.ok(withWhy.length > 0, 'merged candidates carry why-data');
    let proofs = 0;
    for (const c of withWhy) {
        const snapshot = {
            numPlayers: job.numPlayers,
            seat: job.seat,
            belief: {
                pinned: acc.belief!.pinned, voids: acc.belief!.voids, floor: acc.belief!.floor,
                poolCount: acc.belief!.pool.length,
                hand: [], oppCounts: [], table: [], defender: 0,
                trump: acc.beliefCtx?.trump ?? -1,
            },
        } as never;
        const why = explainCandidate(t, c, best, snapshot);
        assert.ok(why, `explanation built for ${c.label}`);
        assert.ok(why!.headline.length > 0, 'headline text');
        assert.ok(!why!.headline.includes('{'), 'no unfilled template params');
        for (const line of why!.proof) {
            assert.ok(line.includes('%'), 'proof cites a probability');
            assert.ok(!line.includes('{'), 'no unfilled template params in the proof');
            assert.ok(!line.includes('oracle_'), 'no raw string ids leak');
        }
        if (why!.proof.length > 0) proofs++;
        if (why!.tree.length > 0) {
            const share = why!.tree.reduce((sum, nd) => sum + nd.share, 0);
            assert.ok(share <= 1.0001, 'tree shares bounded');
        }
    }
    assert.ok(proofs > 0, 'at least one candidate has a storyline proof');
    console.log(`  §12.2-7: sidecar decoded; ${withWhy.length} candidates explained, ${proofs} with storyline proofs`);
});

test('§12.2-6 env reload: raising OG_W1 grows nsim between batches', async () => {
    const inst = await freshInstance();
    const { code, frames, id } = await fixture('4p', hw(4), 42);
    const idx = Math.floor(frames.length * 0.4);
    const job = buildOracleJob(frames, code, idx, true, id)!;
    const nsimAt = (w1: string) => {
        inst.writeEnv({ ...ENV_BASE, OG_W1: w1 });
        const r = inst.analyzeOnce(job, 5);
        const rec = (r as any).record;
        return Math.max(...rec.candidates.map((c: any) => c.nsim || 0));
    };
    const small = nsimAt('8');
    const big = nsimAt('32');
    assert.ok(big > small, `nsim grew after reload (${small} -> ${big})`);
    console.log(`  §12.2-6: OG_W1 8->32 grew nsim ${small} -> ${big}`);
});
