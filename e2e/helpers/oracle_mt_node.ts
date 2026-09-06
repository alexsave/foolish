/* =============================================================================
 * Mode B, driven headlessly (docs/INFINITE_ORACLE_DESIGN.md §8b.7)
 * =============================================================================
 * Node supports shared wasm memory and worker_threads natively, so the whole of
 * Mode B runs outside a browser - the SAME OracleMtSession the replay screen
 * uses, with node:worker_threads trampolines instead of Worker. That is the
 * point: the suite and the bench exercise the shipped object, not a re-typed
 * copy of it.
 * ========================================================================== */

import { readFileSync } from 'node:fs';
import { Worker } from 'node:worker_threads';
import { gunzip } from '@sdk/ts/wasm/gunzip.ts';
import { OracleJob } from '../../src/oracle/types.ts';
import { OracleMtSession, ORACLE_MT_ENV } from '../../src/oracle/oracleMtSession.ts';

export const ORACLE_MT_BYTES = (): Uint8Array =>
    gunzip(new Uint8Array(readFileSync('public/oracle-mt.wasm.gz')));

export interface MtRig {
    session: OracleMtSession;
    /** Arm a generation and let it run for `ms`, then park the threads. */
    run(job: OracleJob, seedBase: number, ms: number): Promise<void>;
    close(): Promise<void>;
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/** Load the module, prepare the job's shared state, and spawn N parked threads. */
export async function openMtRig(
    job: OracleJob,
    nThreads: number,
    env: Readonly<Record<string, string>> = ORACLE_MT_ENV,
): Promise<MtRig> {
    const session = new OracleMtSession();
    await session.load(ORACLE_MT_BYTES(), nThreads);
    const args = session.prepare(job, env);
    const bytes = session.moduleBytes!;
    const memory = session.memory!;

    const workers: Worker[] = [];
    await Promise.all(args.map((a) => new Promise<void>((resolve, reject) => {
        const w = new Worker(new URL('./oracle_mt_thread.mjs', import.meta.url), {
            workerData: { bytes, ...a },
        });
        workers.push(w);
        w.on('message', (m: { t: string; message?: string }) => {
            if (m.t === 'ready') resolve();
            else reject(new Error(m.message ?? 'oracle-mt worker failed'));
        });
        w.on('error', reject);
        // NB: never unref() - an unref'd worker can be reclaimed before it runs.
        w.postMessage({ memory });
    })));

    return {
        session,
        async run(j: OracleJob, seedBase: number, ms: number) {
            session.setup(j.seat, seedBase);
            await sleep(ms);
            session.stop();
            // let every thread leave octogen before anything is read or re-armed
            for (let i = 0; i < 400 && session.active() > 0; i++) await sleep(5);
        },
        async close() {
            session.stop();
            await Promise.all(workers.map((w) => w.terminate()));
        },
    };
}
