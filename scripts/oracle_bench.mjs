// Ad-hoc latency/throughput benchmark: Mode A (instance fleet, per-batch
// marshal + JSON dump) vs Mode B (shared-memory threads, marshal once, C
// accumulator). Node's worker_threads + shared WebAssembly.Memory stand in for
// the browser fleet (docs/INFINITE_ORACLE_DESIGN.md §8b.7). Measures octogen
// choose-calls/sec at the same OG_W1 and thread count.
//
//   node scripts/oracle_bench.mjs            # both modes, N = cores-2
//   BENCH_THREADS=4 BENCH_SECONDS=4 node scripts/oracle_bench.mjs
import { Worker, isMainThread, parentPort, workerData } from 'node:worker_threads';
import fs from 'node:fs';
import os from 'node:os';

const R = '/home/user/foolish';
const A_WASM = `${R}/public/oracle.wasm.gz`;
const B_WASM = `${R}/public/oracle-mt.wasm.gz`;
const SECONDS = Number(process.env.BENCH_SECONDS || '4');
const THREADS = Number(process.env.BENCH_THREADS || String(Math.max(1, os.cpus().length - 2)));
const OG_W1 = process.env.BENCH_W1 || '24';
const ENV = { OG_KEEP1: '26', OG_KEEP2: '26', OG_W1, OG_W2: '1', OG_W3: '0', OG_EXPLAIN_SOLVE_BUDGET: '2000000' };

// ---- shared job construction (main thread) --------------------------------
async function buildJob() {
    const { splitReplayCode } = await import(`${R}/supabase/functions/_shared/replay/extras.ts`);
    const { codeToGame } = await import(`${R}/supabase/functions/_shared/replay/codec.ts`);
    const { decodeReplay } = await import(`${R}/supabase/functions/_shared/replay/decode.ts`);
    const { buildReplaySteps } = await import(`${R}/src/replay/view.ts`);
    const { buildOracleJob } = await import(`${R}/src/oracle/replayOracleInput.ts`);
    const CODE = 'ENSCBI2LBAVUBJJ3J7NODALIBDGEQYLLLICQ';
    const decoded = await decodeReplay(codeToGame(splitReplayCode(CODE).moves));
    const steps = buildReplaySteps(decoded);
    const idx = Math.floor(decoded.logs.length * 0.4);
    return buildOracleJob(decoded, steps, idx, true, CODE);
}
async function gunzipBytes(p) {
    const { gunzip } = await import(`${R}/supabase/functions/_shared/wasm/gunzip.ts`);
    return gunzip(new Uint8Array(fs.readFileSync(p)));
}

// =====================  MODE A: instance fleet  ============================
// tsx path aliases (@shared/*) don't resolve inside worker_threads, so instead
// of importing the .ts bridge in the worker we capture the marshaled state
// bytes ONCE in the main thread (via the real engine.ts marshal) and hand the
// worker plain byte blobs. The worker replays exactly Mode A's per-batch work —
// write state/keys/logs, choose, read + JSON.parse the dump — in plain JS. The
// only thing skipped is the JS marshal's arithmetic (a memcpy in its place),
// which is dwarfed by the octogen compute + the dump parse this measures.
async function captureMarshal(job) {
    const { __marshalGame, __mem, __setResident } = await import(`${R}/supabase/functions/_shared/wasm/engine.ts`);
    const bytes = await gunzipBytes(A_WASM);
    const src = await WebAssembly.instantiate(bytes, {});
    const ex = src.instance.exports;
    ex.wasm_init();
    __setResident(null);
    __marshalGame(ex, job.gameBlob);           // writes state bytes at io ptr, imports them
    const io = ex.wasm_io_ptr();
    const stateBytes = new Uint8Array(__mem(ex).slice(io, io + 16384));   // generous snapshot
    return { bytes, stateBytes };
}
async function runModeA(job) {
    const { bytes, stateBytes } = await captureMarshal(job);
    const workers = [];
    for (let tid = 0; tid < THREADS; tid++) {
        workers.push(new Promise((res) => {
            const w = new Worker(new URL(import.meta.url), {
                workerData: {
                    mode: 'A', bytes, stateBytes, seat: job.seat, numPlayers: job.numPlayers,
                    logsWire: job.logsWire, memoryOn: job.memoryOn, env: ENV, seconds: SECONDS, tid,
                },
            });
            w.on('message', res);
        }));
    }
    const counts = await Promise.all(workers);
    return counts.reduce((a, b) => a + b.chooses, 0);
}
async function modeAWorker({ bytes, stateBytes, seat, numPlayers, logsWire, memoryOn, env, seconds, tid }) {
    const src = await WebAssembly.instantiate(bytes, {});
    const ex = src.instance.exports;
    ex.wasm_init();
    const mem = () => new Uint8Array(ex.memory.buffer);
    ex.wasm_clearenv();
    for (const [k, v] of Object.entries(env)) {
        const buf = mem(); let q = ex.wasm_io_ptr();
        for (let i = 0; i < k.length; i++) buf[q++] = k.charCodeAt(i) & 0xff; buf[q++] = 0;
        for (let i = 0; i < v.length; i++) buf[q++] = v.charCodeAt(i) & 0xff; buf[q++] = 0;
        ex.wasm_setenv_from_io();
    }
    ex.wasm_og_reload_flags();
    const dec = new TextDecoder();
    let chooses = 0, seed = 0x1234 + tid * 7919;
    const end = Date.now() + seconds * 1000;
    while (Date.now() < end) {
        for (let k = 0; k < 8; k++) {                     // batch the clock check
            seed = (seed * 1103515245 + 12345) >>> 0;
            mem().set(stateBytes, ex.wasm_io_ptr()); ex.wasm_import_state();
            { const buf = mem(); const q = ex.wasm_io_ptr(); for (let i = 0; i < numPlayers; i++) buf[q + i] = 0xff; ex.wasm_import_strategy_keys(); }
            if (memoryOn && logsWire.length > 2) { mem().set(logsWire, ex.wasm_io_ptr()); ex.wasm_import_logs(); }
            ex.wasm_set_strategy_seed(seed >>> 0);
            ex.wasm_og_explain_reset();
            ex.wasm_choose_move(20, seat);
            // the per-batch overhead that distinguishes Mode A: read + parse the dump
            const len = ex.wasm_og_explain_len();
            if (len > 0) {
                const ptr = ex.wasm_og_explain_ptr();
                const text = dec.decode(mem().subarray(ptr, ptr + len));
                const nl = text.indexOf('\n');
                try { JSON.parse(nl >= 0 ? text.slice(0, nl) : text); chooses++; } catch { /* skip */ }
            }
        }
    }
    parentPort.postMessage({ chooses });
}

// =====================  MODE B: shared-memory threads  =====================
async function runModeB(job) {
    const { __marshalGame, __mem, __setResident } = await import(`${R}/supabase/functions/_shared/wasm/engine.ts`);
    const bytes = await gunzipBytes(B_WASM);
    const mod = await WebAssembly.compile(bytes);

    const STACK = 512 * 1024;
    // control instance to discover tls size and marshal the shared game
    const probeMem = new WebAssembly.Memory({ initial: 64, maximum: 2048, shared: true });
    const probe = await WebAssembly.instantiate(mod, { env: { memory: probeMem } });
    const tlsSize = probe.exports.__tls_size?.value ?? 0;
    const tlsBlock = (Math.max(tlsSize, 16) + 15) & ~15;

    // size the real shared memory: statics + reserve + per-instance 8MB TT (the
    // control instance allocates one too) + slack
    const need = 2 * 1024 * 1024 + THREADS * (STACK + tlsBlock) + (THREADS + 1) * 9 * 1024 * 1024 + 8 * 1024 * 1024;
    const initialPages = Math.min(2048, Math.ceil(need / 65536));
    const memory = new WebAssembly.Memory({ initial: initialPages, maximum: 2048, shared: true });
    const control = await WebAssembly.instantiate(mod, { env: { memory } });
    // MT memory is imported, so exports have no `memory`; attach it so the
    // engine.ts marshal helpers (which read ex.memory.buffer) work.
    const ex = { ...control.exports, memory };
    ex.wasm_init();

    // marshal the job into the shared g_game (control instance, single-threaded)
    __setResident(null);
    __marshalGame(ex, job.gameBlob);
    { const buf = __mem(ex); const q = ex.wasm_io_ptr(); for (let i = 0; i < job.numPlayers; i++) buf[q + i] = 0xff; ex.wasm_import_strategy_keys(); }
    if (job.memoryOn && job.logsWire.length > 2) { const buf = __mem(ex); buf.set(job.logsWire, ex.wasm_io_ptr()); ex.wasm_import_logs(); }
    // env
    ex.wasm_clearenv();
    for (const [k, v] of Object.entries(ENV)) {
        const buf = __mem(ex); let q = ex.wasm_io_ptr();
        for (let i = 0; i < k.length; i++) buf[q++] = k.charCodeAt(i) & 0xff; buf[q++] = 0;
        for (let i = 0; i < v.length; i++) buf[q++] = v.charCodeAt(i) & 0xff; buf[q++] = 0;
        ex.wasm_setenv_from_io();
    }
    ex.wasm_og_reload_flags();
    ex.wasm_mt_warmup(job.seat);                // masks + snap hook (MT3) on control

    const base = ex.wasm_mt_reserve(THREADS, STACK, tlsBlock);
    const tlsBase = base + THREADS * STACK;

    const spawned = [];
    const kids = [];
    for (let tid = 0; tid < THREADS; tid++) {
        spawned.push(new Promise((res) => {
            const w = new Worker(new URL(import.meta.url), {
                workerData: { mode: 'B', bytes, tid, stackTop: base + (tid + 1) * STACK, tlsPtr: tlsBase + tid * tlsBlock },
            });
            kids.push(w);
            // NB: do NOT unref() — an unref'd worker can be reclaimed before it runs.
            w.on('online', () => { w.postMessage({ memory }); res(); });
            w.on('error', (e) => { console.error(`  [modeB worker ${tid}] ${e.message}`); res(); });
        }));
    }
    await Promise.all(spawned);

    // let threads instantiate the 833 KB module + reach the wait, then launch
    // (missed-wakeup safe: thread_main re-checks the generation before parking)
    await new Promise((r) => setTimeout(r, 500));
    ex.wasm_mt_setup(job.seat, 0xC0FFEE, THREADS);
    await new Promise((r) => setTimeout(r, SECONDS * 1000));
    ex.wasm_mt_stop();
    await new Promise((r) => setTimeout(r, 50));
    const total = ex.wasm_mt_total();
    await Promise.all(kids.map((w) => w.terminate()));
    return total;
}
function modeBWorker({ bytes, tid, stackTop, tlsPtr }) {
    const mod = new WebAssembly.Module(bytes);
    parentPort.once('message', ({ memory }) => {
        try {
            const inst = new WebAssembly.Instance(mod, { env: { memory } });
            inst.exports.__stack_pointer.value = stackTop;
            inst.exports.__wasm_init_tls(tlsPtr);
            inst.exports.wasm_mt_thread_main(tid);   // never returns
        } catch (e) {
            console.error(`[Bworker ${tid}] FAILED: ${e.message}`);
        }
    });
}

// ---- entry ----------------------------------------------------------------
if (isMainThread) {
    const job = await buildJob();
    if (!job) { console.error('no job'); process.exit(1); }
    console.log(`decision seat ${job.seat}, memoryOn ${job.memoryOn}, OG_W1=${OG_W1}, ${THREADS} threads, ${SECONDS}s each\n`);

    console.log('Mode A (instance fleet: marshal + choose + JSON dump per batch)…');
    const aTotal = await runModeA(job);
    const aRate = aTotal / SECONDS;
    console.log(`  Mode A: ${aTotal.toLocaleString()} chooses  →  ${Math.round(aRate).toLocaleString()} choose/s\n`);

    console.log('Mode B (shared-memory threads: marshal once, C accumulator)…');
    const bTotal = await runModeB(job);
    const bRate = bTotal / SECONDS;
    console.log(`  Mode B: ${bTotal.toLocaleString()} chooses  →  ${Math.round(bRate).toLocaleString()} choose/s\n`);

    const ratio = bRate / aRate;
    console.log(`Mode B / Mode A = ${ratio.toFixed(2)}x  (${ratio > 1 ? 'Mode B faster' : 'Mode A faster'})`);
    process.exit(0);
} else if (workerData.mode === 'A') {
    await modeAWorker(workerData);
} else if (workerData.mode === 'B') {
    modeBWorker(workerData);
}
