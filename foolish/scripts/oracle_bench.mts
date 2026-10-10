/* =============================================================================
 * Infinite Oracle - Mode A vs Mode B, measured (§8b.7)
 * =============================================================================
 * Mode A is an INSTANCE FLEET: N private linear memories, and every batch pays a
 * state import, a strategy-key write, a log import and an answer it then reads back.
 * Mode B is ONE shared memory: the job is marshaled once and the threads fold
 * integers into a C accumulator. This measures what that is worth, in octogen
 * choose-calls per second at the same w1 and the same thread count.
 *
 *   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx scripts/oracle_bench.mts
 *   BENCH_THREADS=4 BENCH_SECONDS=6 ... node --import tsx scripts/oracle_bench.mts
 *
 * Mode A's fleet is stood up as node:worker_threads, each driving its own
 * oracle.wasm through the replay screen's own bridge (src/oracle/oracleBridge.ts),
 * handed the job's kernel-written state and log bytes, so the worker does
 * exactly Mode A's per-batch work (import state, keys, logs, seed, choose, read
 * the answer).
 * ========================================================================== */

import { readFileSync } from 'node:fs';
import os from 'node:os';
import { Worker } from 'node:worker_threads';

import { gunzip } from '@sdk/ts/wasm/gunzip.ts';
import { buildReplayFrames } from '../src/replay/frames.ts';
import { buildOracleJob } from '../src/oracle/replayOracleInput.ts';
import { ORACLE_MT_KNOBS, oracleSeedBase } from '../src/oracle/oracleMtSession.ts';
import { openMtRig } from '../e2e/helpers/oracle_mt_node.ts';
import { seededCode } from '../e2e/helpers/seeded_codes.ts';

const SECONDS = Number(process.env.BENCH_SECONDS || '5');
const THREADS = Number(process.env.BENCH_THREADS || String(Math.max(1, os.cpus().length - 2)));

async function buildJob() {
    const played = { code: seededCode(4, 42) };
    const frames = buildReplayFrames(played.code, 'g', null);
    const idx = Math.floor(frames.length * 0.4);
    const job = buildOracleJob(frames, played.code, idx, true, 'bench');
    if (!job) throw new Error('no decision at the sampled step');
    return job;
}

async function runModeA(job: Awaited<ReturnType<typeof buildJob>>): Promise<number> {
    const bytes = gunzip(new Uint8Array(readFileSync('public/oracle.wasm.gz')));
    const counts = await Promise.all(Array.from({ length: THREADS }, (_, tid) =>
        new Promise<number>((resolve, reject) => {
            const w = new Worker(new URL('../e2e/helpers/oracle_a_thread.mjs', import.meta.url), {
                workerData: { bytes, job, knobs: ORACLE_MT_KNOBS, seconds: SECONDS, tid },
            });
            w.on('message', (m: { chooses: number }) => { resolve(m.chooses); w.terminate(); });
            w.on('error', reject);
        })));
    return counts.reduce((a, b) => a + b, 0);
}

async function runModeB(job: Awaited<ReturnType<typeof buildJob>>): Promise<number> {
    const rig = await openMtRig(job, THREADS);
    await rig.run(job, oracleSeedBase(job.decisionId), SECONDS * 1000);
    const total = rig.session.totalChooses();
    await rig.close();
    return total;
}

const job = await buildJob();
console.log(`decision seat ${job.seat}, memoryOn ${job.memoryOn}, w1=${ORACLE_MT_KNOBS.w1}, ${THREADS} threads, ${SECONDS}s each\n`);

console.log('Mode A (instance fleet: import state + choose + read the answer per batch)...');
const aTotal = await runModeA(job);
const aRate = aTotal / SECONDS;
console.log(`  Mode A: ${aTotal.toLocaleString()} chooses  ->  ${Math.round(aRate).toLocaleString()} choose/s\n`);

console.log('Mode B (shared-memory threads: marshal once, C accumulator)...');
const bTotal = await runModeB(job);
const bRate = bTotal / SECONDS;
console.log(`  Mode B: ${bTotal.toLocaleString()} chooses  ->  ${Math.round(bRate).toLocaleString()} choose/s\n`);

const ratio = bRate / aRate;
console.log(`Mode B / Mode A = ${ratio.toFixed(2)}x  (${ratio > 1 ? 'Mode B faster' : 'Mode A faster'})`);
process.exit(0);
