/* =============================================================================
 * Infinite Oracle - Mode B session (docs/INFINITE_ORACLE_DESIGN.md §8b)
 * =============================================================================
 * Everything Mode B does that is NOT "spawn a thread": compile oracle-mt.wasm
 * over ONE shared WebAssembly.Memory, marshal the job into the shared resident
 * game exactly once, hand out per-thread bootstrap arguments, arm a generation,
 * and read the C accumulator back as OracleCandidate rows.
 *
 * It is deliberately host-agnostic - it never mentions Worker. The browser
 * controller spawns `new Worker(...)`; the headless bench and the e2e suite
 * spawn node:worker_threads. Both drive the same object, so what the tests
 * exercise IS what the replay screen runs (the earlier cut of this had three
 * hand-copied bootstrap sequences and they had already drifted apart).
 * ========================================================================== */

import { Game } from '@api/core/types.ts';
import { __marshalGame, __mem, __setResident } from '@sdk/ts/wasm/engine.ts';
import {
    OracleJob, OracleCandidate, OracleVerdict,
    oracleCardToken, canonicalMoveKey, ORACLE_TRUMP_KEEP,
} from './types';

/** Per-thread stack (§8b.6): 512 KiB, ~36x the measured 14.3 KiB worst case.
 *  Thread stacks live in the heap region where --stack-first's trap-on-overflow
 *  does not reach, so the C side also stamps a canary at each stack's low end. */
export const ORACLE_MT_STACK_BYTES = 512 * 1024;

/** Move-type ids, in MOVE_* order (legal.h). */
const MOVE_TYPE = ['attack', 'cover', 'pass', 'pickup', 'good', 'wait'];

const OG_EX_NONE_V = 2000001, OG_EX_UNKNOWN_V = 2000002, OG_EX_ILLEGAL_V = 2000003;

export function decodeVerdict(val: number): { verdict: OracleVerdict; verdictVal?: number } {
    if (val === OG_EX_NONE_V || val === 0) return { verdict: 'none' };
    if (val === OG_EX_UNKNOWN_V) return { verdict: 'unknown' };
    if (val === OG_EX_ILLEGAL_V) return { verdict: 'illegal' };
    if (val > 0) return { verdict: 'win', verdictVal: val };
    if (val < 0) return { verdict: 'loss', verdictVal: val };
    return { verdict: 'draw' };
}

/** Mode B's env set. The struct-path knobs (OG_NO_BBSOLVE, OG_NO_FASTROLL,
 *  OG_LEAF, OG_DIFFTEST, OG_NO_WORLDSIM) are FORBIDDEN here (§8b.5 MT3): they
 *  route threads into game.c's handle_* where the benign-but-real statics
 *  (engine_last_reject, log_alloc's drop sink) live. The default fast bitboard
 *  path never enters them.
 *
 *  OG_W1 is FIXED here, where Mode A retunes it per batch (oracleWorker.ts). Mode
 *  A has to: its batch size sets how often a worker posts, and so how often React
 *  re-renders. In Mode B nothing crosses the boundary per batch - the main thread
 *  polls a C counter on its own frame budget - so the batch size is only an inner
 *  loop and there is nothing to adapt it to. */
export const ORACLE_MT_ENV: Readonly<Record<string, string>> = {
    OG_KEEP1: '26', OG_KEEP2: '26', OG_W1: '24', OG_W2: '1', OG_W3: '0',
    OG_EXPLAIN_SOLVE_BUDGET: '2000000',
};

export interface MtExports {
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
    wasm_mt_active(): number;
    wasm_mt_ncand(): number;
    wasm_mt_ready(): number;
    wasm_mt_batches(): number;
    wasm_mt_mismatch(): number;
    wasm_mt_canary(): number;
    wasm_mt_chosen(): number;
    wasm_mt_sumfp(i: number): number;
    wasm_mt_nsim(i: number): number;
    wasm_mt_forced(i: number): number;
    wasm_mt_candidates(): number;
    wasm_mt_solver(): number;
    wasm_mt_verdict(i: number): number;
    wasm_mt_defuse(): void;
}

/** What one worker trampoline needs, and nothing else. */
export interface MtThreadArgs {
    tid: number;
    stackTop: number;   // __stack_pointer for this thread (stacks grow DOWN)
    stackLow: number;   // its canary word, at the low end of its own region
    tlsPtr: number;     // __wasm_init_tls base for this thread
}

/** FNV-1a over the decision id. Mode B's seed base is derived from the JOB, not
 *  from the wall clock: two runs of the same decision then draw the same seed
 *  STREAM, which is the most reproducibility a wall-clock-bounded sampler can
 *  offer (see the determinism note in docs/INFINITE_ORACLE_MODE_B.md). */
export function oracleSeedBase(decisionId: string): number {
    let h = 0x811c9dc5;
    for (let i = 0; i < decisionId.length; i++) {
        h ^= decisionId.charCodeAt(i);
        h = Math.imul(h, 0x01000193) >>> 0;
    }
    return h >>> 0;
}

export class OracleMtSession {
    ex: MtExports | null = null;
    memory: WebAssembly.Memory | null = null;
    private tlsBlock = 0;
    private base = 0;
    nThreads = 0;

    /** Compile the module, size the shared memory for N threads, instantiate the
     *  CONTROL instance (main thread). Idempotent per session. */
    async load(bytes: Uint8Array, nThreads: number): Promise<void> {
        this.nThreads = Math.max(1, nThreads);
        const mod = await WebAssembly.compile(bytes as BufferSource);

        // Discover the TLS block size from a throwaway instance. It is large in
        // this build on purpose: the per-thread LegalMoves (MT2) alone is ~330 KB
        // at MAX_LEGAL_MOVES=4096, and cordite's recursion scratch rides along.
        const probeMem = new WebAssembly.Memory({ initial: 64, maximum: 2048, shared: true });
        const probe = await WebAssembly.instantiate(mod, { env: { memory: probeMem } });
        const tlsSize = (probe.exports as unknown as MtExports).__tls_size.value;
        this.tlsBlock = (Math.max(tlsSize, 16) + 15) & ~15;

        // Size the memory for what is live BEFORE any thread solves: statics,
        // the reserved stack/TLS region, and two transposition tables. The rest
        // arrives by memory.grow, which is atomic on shared memory and
        // serialized on the bump pointer by the MT1 spinlock - so the module
        // never reserves the 8 MiB x (threads + 1) worst case up front. The
        // declared MAXIMUM still has to cover it (128 MiB, --max-memory in
        // c/Makefile), because a shared memory must declare one.
        const need = 4 * 1024 * 1024
            + this.nThreads * (ORACLE_MT_STACK_BYTES + this.tlsBlock)
            + 2 * 9 * 1024 * 1024;
        const initial = Math.min(2048, Math.ceil(need / 65536));
        this.memory = new WebAssembly.Memory({ initial, maximum: 2048, shared: true });
        const inst = await WebAssembly.instantiate(mod, { env: { memory: this.memory } });
        // A shared-memory module IMPORTS its memory, so exports carry no
        // `memory` - the engine.ts marshal helpers read ex.memory.buffer, so
        // attach it.
        this.ex = { ...(inst.exports as unknown as MtExports), memory: this.memory };
        this.ex.wasm_init();
        this.moduleBytes = bytes;
    }

    /** The compiled bytes, for the trampolines (a Module is not structured-clonable
     *  everywhere; bytes always are). */
    moduleBytes: Uint8Array | null = null;

    /** §8.5 steps 1-4 against the SHARED resident game, then §8b.5 MT3's warmup
     *  (force the lazily-first-touch bitboard masks, detach the snapshot hook)
     *  and the stack/TLS reservation. Control instance only, main thread only. */
    prepare(job: OracleJob, env: Readonly<Record<string, string>> = ORACLE_MT_ENV): MtThreadArgs[] {
        const ex = this.ex;
        if (!ex) throw new Error('oracle-mt session not loaded');

        __setResident(null);
        __marshalGame(ex as never, job.gameBlob as unknown as Game);
        {
            const buf = __mem(ex as never); const q = ex.wasm_io_ptr();
            for (let i = 0; i < job.numPlayers; i++) buf[q + i] = 0xff;
            ex.wasm_import_strategy_keys();
        }
        if (job.memoryOn && job.logsWire.length > 2) {
            __mem(ex as never).set(job.logsWire, ex.wasm_io_ptr());
            ex.wasm_import_logs();
        }
        ex.wasm_clearenv();
        for (const [k, v] of Object.entries(env)) {
            const buf = __mem(ex as never); let q = ex.wasm_io_ptr();
            for (let i = 0; i < k.length; i++) buf[q++] = k.charCodeAt(i) & 0xff; buf[q++] = 0;
            for (let i = 0; i < v.length; i++) buf[q++] = v.charCodeAt(i) & 0xff; buf[q++] = 0;
            ex.wasm_setenv_from_io();
        }
        ex.wasm_og_reload_flags();
        ex.wasm_mt_warmup(job.seat);

        // Reserve AFTER the warmup so the control instance's own table is already
        // placed: the region is then bracketed by allocations that outlive it and
        // the heap only ever grows above the live stacks (§8b.4/§8b.6). ONCE per
        // session - the allocator is bump-only, so reserving per job would leak
        // N x (512 KiB + TLS) every time the user steps to another decision.
        if (!this.threadArgs) {
            this.base = ex.wasm_mt_reserve(this.nThreads, ORACLE_MT_STACK_BYTES, this.tlsBlock);
            if (!this.base) throw new Error('oracle-mt: could not reserve thread stacks');
            const tlsBase = this.base + this.nThreads * ORACLE_MT_STACK_BYTES;
            const out: MtThreadArgs[] = [];
            for (let tid = 0; tid < this.nThreads; tid++) {
                out.push({
                    tid,
                    stackTop: this.base + (tid + 1) * ORACLE_MT_STACK_BYTES,
                    stackLow: this.base + tid * ORACLE_MT_STACK_BYTES,
                    tlsPtr: tlsBase + tid * this.tlsBlock,
                });
            }
            this.threadArgs = out;
        }
        return this.threadArgs;
    }

    /** Reserved once per session; the trampolines are spawned once and reused
     *  across jobs (they park on the generation, they do not exit). */
    threadArgs: MtThreadArgs[] | null = null;

    setup(seat: number, seedBase: number): void {
        this.ex!.wasm_mt_setup(seat, seedBase >>> 0, this.nThreads);
    }
    stop(): void { this.ex?.wasm_mt_stop(); }
    active(): number { return this.ex ? this.ex.wasm_mt_active() : 0; }
    batches(): number { return this.ex ? Math.round(this.ex.wasm_mt_batches()) : 0; }
    mismatches(): number { return this.ex ? Math.round(this.ex.wasm_mt_mismatch()) : 0; }
    canaryTrips(): number { return this.ex ? this.ex.wasm_mt_canary() : 0; }
    totalChooses(): number { return this.ex ? this.ex.wasm_mt_total() : 0; }

    /** True once the endgame solver has PROVEN a win or a loss for some
     *  candidate - the regime switch the overlay renders as "exact". */
    hasProof(): boolean {
        const ex = this.ex;
        if (!ex || ex.wasm_mt_solver() === 0) return false;
        const n = ex.wasm_mt_ncand();
        for (let i = 0; i < n; i++) {
            const v = decodeVerdict(ex.wasm_mt_verdict(i)).verdict;
            if (v === 'win' || v === 'loss') return true;
        }
        return false;
    }
    solverFired(): boolean { return !!this.ex && this.ex.wasm_mt_solver() !== 0; }
    defuse(): void { this.ex?.wasm_mt_defuse(); }

    /** The smallest per-candidate world count - the convergence yardstick. */
    minNsim(): number {
        const ex = this.ex;
        if (!ex) return 0;
        const n = ex.wasm_mt_ncand();
        if (n <= 0) return 0;
        let m = Infinity;
        for (let i = 0; i < n; i++) { const s = ex.wasm_mt_nsim(i); if (s > 0) m = Math.min(m, s); }
        return m === Infinity ? 0 : m;
    }

    /** Read the C accumulator into the same OracleCandidate rows Mode A's
     *  accumulator produces, so the overlay cannot tell the modes apart. */
    readCandidates(job: OracleJob, exact: boolean): OracleCandidate[] {
        const ex = this.ex;
        if (!ex) return [];
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
            // Mode B has no per-batch variance stream (that is the point - no
            // per-batch anything crosses the wasm boundary), so the +- text uses
            // an n-based proxy at the finish-position spread Mode A measures.
            const se = nsim > 0 ? 1.2 / Math.sqrt(nsim) : Infinity;
            const tax = mean != null && type === 'attack' && job.deckAlive
                ? ORACLE_TRUMP_KEEP * cards.filter((tk) => tk.endsWith('*')).length : 0;
            const dv = decodeVerdict(ex.wasm_mt_verdict(i));
            const verdict: OracleVerdict = dv.verdict !== 'none' ? dv.verdict : (forced ? 'loss' : 'none');
            out.push({
                key, type, label: `${type} ${cards.join(',')}`, cards,
                target: target.length ? target : undefined,
                n: nsim, mean, se, adjusted: mean == null ? null : mean + tax,
                verdict, verdictVal: dv.verdictVal, forcedLoss: forced, pruned: false,
                chosen: i === chosen,
                played: key === job.recordedKey,
            });
        }
        return sortCandidates(out, exact);
    }
}

/** The overlay's row order. Identical rule to the Mode A accumulator: bars,
 *  sort and the displayed number all key off the true expected finish; the
 *  trump tax is only an invisible tie-break. */
export function sortCandidates(list: OracleCandidate[], exact: boolean): OracleCandidate[] {
    if (exact) {
        const RANK: Record<string, number> = { win: 0, draw: 1, unknown: 2, none: 3, loss: 4, illegal: 5 };
        const depth = (c: OracleCandidate) => (c.verdictVal != null ? 1000 - Math.abs(c.verdictVal) : 1000);
        list.sort((a, b) => (RANK[a.verdict] ?? 9) - (RANK[b.verdict] ?? 9) || depth(b) - depth(a));
    } else {
        list.sort((a, b) => {
            if (a.mean == null && b.mean == null) return 0;
            if (a.mean == null) return 1;
            if (b.mean == null) return -1;
            return (a.mean - b.mean) || ((a.adjusted ?? a.mean) - (b.adjusted ?? b.mean));
        });
    }
    return list;
}
