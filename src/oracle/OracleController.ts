/* =============================================================================
 * Infinite Oracle — main-thread controller (§8.7)
 * Lazily fetches oracle.wasm.gz, spawns the worker fleet, merges their batch
 * streams into running per-candidate estimates, drives convergence, and
 * publishes throttled snapshots to the overlay. Owns lifecycle: a new job
 * cancels the prior run (run-generation counter drops stale batches); dispose
 * terminates the fleet. StrictMode-safe.
 * ========================================================================== */

import { gunzip } from '@shared/sdk/ts/wasm/gunzip.ts';
import { OracleAccumulator } from './accumulator';
import {
    OracleJob, OracleSnapshot, OracleStatus, WorkerToMain,
    oracleWorkerCount, ORACLE_HARD_CAP_MS, ORACLE_MIN_FOCUS_MS,
} from './types';

const ORACLE_WASM_URL = '/oracle.wasm.gz';
type Subscriber = (s: OracleSnapshot) => void;

// Coalesce publishes to one per animation frame: 8 workers post batches far
// faster than React should render, so a rAF loop collapses every batch that
// landed since the last frame into a single re-render — smooth "come into
// focus" without melting React (never a setState per worker message).
const raf: (cb: () => void) => number =
    typeof requestAnimationFrame !== 'undefined'
        ? (cb) => requestAnimationFrame(() => cb())
        : (cb) => setTimeout(cb, 16) as unknown as number;
const cancelRaf: (id: number) => void =
    typeof cancelAnimationFrame !== 'undefined'
        ? (id) => cancelAnimationFrame(id)
        : (id) => clearTimeout(id as unknown as ReturnType<typeof setTimeout>);

export class OracleController {
    private workers: Worker[] = [];
    private bytes: Uint8Array | null = null;
    private loadPromise: Promise<void> | null = null;
    private ready = false;

    private gen = 0;                     // run generation; stale messages dropped
    private job: OracleJob | null = null;
    private acc: OracleAccumulator | null = null;
    private status: OracleStatus = 'idle';
    private error?: string;
    private startMs = 0;
    private rafId: number | null = null;
    private dirty = false;

    private subs = new Set<Subscriber>();

    subscribe(cb: Subscriber): () => void {
        this.subs.add(cb);
        if (this.job) cb(this.snapshot());
        return () => { this.subs.delete(cb); };
    }

    /** Fetch + gunzip + spawn + init the fleet (once). Throws on any failure so
     *  the overlay can render oracle_unavailable (§9.6). */
    private async ensureLoaded(): Promise<void> {
        if (this.ready) return;
        if (!this.loadPromise) this.loadPromise = this.load();
        await this.loadPromise;
    }

    private async load(): Promise<void> {
        const resp = await fetch(ORACLE_WASM_URL);
        if (!resp.ok) throw new Error(`oracle fetch ${resp.status}`);
        const gz = new Uint8Array(await resp.arrayBuffer());
        this.bytes = gunzip(gz);

        const n = oracleWorkerCount();
        const readies: Promise<void>[] = [];
        for (let i = 0; i < n; i++) {
            const w = new Worker(new URL('./oracleWorker.ts', import.meta.url), { type: 'module' });
            w.onmessage = (e: MessageEvent<WorkerToMain>) => this.onMessage(e.data);
            const ready = new Promise<void>((resolve) => {
                const onReady = (e: MessageEvent<WorkerToMain>) => {
                    if (e.data.t === 'ready') { w.removeEventListener('message', onReady); resolve(); }
                };
                w.addEventListener('message', onReady);
            });
            w.postMessage({ t: 'init', bytes: this.bytes });
            this.workers.push(w);
            readies.push(ready);
        }
        await Promise.all(readies);
        this.ready = true;
    }

    /** Begin analyzing `job`. Cancels any prior run. */
    async start(job: OracleJob): Promise<void> {
        this.gen++;
        const gen = this.gen;
        this.job = job;
        this.acc = new OracleAccumulator({ deckAlive: job.deckAlive, recordedKey: job.recordedKey });
        this.error = undefined;
        this.startMs = Date.now();
        this.status = 'loading';
        this.publish(true);

        try {
            await this.ensureLoaded();
        } catch (err) {
            if (gen !== this.gen) return;
            this.status = 'error';
            this.error = err instanceof Error ? err.message : String(err);
            this.publish(true);
            return;
        }
        if (gen !== this.gen) return;      // superseded while loading

        this.status = 'running';
        this.workers.forEach((w, i) =>
            w.postMessage({ t: 'analyze', job, seedSalt: (i * 0x9e3779b1 + 1) >>> 0, gen }));
        this.publish(true);
    }

    /** Cancel the current run; keep the fleet warm. */
    stopCurrent(): void {
        this.gen++;
        this.workers.forEach((w) => w.postMessage({ t: 'stop' }));
        if (this.status === 'running' || this.status === 'loading') this.status = 'idle';
    }

    /** Terminate the fleet (replay-screen unmount). */
    dispose(): void {
        this.gen++;
        this.workers.forEach((w) => w.terminate());
        this.workers = [];
        this.ready = false;
        this.loadPromise = null;
        if (this.rafId != null) { cancelRaf(this.rafId); this.rafId = null; }
        this.subs.clear();
    }

    private terminal(): boolean {
        return this.status === 'exact' || this.status === 'forced'
            || this.status === 'converged' || this.status === 'error';
    }

    private onMessage(m: WorkerToMain): void {
        if (m.t === 'ready') return;
        if (m.gen !== this.gen || !this.acc || this.terminal()) return;

        switch (m.t) {
            case 'batch': {
                this.acc.add(m.record);
                // convergence checkpoint (§8.7), held past the minimum focus
                // duration so the sharpening is perceptible on fast devices.
                const elapsed = Date.now() - this.startMs;
                if ((this.acc.converged() && elapsed >= ORACLE_MIN_FOCUS_MS) || elapsed >= ORACLE_HARD_CAP_MS) {
                    this.status = 'converged';
                    this.stopFleet();
                    this.publish(true);
                } else {
                    this.publish(false);
                }
                break;
            }
            case 'exact':
                this.status = 'exact';
                this.stopFleet();
                this.publish(true);
                break;
            case 'forced':
                this.status = 'forced';
                this.stopFleet();
                this.publish(true);
                break;
            case 'empty':
                // No dump — a decision with no deliberation (unexpected). Treat as
                // forced so the panel settles rather than spinning.
                this.status = 'forced';
                this.stopFleet();
                this.publish(true);
                break;
            case 'error':
                this.status = 'error';
                this.error = m.message;
                this.stopFleet();
                this.publish(true);
                break;
        }
    }

    private stopFleet(): void {
        this.workers.forEach((w) => w.postMessage({ t: 'stop' }));
    }

    private snapshot(): OracleSnapshot {
        const job = this.job!;
        const acc = this.acc;
        const exact = this.status === 'exact' || (!!acc && acc.hasWinLoss());
        const candidates = acc ? acc.candidates(exact) : [];
        const elapsedMs = this.startMs ? Date.now() - this.startMs : 0;
        const totalWorlds = acc ? acc.totalWorlds : 0;
        const worldsPerSec = elapsedMs > 0 ? Math.round((totalWorlds / elapsedMs) * 1000) : 0;
        return {
            decisionId: job.decisionId,
            status: this.status,
            regime: exact ? 'exact' : 'mc',
            candidates,
            totalWorlds,
            worldsPerSec,
            batches: acc ? acc.batches : 0,
            elapsedMs,
            memoryOn: job.memoryOn,
            seat: job.seat,
            recordedKey: job.recordedKey,
            recordedLabel: job.recordedLabel,
            recordedPresent: acc ? acc.hasKey(job.recordedKey) : false,
            approx: job.approx,
            deckAlive: job.deckAlive,
            error: this.error,
        };
    }

    private flush(): void {
        this.dirty = false;
        const snap = this.snapshot();
        this.subs.forEach((cb) => cb(snap));
    }

    private publish(force: boolean): void {
        if (!this.job) return;
        if (force) {
            if (this.rafId != null) { cancelRaf(this.rafId); this.rafId = null; }
            this.flush();
            return;
        }
        // Coalesce: mark dirty and render on the next animation frame.
        this.dirty = true;
        if (this.rafId != null) return;
        this.rafId = raf(() => { this.rafId = null; if (this.dirty) this.flush(); });
    }
}
