/* =============================================================================
 * Infinite Oracle — headless suite (docs/INFINITE_ORACLE_DESIGN.md §12.2)
 * Loads the committed public/oracle.wasm.gz and drives the REAL src/oracle
 * modules (logsWire, replayOracleInput, accumulator) + the OracleInstance
 * bridge against three replays. No Postgres, no browser. Validates the hardest
 * reconstruction (defender/goods/elimination), batching, the memory toggle, the
 * exact endgame regime, and the env-reload hook.
 *
 * The replays are now PLAYED here (helpers/seeded_game.ts) rather than frozen as
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

import { decodeReplay } from '../supabase/functions/_shared/replay/decode.ts';
import { bytesToBigint } from '../supabase/functions/_shared/replay/codec.ts';
import { gunzip } from '../supabase/functions/_shared/wasm/gunzip.ts';
import { buildReplayFrames } from '../src/replay/frames.ts';
import { buildOracleJob, findDecisionIndex } from '../src/oracle/replayOracleInput.ts';
import { OracleInstance } from '../src/oracle/oracleBridge.ts';
import { OracleAccumulator } from '../src/oracle/accumulator.ts';
import { playSeededV6 } from './helpers/seeded_game.ts';

const ORACLE_BYTES = gunzip(new Uint8Array(readFileSync('public/oracle.wasm.gz')));
const ENV_BASE = { OG_KEEP1: '26', OG_KEEP2: '26', OG_W2: '1', OG_W3: '0', OG_EXPLAIN_SOLVE_BUDGET: '2000000' };

// The three shapes that stress the marshal differently: heads-up-ish, the
// 4-player middle, and a full 8-way table (52-card deck, most eliminations).
const SHAPES: [string, number, number][] = [['3p', 3, 41], ['4p', 4, 42], ['8p', 8, 43]];

interface Fixture { decoded: Awaited<ReturnType<typeof decodeReplay>>; frames: ReturnType<typeof buildReplayFrames>; id: string }
const cache = new Map<string, Fixture>();

async function fixture(label: string, np: number, s: number): Promise<Fixture> {
    const hit = cache.get(label);
    if (hit) return hit;
    const played = await playSeededV6(np, s);
    assert.ok(played, `${label}: the seeded game finished`);
    const decoded = await decodeReplay(bytesToBigint(played!.code));
    const frames = buildReplayFrames(played!.code, 'g', null, { fool: decoded.fool });
    const f = { decoded, frames, id: label };
    cache.set(label, f);
    return f;
}
async function freshInstance() {
    const inst = new OracleInstance();
    await inst.init(ORACLE_BYTES);
    return inst;
}

test('§12.2-1 reconstruction: every decision marshals to a well-formed deliberation', async () => {
    const inst = await freshInstance();
    let decisions = 0;
    for (const [label, np, seed] of SHAPES) {
        const { decoded, frames, id } = await fixture(label, np, seed);
        const seen = new Set<number>();
        for (let idx = 0; idx < frames.length; idx++) {
            const j = findDecisionIndex(frames, idx);
            if (j == null || seen.has(j)) continue;
            seen.add(j);
            const job = buildOracleJob(frames, decoded, idx, true, id);
            assert.ok(job, `job at step ${idx} of ${id}`);
            inst.writeEnv({ ...ENV_BASE, OG_W1: '8' });
            const r = inst.analyzeOnce(job!.gameBlob, job!.seat, job!.logsWire, true, 0x51 + j);
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

test('§12.2-2 batching: keys stable, n increases, worlds vary across seeds', async () => {
    const inst = await freshInstance();
    const { decoded, frames, id } = await fixture('4p', 4, 42);
    const idx = Math.floor(frames.length * 0.4);
    const job = buildOracleJob(frames, decoded, idx, true, id)!;
    assert.ok(job);
    const acc = new OracleAccumulator({ deckAlive: job.deckAlive, recordedKey: job.recordedKey });
    inst.writeEnv({ ...ENV_BASE, OG_W1: '24' });
    let prevKeys = '';
    const means: number[] = [];
    for (let b = 0; b < 5; b++) {
        const r = inst.analyzeOnce(job.gameBlob, job.seat, job.logsWire, true, 1009 + b * 7919);
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
    const { decoded, frames, id } = await fixture('3p', 3, 41);
    // find a late heads-up decision where memory ON reaches the exact solver
    let found = false;
    for (let idx = frames.length - 1; idx >= frames.length - 12 && !found; idx--) {
        const j = findDecisionIndex(frames, idx);
        if (j == null || j !== idx) continue;
        const run = (memoryOn: boolean) => {
            const job = buildOracleJob(frames, decoded, idx, memoryOn, id)!;
            const acc = new OracleAccumulator({ deckAlive: job.deckAlive, recordedKey: job.recordedKey });
            inst.writeEnv({ ...ENV_BASE, OG_W1: '16' });
            for (let b = 0; b < 2; b++) {
                const r = inst.analyzeOnce(job.gameBlob, job.seat, job.logsWire, memoryOn, 31 + b);
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
    const { decoded, frames, id } = await fixture('3p', 3, 41);
    let sawExact = false;
    for (let idx = frames.length - 1; idx >= frames.length - 8; idx--) {
        const j = findDecisionIndex(frames, idx);
        if (j == null || j !== idx) continue;
        const job = buildOracleJob(frames, decoded, idx, true, id)!;
        inst.writeEnv({ ...ENV_BASE, OG_W1: '8' });
        const r = inst.analyzeOnce(job.gameBlob, job.seat, job.logsWire, true, 77);
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

test('§12.2-6 env reload: raising OG_W1 grows nsim between batches', async () => {
    const inst = await freshInstance();
    const { decoded, frames, id } = await fixture('4p', 4, 42);
    const idx = Math.floor(frames.length * 0.4);
    const job = buildOracleJob(frames, decoded, idx, true, id)!;
    const nsimAt = (w1: string) => {
        inst.writeEnv({ ...ENV_BASE, OG_W1: w1 });
        const r = inst.analyzeOnce(job.gameBlob, job.seat, job.logsWire, true, 5);
        const rec = (r as any).record;
        return Math.max(...rec.candidates.map((c: any) => c.nsim || 0));
    };
    const small = nsimAt('8');
    const big = nsimAt('32');
    assert.ok(big > small, `nsim grew after reload (${small} -> ${big})`);
    console.log(`  §12.2-6: OG_W1 8->32 grew nsim ${small} -> ${big}`);
});
