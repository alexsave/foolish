/* =============================================================================
 * Infinite Oracle - Mode B controller (docs/INFINITE_ORACLE_DESIGN.md §8b)
 * =============================================================================
 * Shared-memory threads, coordination in C. The control instance runs on the
 * MAIN thread: it marshals g_game ONCE, spawns N worker trampolines over one
 * shared WebAssembly.Memory, and polls the C accumulator each animation frame to
 * publish snapshots. Same public API as OracleController (Mode A), so the
 * overlay and ReplayScreen are mode-agnostic - the factory picks the engine.
 *
 * Requires cross-origin isolation (SharedArrayBuffer). The main thread NEVER
 * blocks on Atomics.wait (browsers forbid it): it only atomic-stores the job and
 * relaxed-reads the snapshot; the workers do the waiting.
 * ========================================================================== */

import { gunzip } from '@sdk/ts/wasm/gunzip.ts';
import {
    OracleJob, OracleSnapshot, OracleStatus,
    oracleWorkerCount, ORACLE_CONVERGE_MIN_N, ORACLE_HARD_CAP_MS, ORACLE_MIN_FOCUS_MS,
} from './types';
import { OracleMtSession, oracleSeedBase } from './oracleMtSession';

const ORACLE_MT_WASM_URL = '/oracle-mt.wasm.gz';

type Subscriber = (s: OracleSnapshot) => void;

const raf: (cb: () => void) => number =
    typeof requestAnimationFrame !== 'undefined'
        ? (cb) => requestAnimationFrame(() => cb())
        : (cb) => setTimeout(cb, 16) as unknown as number;
const cancelRaf: (id: number) => void =
    typeof cancelAnimationFrame !== 'undefined'
        ? (id) => cancelAnimationFrame(id)
        : (id) => clearTimeout(id as unknown as ReturnType<typeof setTimeout>);

export class OracleModeBController {
    private session = new OracleMtSession();
    private workers: Worker[] = [];
    private loadPromise: Promise<void> | null = null;
    private ready = false;
    private nThreads = oracleWorkerCount();

    private gen = 0;
    private job: OracleJob | null = null;
    private status: OracleStatus = 'idle';
    private error?: string;
    private startMs = 0;
    private rafId: number | null = null;
    private defused = false;

    private subs = new Set<Subscriber>();

    subscribe(cb: Subscriber): () => void {
        this.subs.add(cb);
        if (this.job) cb(this.snapshot());
        return () => { this.subs.delete(cb); };
    }

    private async ensureLoaded(): Promise<void> {
        if (this.ready) return;
        if (!this.loadPromise) this.loadPromise = this.load();
        await this.loadPromise;
    }

    private async load(): Promise<void> {
        const resp = await fetch(ORACLE_MT_WASM_URL);
        if (!resp.ok) throw new Error(`oracle-mt fetch ${resp.status}`);
        await this.session.load(gunzip(new Uint8Array(await resp.arrayBuffer())), this.nThreads);
        this.ready = true;
    }

    async start(job: OracleJob): Promise<void> {
        this.gen++;
        const gen = this.gen;
        this.job = job;
        this.error = undefined;
        this.startMs = Date.now();
        this.defused = false;
        this.status = 'loading';
        this.publish();

        try {
            await this.ensureLoaded();
            // A previous generation's threads must be OUT of octogen before the
            // accumulator is zeroed, or a late batch lands in the new job's
            // table. The main thread may not Atomics.wait, so this is a
            // non-blocking poll on the C-side active count.
            this.session.stop();
            await this.drain();
            if (gen !== this.gen) return;

            const args = this.session.prepare(job);
            await this.spawn(args);
            if (gen !== this.gen) return;
            this.session.setup(job.seat, oracleSeedBase(job.decisionId));
        } catch (err) {
            if (gen !== this.gen) return;
            this.status = 'error';
            this.error = err instanceof Error ? err.message : String(err);
            this.publish();
            return;
        }
        this.status = 'running';
        this.poll(gen);
    }

    /** Wait (without blocking) until no thread is still inside a batch. */
    private async drain(): Promise<void> {
        for (let i = 0; i < 200 && this.session.active() > 0; i++) {
            await new Promise((r) => setTimeout(r, 5));
        }
    }

    /** Spawn the trampolines ONCE and keep them parked between jobs. */
    private async spawn(args: ReturnType<OracleMtSession['prepare']>): Promise<void> {
        if (this.workers.length) return;
        const bytes = this.session.moduleBytes!;
        const memory = this.session.memory!;
        await Promise.all(args.map((a) => new Promise<void>((resolve, reject) => {
            const w = new Worker(new URL('./oracleMtWorker.ts', import.meta.url), { type: 'module' });
            w.onmessage = (e: MessageEvent) => {
                const m = e.data as { t: string; message?: string };
                if (m.t === 'ready') resolve();
                else reject(new Error(m.message ?? 'oracle-mt worker failed'));
            };
            w.onerror = (e) => reject(new Error(String((e as ErrorEvent).message ?? 'oracle-mt worker error')));
            this.workers.push(w);
            w.postMessage({ bytes, memory, ...a });
        })));
    }

    private poll(gen: number): void {
        if (gen !== this.gen) return;
        const elapsed = Date.now() - this.startMs;

        // A canary trip means a thread stack was smashed: nothing read out of the
        // shared table after that is trustworthy, so the run is killed (§8b.6).
        if (this.session.canaryTrips() > 0) {
            this.status = 'error';
            this.error = 'oracle-mt stack canary';
            this.session.stop();
            this.publish();
            return;
        }

        // exact endgame: if the solver proved a win/loss, stop immediately (§8b.5).
        if (this.session.hasProof()) {
            this.status = 'exact';
            this.session.stop();
            this.publish();
            return;
        }
        // endgame gate passed but nothing proven at budget: defuse the expensive
        // per-choose probe so later batches are pure-MC-priced (§5.4).
        if (this.session.solverFired() && !this.defused) {
            this.defused = true;
            this.session.defuse();
        }

        const minN = this.session.minNsim();
        if ((minN >= ORACLE_CONVERGE_MIN_N && elapsed >= ORACLE_MIN_FOCUS_MS) || elapsed >= ORACLE_HARD_CAP_MS) {
            this.status = 'converged';
            this.session.stop();
            this.publish();
            return;
        }
        this.publish();
        this.rafId = raf(() => this.poll(gen));
    }

    stopCurrent(): void {
        this.gen++;
        this.session.stop();
        if (this.rafId != null) { cancelRaf(this.rafId); this.rafId = null; }
        if (this.status === 'running' || this.status === 'loading') this.status = 'idle';
    }

    dispose(): void {
        this.gen++;
        this.session.stop();
        if (this.rafId != null) { cancelRaf(this.rafId); this.rafId = null; }
        this.workers.forEach((w) => w.terminate());
        this.workers = [];
        this.subs.clear();
    }

    private snapshot(): OracleSnapshot {
        const job = this.job!;
        const active = this.ready && this.status !== 'loading' && this.status !== 'error';
        const exact = active && (this.status === 'exact' || this.session.hasProof());
        const candidates = active ? this.session.readCandidates(job, exact) : [];
        const totalWorlds = candidates.reduce((m, c) => Math.max(m, c.n), 0);
        const elapsedMs = this.startMs ? Date.now() - this.startMs : 0;
        return {
            decisionId: job.decisionId,
            status: this.status,
            regime: exact ? 'exact' : 'mc',
            candidates,
            totalWorlds,
            worldsPerSec: elapsedMs > 0 ? Math.round((totalWorlds / elapsedMs) * 1000) : 0,
            batches: this.session.batches(),
            elapsedMs,
            memoryOn: job.memoryOn,
            seat: job.seat,
            recordedKey: job.recordedKey,
            recordedLabel: job.recordedLabel,
            recordedPresent: candidates.some((c) => c.played),
            approx: job.approx,
            deckAlive: job.deckAlive,
            error: this.error,
        };
    }

    private publish(): void {
        if (!this.job) return;
        const snap = this.snapshot();
        this.subs.forEach((cb) => cb(snap));
    }
}
