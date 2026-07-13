// Mode B correctness check (docs/INFINITE_ORACLE_DESIGN.md §8b.7): run the same
// decision through Mode A (dump + JS accumulator) and Mode B (C accumulator over
// shared-memory threads) and assert the per-candidate mean-finish estimates
// agree within a couple SE — different seed streams, same converged values.
//   TSX_TSCONFIG_PATH=tsconfig.json node --import tsx scripts/oracle_mt_verify.mjs
import { Worker, isMainThread, parentPort, workerData } from 'node:worker_threads';
import fs from 'node:fs';

const R = '/home/user/foolish';
const THREADS = Number(process.env.MT_THREADS || '3');
const SECONDS = Number(process.env.MT_SECONDS || '4');
const ENV = { OG_KEEP1: '26', OG_KEEP2: '26', OG_W1: '24', OG_W2: '1', OG_W3: '0', OG_EXPLAIN_SOLVE_BUDGET: '2000000' };

async function gunzipBytes(p) {
    const { gunzip } = await import(`${R}/supabase/functions/_shared/wasm/gunzip.ts`);
    return gunzip(new Uint8Array(fs.readFileSync(p)));
}

if (!isMainThread) {
    // Mode B worker trampoline: compile, wait for the shared memory, run forever.
    const mod = new WebAssembly.Module(workerData.bytes);
    parentPort.once('message', ({ memory }) => {
        const inst = new WebAssembly.Instance(mod, { env: { memory } });
        inst.exports.__stack_pointer.value = workerData.stackTop;
        inst.exports.__wasm_init_tls(workerData.tlsPtr);
        inst.exports.wasm_mt_thread_main(workerData.tid);
    });
} else {
    const { splitReplayCode } = await import(`${R}/supabase/functions/_shared/replay/extras.ts`);
    const { codeToGame } = await import(`${R}/supabase/functions/_shared/replay/codec.ts`);
    const { decodeReplay } = await import(`${R}/supabase/functions/_shared/replay/decode.ts`);
    const { buildReplaySteps } = await import(`${R}/src/replay/view.ts`);
    const { buildOracleJob } = await import(`${R}/src/oracle/replayOracleInput.ts`);
    const { OracleInstance } = await import(`${R}/src/oracle/oracleBridge.ts`);
    const { OracleAccumulator } = await import(`${R}/src/oracle/accumulator.ts`);
    const { oracleCardToken, canonicalMoveKey } = await import(`${R}/src/oracle/types.ts`);
    const { __marshalGame, __mem, __setResident } = await import(`${R}/supabase/functions/_shared/wasm/engine.ts`);

    const CODE = 'ENSCBI2LBAVUBJJ3J7NODALIBDGEQYLLLICQ';
    const decoded = await decodeReplay(codeToGame(splitReplayCode(CODE).moves));
    const steps = buildReplaySteps(decoded);
    const job = buildOracleJob(decoded, steps, Math.floor(decoded.logs.length * 0.4), true, CODE);
    const trump = job.gameBlob.power_suit;
    console.log(`decision seat ${job.seat}, ${THREADS} threads, ${SECONDS}s\n`);

    // ---------- Mode A reference (single instance) ----------
    const aInst = new OracleInstance();
    await aInst.init(await gunzipBytes(`${R}/public/oracle.wasm.gz`));
    aInst.writeEnv(ENV);
    const acc = new OracleAccumulator({ deckAlive: job.deckAlive, recordedKey: job.recordedKey });
    { const end = Date.now() + SECONDS * 1000; let s = 7;
      while (Date.now() < end) { s = (s * 1103515245 + 12345) >>> 0;
        const r = aInst.analyzeOnce(job.gameBlob, job.seat, job.logsWire, true, s); if ('record' in r) acc.add(r.record); } }
    const aMap = new Map(acc.candidates(false).filter((c) => c.mean != null).map((c) => [c.key, c.mean]));

    // ---------- Mode B (shared-memory threads) ----------
    const bytes = await gunzipBytes(`${R}/public/oracle-mt.wasm.gz`);
    const mod = await WebAssembly.compile(bytes);
    const STACK = 512 * 1024;
    const probe = await WebAssembly.instantiate(mod, { env: { memory: new WebAssembly.Memory({ initial: 64, maximum: 2048, shared: true }) } });
    const tlsBlock = (Math.max(probe.exports.__tls_size?.value ?? 0, 16) + 15) & ~15;
    const need = 2 * 1024 * 1024 + THREADS * (STACK + tlsBlock) + (THREADS + 1) * 9 * 1024 * 1024 + 8 * 1024 * 1024;
    const memory = new WebAssembly.Memory({ initial: Math.min(2048, Math.ceil(need / 65536)), maximum: 2048, shared: true });
    const ex = { ...(await WebAssembly.instantiate(mod, { env: { memory } })).exports, memory };
    ex.wasm_init();
    __setResident(null); __marshalGame(ex, job.gameBlob);
    { const buf = __mem(ex); const q = ex.wasm_io_ptr(); for (let i = 0; i < job.numPlayers; i++) buf[q + i] = 0xff; ex.wasm_import_strategy_keys(); }
    if (job.logsWire.length > 2) { __mem(ex).set(job.logsWire, ex.wasm_io_ptr()); ex.wasm_import_logs(); }
    ex.wasm_clearenv();
    for (const [k, v] of Object.entries(ENV)) { const buf = __mem(ex); let q = ex.wasm_io_ptr();
        for (let i = 0; i < k.length; i++) buf[q++] = k.charCodeAt(i) & 0xff; buf[q++] = 0;
        for (let i = 0; i < v.length; i++) buf[q++] = v.charCodeAt(i) & 0xff; buf[q++] = 0; ex.wasm_setenv_from_io(); }
    ex.wasm_og_reload_flags();
    ex.wasm_mt_warmup(job.seat);
    const base = ex.wasm_mt_reserve(THREADS, STACK, tlsBlock);
    const tlsBase = base + THREADS * STACK;
    const kids = [];
    await Promise.all(Array.from({ length: THREADS }, (_, tid) => new Promise((res) => {
        const w = new Worker(new URL(import.meta.url), { workerData: { bytes, tid, stackTop: base + (tid + 1) * STACK, tlsPtr: tlsBase + tid * tlsBlock } });
        kids.push(w);
        w.on('online', () => { w.postMessage({ memory }); res(); });
        w.on('error', (e) => { console.error('worker error', e.message); res(); });
    })));
    await new Promise((r) => setTimeout(r, 400));
    ex.wasm_mt_setup(job.seat, 0xC0FFEE, THREADS);
    await new Promise((r) => setTimeout(r, SECONDS * 1000));
    ex.wasm_mt_stop();
    await new Promise((r) => setTimeout(r, 60));

    // read Mode B candidate descriptors + scores
    const ncand = ex.wasm_mt_candidates();
    const buf = __mem(ex); let p = ex.wasm_io_ptr();
    const decodeCard = (b) => ({ suit: b >> 4, value: b & 0xf });
    const bMap = new Map();
    for (let i = 0; i < ncand; i++) {
        const MTYPE = ['attack', 'cover', 'pass', 'pickup', 'good', 'wait'];
        const type = MTYPE[buf[p++]] ?? '?';
        const ncards = buf[p++]; const cards = [];
        for (let k = 0; k < ncards; k++) cards.push(oracleCardToken(decodeCard(buf[p++]), trump));
        const ntargets = buf[p++]; const targets = [];
        for (let k = 0; k < ntargets; k++) targets.push(oracleCardToken(decodeCard(buf[p++]), trump));
        const key = canonicalMoveKey(type, cards, targets);
        const nsim = ex.wasm_mt_nsim(i);
        const mean = nsim > 0 ? ex.wasm_mt_sumfp(i) / nsim : null;
        bMap.set(key, { mean, nsim, label: `${type} ${cards.join(',')}${targets.length ? '->' + targets.join(',') : ''}` });
    }
    await Promise.all(kids.map((w) => w.terminate()));

    // ---------- compare ----------
    console.log(`Mode B worlds/candidate ≈ ${Math.round([...bMap.values()][0]?.nsim || 0)}, batches ${Math.round(ex.wasm_mt_batches())}\n`);
    console.log('candidate                         Mode A EF   Mode B EF   |Δ|');
    let worst = 0, matched = 0;
    for (const [key, b] of bMap) {
        const a = aMap.get(key);
        if (a == null || b.mean == null) { console.log(`  ${b.label.padEnd(30)} (unmatched)`); continue; }
        const d = Math.abs(a - b.mean); worst = Math.max(worst, d); matched++;
        console.log(`  ${b.label.padEnd(30)}  ${a.toFixed(3)}      ${b.mean.toFixed(3)}      ${d.toFixed(3)}`);
    }
    const ok = matched > 0 && worst < 0.05;
    console.log(`\nmatched ${matched} candidates, worst |Δ| = ${worst.toFixed(4)}  →  ${ok ? 'PASS (Mode A ≈ Mode B)' : 'CHECK'}`);
    process.exit(ok ? 0 : 1);
}
