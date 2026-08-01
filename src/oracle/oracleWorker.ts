/* =============================================================================
 * Infinite Oracle — Worker entry: protocol + batch loop (§8.5, §8.6)
 * One oracle.wasm instance per worker (Mode A: instance fleet). Loops
 * marshal -> seed -> choose -> read dump, posts each batch to the controller,
 * adapts OG_W1 to device speed, and honours the exact/forced/defuse stop rules.
 * Cancellation-safe: a new 'analyze' (or 'stop') bumps the generation and the
 * running loop exits at its next checkpoint.
 * ========================================================================== */

import { OracleInstance } from './oracleBridge';
import {
    MainToWorker, WorkerToMain, OracleJob,
    ORACLE_W1_START, ORACLE_W1_MIN, ORACLE_W1_MAX,
    ORACLE_BATCH_FAST_MS, ORACLE_BATCH_SLOW_MS, ORACLE_SOLVE_BUDGET,
} from './types';

const ctx = self as unknown as {
    postMessage(m: WorkerToMain, transfer?: Transferable[]): void;
    onmessage: ((e: MessageEvent<MainToWorker>) => void) | null;
};

let inst: OracleInstance | null = null;
let activeGen = -1;          // the generation the fleet should currently run

const post = (m: WorkerToMain) => ctx.postMessage(m);

function cryptoU32(): number {
    const a = new Uint32Array(1);
    globalThis.crypto.getRandomValues(a);
    return a[0] >>> 0;
}

ctx.onmessage = (e: MessageEvent<MainToWorker>) => {
    const msg = e.data;
    if (msg.t === 'init') {
        inst = new OracleInstance();
        inst.init(msg.bytes).then(() => post({ t: 'ready' }));
    } else if (msg.t === 'analyze') {
        activeGen = msg.gen;
        void runLoop(msg.job, msg.seedSalt, msg.gen);
    } else if (msg.t === 'stop') {
        activeGen = -1;      // cancels any running loop at its next checkpoint
    }
};

async function runLoop(job: OracleJob, seedSalt: number, gen: number): Promise<void> {
    if (!inst) return;

    // Fresh crypto-seeded xorshift32 per run, offset by the worker index so the
    // fleet never shares a world stream. Never 0 (§8.5 step 5).
    let seedState = ((seedSalt ^ cryptoU32() ^ 0x9e3779b9) >>> 0) || 0x1234567;
    const nextSeed = () => {
        let x = seedState;
        x ^= x << 13; x ^= x >>> 17; x ^= x << 5;
        seedState = x >>> 0;
        return seedState || 1;
    };

    let w1 = ORACLE_W1_START;
    let solveBudget = ORACLE_SOLVE_BUDGET;
    let envDirty = true;
    const env = () => ({
        OG_KEEP1: '26', OG_KEEP2: '26',            // racing OFF -> uniform nsim per batch
        OG_W1: String(w1), OG_W2: '1', OG_W3: '0', // batch = W1+1 worlds per candidate
        OG_EXPLAIN_SOLVE_BUDGET: String(solveBudget),
    });

    while (activeGen === gen) {
        if (envDirty) { inst.writeEnv(env()); envDirty = false; }

        const seed = nextSeed();
        const t0 = performance.now();
        const res = inst.analyzeOnce(job.gameBlob, job.seat, job.logsWire, job.memoryOn, seed);
        const batchMs = performance.now() - t0;

        if ('error' in res) {
            if (res.error === 'empty') post({ t: 'empty', decisionId: job.decisionId, gen });
            else post({ t: 'error', decisionId: job.decisionId, gen, message: res.error });
            return;
        }

        const rec = res.record;
        if (res.paths) {
            ctx.postMessage(
                { t: 'batch', decisionId: job.decisionId, record: rec, batchMs, gen, paths: res.paths },
                [res.paths],   // transfer, don't copy — binary stays zero-cost
            );
        } else {
            post({ t: 'batch', decisionId: job.decisionId, record: rec, batchMs, gen });
        }

        const solverApplied = !!(rec.solver && rec.solver.applied);
        const cands = rec.candidates || [];
        const hasWinLoss = cands.some((c) => c.verdict === 'win' || c.verdict === 'loss');

        // 11a EXACT — proven; stop the fleet.
        if (solverApplied && hasWinLoss) { post({ t: 'exact', decisionId: job.decisionId, gen }); return; }
        // 11b FORCED/SOLVED — nothing to accumulate; stop after this batch.
        const allZero = cands.length > 0 && cands.every((c) => (c.nsim || 0) === 0);
        if (allZero && !hasWinLoss) { post({ t: 'forced', decisionId: job.decisionId, gen }); return; }
        // 11c UNPROVEN-SOLVER DEFUSE — solver gate passes but proves nothing;
        // rewrite the probe budget to 0 (per-call getenv) so later batches are
        // pure-MC-priced, and keep batching.
        if (solverApplied && !hasWinLoss && solveBudget !== 0) { solveBudget = 0; envDirty = true; }

        // Adaptive batch sizing (§8.6): aim ~40 ms/batch.
        if (batchMs < ORACLE_BATCH_FAST_MS && w1 < ORACLE_W1_MAX) {
            w1 = Math.min(ORACLE_W1_MAX, w1 * 2); envDirty = true;
        } else if (batchMs > ORACLE_BATCH_SLOW_MS && w1 > ORACLE_W1_MIN) {
            w1 = Math.max(ORACLE_W1_MIN, Math.floor(w1 / 2)); envDirty = true;
        }

        // Yield so incoming stop/analyze messages are processed.
        await new Promise((r) => setTimeout(r, 0));
    }
}
