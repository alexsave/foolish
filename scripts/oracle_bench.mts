/* =============================================================================
 * Infinite Oracle - Mode A vs Mode B, measured (§8b.7)
 * =============================================================================
 * Mode A is an INSTANCE FLEET: N private linear memories, and every batch pays a
 * marshal, a strategy-key write, a log import and a JSON dump it then parses.
 * Mode B is ONE shared memory: the job is marshaled once and the threads fold
 * integers into a C accumulator. This measures what that is worth, in octogen
 * choose-calls per second at the same OG_W1 and the same thread count.
 *
 *   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx scripts/oracle_bench.mts
 *   BENCH_THREADS=4 BENCH_SECONDS=6 ... node --import tsx scripts/oracle_bench.mts
 *
 * Mode A's fleet is stood up as node:worker_threads running PLAIN JS against
 * oracle.wasm: the marshaled state bytes are captured once on the main thread
 * and memcpy'd per batch, so the worker does exactly Mode A's per-batch wasm
 * work (import state, keys, logs, seed, choose, read + JSON.parse the dump) and
 * skips only the JS marshal's arithmetic - which is dwarfed by the octogen
 * compute and the dump parse this measures.
 * ========================================================================== */

import { readFileSync } from 'node:fs';
import os from 'node:os';
import { Worker } from 'node:worker_threads';

import { decodeReplay } from '@api/common/replay/decode.ts';
import { bytesToBigint } from '@api/common/replay/codec.ts';
import { gunzip } from '@sdk/ts/wasm/gunzip.ts';
import { __marshalGame, __mem, __setResident } from '@sdk/ts/wasm/engine.ts';
import { buildReplayFrames } from '../src/replay/frames.ts';
import { buildOracleJob } from '../src/oracle/replayOracleInput.ts';
import { ORACLE_MT_ENV, oracleSeedBase } from '../src/oracle/oracleMtSession.ts';
import { openMtRig } from '../e2e/helpers/oracle_mt_node.ts';
import { playSeededV6 } from '../e2e/helpers/seeded_game.ts';

const SECONDS = Number(process.env.BENCH_SECONDS || '5');
const THREADS = Number(process.env.BENCH_THREADS || String(Math.max(1, os.cpus().length - 2)));

async function buildJob() {
    const played = await playSeededV6(4, 42);
    if (!played) throw new Error('the seeded game did not finish');
    const decoded = await decodeReplay(bytesToBigint(played.code));
    const frames = buildReplayFrames(played.code, 'g', null, { fool: decoded.fool });
    const idx = Math.floor(frames.length * 0.4);
    const job = buildOracleJob(frames, decoded, idx, true, 'bench');
    if (!job) throw new Error('no decision at the sampled step');
    return job;
}

/** Capture the exact bytes __marshalGame writes, so the plain-JS Mode A workers
 *  can replay the import without importing engine.ts. */
async function captureMarshal(job: Awaited<ReturnType<typeof buildJob>>) {
    const bytes = gunzip(new Uint8Array(readFileSync('public/oracle.wasm.gz')));
    const src = await WebAssembly.instantiate(bytes as BufferSource, {});
    const ex = (src as WebAssembly.WebAssemblyInstantiatedSource).instance.exports as never;
    (ex as { wasm_init(): void }).wasm_init();
    __setResident(null);
    __marshalGame(ex, job.gameBlob as never);
    const io = (ex as { wasm_io_ptr(): number }).wasm_io_ptr();
    return { bytes, stateBytes: new Uint8Array(__mem(ex).slice(io, io + 16384)) };
}

async function runModeA(job: Awaited<ReturnType<typeof buildJob>>): Promise<number> {
    const { bytes, stateBytes } = await captureMarshal(job);
    const counts = await Promise.all(Array.from({ length: THREADS }, (_, tid) =>
        new Promise<number>((resolve, reject) => {
            const w = new Worker(new URL('../e2e/helpers/oracle_a_thread.mjs', import.meta.url), {
                workerData: {
                    bytes, stateBytes, seat: job.seat, numPlayers: job.numPlayers,
                    logsWire: job.logsWire, memoryOn: job.memoryOn,
                    env: ORACLE_MT_ENV, seconds: SECONDS, tid,
                },
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
console.log(`decision seat ${job.seat}, memoryOn ${job.memoryOn}, OG_W1=${ORACLE_MT_ENV.OG_W1}, ${THREADS} threads, ${SECONDS}s each\n`);

console.log('Mode A (instance fleet: import state + choose + JSON dump per batch)...');
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
