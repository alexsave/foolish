/* =============================================================================
 * Infinite Oracle — Mode B controller (docs/INFINITE_ORACLE_DESIGN.md §8b)
 * Shared-memory threads, coordination in C. The control instance runs on the
 * MAIN thread: it marshals g_game ONCE, spawns N worker trampolines over one
 * shared WebAssembly.Memory, and polls the C accumulator each animation frame to
 * publish snapshots. Same public API as OracleController (Mode A), so the
 * overlay and ReplayScreen are mode-agnostic — the factory picks the engine.
 *
 * Requires cross-origin isolation (SharedArrayBuffer). The main thread NEVER
 * blocks on Atomics.wait (browsers forbid it): it only atomic-stores the job and
 * relaxed-reads the snapshot; the workers do the waiting.
 * ========================================================================== */

import { Game } from '@shared/types.ts';
import { gunzip } from '@shared/wasm/gunzip.ts';
import { __marshalGame, __mem, __setResident } from '@shared/wasm/engine.ts';
import {
    OracleJob, OracleSnapshot, OracleStatus, OracleCandidate, OracleVerdict,
    oracleCardToken, canonicalMoveKey, oracleWorkerCount,
    ORACLE_TRUMP_KEEP, ORACLE_CONVERGE_MIN_N, ORACLE_HARD_CAP_MS, ORACLE_MIN_FOCUS_MS,
} from './types';

const ORACLE_MT_WASM_URL = '/oracle-mt.wasm.gz';
const STACK_BYTES = 512 * 1024;                 // per-thread stack (§8b.6)
const MOVE_TYPE = ['attack', 'cover', 'pass', 'pickup', 'good', 'wait'];

type Subscriber = (s: OracleSnapshot) => void;

interface MtExports {
    memory: WebAssembly.Memory;
    wasm_init(): void;
    wasm_io_ptr(): number;
    wasm_import_state(): void;
    wasm_import_strategy_keys(): void;
    wasm_import_logs(): void;
    wasm_clearenv(): void;
    wasm_setenv_from_io(): void;
    wasm_og_reload_flags(): void;
    __tls_size: WebAssembly.Global;
    wasm_mt_reserve(n: number, stack: number, tls: number): number;
    wasm_mt_warmup(seat: number): void;
    wasm_mt_setup(seat: number, seed: number, n: number): void;
    wasm_mt_stop(): void;
    wasm_mt_total(): number;
    wasm_mt_ncand(): number;
    wasm_mt_ready(): number;
    wasm_mt_batches(): number;
    wasm_mt_chosen(): number;
    wasm_mt_sumfp(i: number): number;
    wasm_mt_nsim(i: number): number;
    wasm_mt_forced(i: number): number;
    wasm_mt_candidates(): number;
}

const raf: (cb: () => void) => number =
    typeof requestAnimationFrame !== 'undefined' ? (cb) => requestAnimationFrame(() => cb()) : (cb) => setTimeout(cb, 16) as unknown as number;
const cancelRaf: (id: number) => void =
    typeof cancelAnimationFrame !== 'undefined' ? (id) => cancelAnimationFrame(id) : (id) => clearTimeout(id as unknown as ReturnType<typeof setTimeout>);

export class OracleModeBController {
    private bytes: Uint8Array | null = null;
    private mod: WebAssembly.Module | null = null;
    private memory: WebAssembly.Memory | null = null;
    private ex: MtExports | null = null;
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
        this.bytes = gunzip(new Uint8Array(await resp.arrayBuffer()));
        this.mod = await WebAssembly.compile(this.bytes as BufferSource);
        // Discover the TLS block size from a throwaway instance.
        const probeMem = new WebAssembly.Memory({ initial: 64, maximum: 2048, shared: true });
        const probe = await WebAssembly.instantiate(this.mod, { env: { memory: probeMem } });
        const tlsSize = (probe.exports as unknown as MtExports).__tls_size.value;
        const tlsBlock = (Math.max(tlsSize, 16) + 15) & ~15;

        // Size the shared memory for statics + reserve + per-instance 8 MiB TT.
        const need = 2 * 1024 * 1024 + this.nThreads * (STACK_BYTES + tlsBlock)
            + (this.nThreads + 1) * 9 * 1024 * 1024 + 8 * 1024 * 1024;
        const initial = Math.min(2048, Math.ceil(need / 65536));
        this.memory = new WebAssembly.Memory({ initial, maximum: 2048, shared: true });
        const inst = await WebAssembly.instantiate(this.mod, { env: { memory: this.memory } });
        this.ex = { ...(inst.exports as unknown as MtExports), memory: this.memory };
        this.ex.wasm_init();
        (this as { tlsBlock?: number }).tlsBlock = tlsBlock;
        this.ready = true;
    }

    async start(job: OracleJob): Promise<void> {
        this.gen++;
        const gen = this.gen;
        this.job = job;
        this.error = undefined;
        this.startMs = Date.now();
        this.status = 'loading';
        this.publish();

        try {
            await this.ensureLoaded();
        } catch (err) {
            if (gen !== this.gen) return;
            this.status = 'error';
            this.error = err instanceof Error ? err.message : String(err);
            this.publish();
            return;
        }
        if (gen !== this.gen || !this.ex || !this.memory || !this.bytes) return;

        const ex = this.ex;
        // marshal the job into the shared g_game (control instance, single-threaded)
        __setResident(null);
        __marshalGame(ex as never, job.gameBlob as unknown as Game);
        {
            const buf = __mem(ex as never); const q = ex.wasm_io_ptr();
            for (let i = 0; i < job.numPlayers; i++) buf[q + i] = 0xff;
            ex.wasm_import_strategy_keys();
        }
        if (job.memoryOn && job.logsWire.length > 2) { __mem(ex as never).set(job.logsWire, ex.wasm_io_ptr()); ex.wasm_import_logs(); }
        ex.wasm_clearenv();
        const env: Record<string, string> = {
            OG_KEEP1: '26', OG_KEEP2: '26', OG_W1: '48', OG_W2: '1', OG_W3: '0',
            OG_EXPLAIN_SOLVE_BUDGET: '2000000',
        };
        for (const [k, v] of Object.entries(env)) {
            const buf = __mem(ex as never); let q = ex.wasm_io_ptr();
            for (let i = 0; i < k.length; i++) buf[q++] = k.charCodeAt(i) & 0xff; buf[q++] = 0;
            for (let i = 0; i < v.length; i++) buf[q++] = v.charCodeAt(i) & 0xff; buf[q++] = 0;
            ex.wasm_setenv_from_io();
        }
        ex.wasm_og_reload_flags();
        ex.wasm_mt_warmup(job.seat);

        const tlsBlock = (this as { tlsBlock?: number }).tlsBlock ?? 65536;
        const base = ex.wasm_mt_reserve(this.nThreads, STACK_BYTES, tlsBlock);
        const tlsBase = base + this.nThreads * STACK_BYTES;

        // spawn worker trampolines (fresh each run; cheap relative to the analysis)
        this.terminateWorkers();
        for (let tid = 0; tid < this.nThreads; tid++) {
            const w = new Worker(new URL('./oracleMtWorker.ts', import.meta.url), { type: 'module' });
            w.postMessage({
                bytes: this.bytes, memory: this.memory, tid,
                stackTop: base + (tid + 1) * STACK_BYTES, tlsPtr: tlsBase + tid * tlsBlock,
            });
            this.workers.push(w);
        }

        // give the workers a moment to instantiate + reach the wait (missed-wakeup
        // safe: the thread loop re-checks the generation before parking), then launch.
        await new Promise((r) => setTimeout(r, 120));
        if (gen !== this.gen) return;
        ex.wasm_mt_setup(job.seat, (Math.floor(this.startMs) ^ 0xC0FFEE) >>> 0, this.nThreads);
        this.status = 'running';
        this.poll(gen);
    }

    private poll(gen: number): void {
        if (gen !== this.gen || !this.ex) return;
        const elapsed = Date.now() - this.startMs;
        const minN = this.minNsim();
        if ((minN >= ORACLE_CONVERGE_MIN_N && elapsed >= ORACLE_MIN_FOCUS_MS) || elapsed >= ORACLE_HARD_CAP_MS) {
            this.status = 'converged';
            this.ex.wasm_mt_stop();
            this.publish();
            return;
        }
        this.publish();
        this.rafId = raf(() => this.poll(gen));
    }

    private minNsim(): number {
        const ex = this.ex!;
        const n = ex.wasm_mt_ncand();
        if (n <= 0) return 0;
        let m = Infinity;
        for (let i = 0; i < n; i++) { const s = ex.wasm_mt_nsim(i); if (s > 0) m = Math.min(m, s); }
        return m === Infinity ? 0 : m;
    }

    stopCurrent(): void {
        this.gen++;
        if (this.ex) this.ex.wasm_mt_stop();
        if (this.rafId != null) { cancelRaf(this.rafId); this.rafId = null; }
        if (this.status === 'running' || this.status === 'loading') this.status = 'idle';
    }

    private terminateWorkers(): void {
        this.workers.forEach((w) => w.terminate());
        this.workers = [];
    }

    dispose(): void {
        this.gen++;
        if (this.ex) this.ex.wasm_mt_stop();
        if (this.rafId != null) { cancelRaf(this.rafId); this.rafId = null; }
        this.terminateWorkers();
        this.subs.clear();
        // keep the compiled module + shared memory for a possible re-open
    }

    private readCandidates(): OracleCandidate[] {
        const ex = this.ex!;
        const job = this.job!;
        const trump = job.gameBlob.power_suit;
        const n = ex.wasm_mt_candidates();       // writes descriptors into io, returns count
        if (n <= 0) return [];
        const buf = __mem(ex as never);
        let p = ex.wasm_io_ptr();
        const chosen = ex.wasm_mt_chosen();
        const decode = (b: number) => oracleCardToken({ suit: b >> 4, value: b & 0xf }, trump);
        const out: OracleCandidate[] = [];
        for (let i = 0; i < n; i++) {
            const type = MOVE_TYPE[buf[p++]] ?? '?';
            const nc = buf[p++]; const cards: string[] = [];
            for (let k = 0; k < nc; k++) cards.push(decode(buf[p++]));
            const nt = buf[p++]; const target: string[] = [];
            for (let k = 0; k < nt; k++) target.push(decode(buf[p++]));
            const key = canonicalMoveKey(type, cards, target);
            const nsim = ex.wasm_mt_nsim(i);
            const mean = nsim > 0 ? ex.wasm_mt_sumfp(i) / nsim : null;
            const forced = ex.wasm_mt_forced(i) !== 0;
            // Mode B has no per-batch variance stream; a nsim-based SE proxy
            // (typical finish stddev ≈ 1.2) drives the ± text + decimal gating.
            const se = nsim > 0 ? 1.2 / Math.sqrt(nsim) : Infinity;
            const tax = mean != null && type === 'attack' && job.deckAlive
                ? ORACLE_TRUMP_KEEP * cards.filter((tk) => tk.endsWith('*')).length : 0;
            const verdict: OracleVerdict = forced ? 'loss' : 'none';
            out.push({
                key, type, label: `${type} ${cards.join(',')}`, cards, target: target.length ? target : undefined,
                n: nsim, mean, se, adjusted: mean == null ? null : mean + tax,
                verdict, forcedLoss: forced, pruned: false, chosen: i === chosen,
                played: key === job.recordedKey,
            });
        }
        out.sort((a, b) => {
            if (a.adjusted == null && b.adjusted == null) return 0;
            if (a.adjusted == null) return 1;
            if (b.adjusted == null) return -1;
            return a.adjusted - b.adjusted;
        });
        return out;
    }

    private snapshot(): OracleSnapshot {
        const job = this.job!;
        const ex = this.ex;
        const candidates = ex && this.status !== 'loading' && this.status !== 'error' ? this.readCandidates() : [];
        const totalWorlds = candidates.reduce((m, c) => Math.max(m, c.n), 0);
        const elapsedMs = this.startMs ? Date.now() - this.startMs : 0;
        const recordedPresent = candidates.some((c) => c.played);
        return {
            decisionId: job.decisionId,
            status: this.status,
            regime: 'mc',
            candidates,
            totalWorlds,
            worldsPerSec: elapsedMs > 0 ? Math.round((totalWorlds / elapsedMs) * 1000) : 0,
            batches: ex ? Math.round(ex.wasm_mt_batches()) : 0,
            elapsedMs,
            memoryOn: job.memoryOn,
            seat: job.seat,
            recordedKey: job.recordedKey,
            recordedLabel: job.recordedLabel,
            recordedPresent,
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
