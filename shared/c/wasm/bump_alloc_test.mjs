// Runs bump_alloc_test.c's module(s).
//
//   node bump_alloc_test.mjs build/bump_alloc_test.wasm build/bump_alloc_test_mt.wasm
//
// The checks are in C: bump_alloc_test() and bump_alloc_hammer() return 0, or
// the line of the check that failed. This file only instantiates. What it has
// to know is that the -matomics build imports its memory, shared, and that a
// thread is a worker with an instance of its own over that memory and a stack
// of its own (every instance starts with the same __stack_pointer, so the
// runner allocates one per thread and installs it before the first call).
import { readFileSync } from 'node:fs';
import { Worker, isMainThread, parentPort, workerData } from 'node:worker_threads';

const THREADS = 4, ROUNDS = 200000, STACK = 65536;

if (!isMainThread) {
    try {
        const inst = new WebAssembly.Instance(new WebAssembly.Module(workerData.bytes), { env: { memory: workerData.memory } });
        inst.exports.__stack_pointer.value = workerData.stackTop;
        parentPort.postMessage({ ready: true });
        parentPort.postMessage({ line: inst.exports.bump_alloc_hammer(workerData.tid, ROUNDS) });
    } catch (e) {
        parentPort.postMessage({ trap: String((e && e.message) || e) });
    }
} else {
    let failed = 0;
    const fail = (what) => { failed++; console.error(`FAIL ${what}`); };
    for (const path of process.argv.slice(2)) {
        const bytes = readFileSync(path);
        const mod = new WebAssembly.Module(bytes);
        const threaded = WebAssembly.Module.imports(mod).some((i) => i.kind === 'memory');
        // The limits bump_alloc_test() is written against: 2 pages, 16 at most.
        const small = threaded ? { env: { memory: new WebAssembly.Memory({ initial: 2, maximum: 16, shared: true }) } } : {};
        const line = new WebAssembly.Instance(mod, small).exports.bump_alloc_test();
        if (line) fail(`${path}: bump_alloc_test.c:${line}`);
        else console.log(`ok ${path}`);
        if (!threaded) continue;

        const memory = new WebAssembly.Memory({ initial: 2, maximum: 1024, shared: true });
        const main = new WebAssembly.Instance(mod, { env: { memory } }).exports;
        const stacks = main.malloc(THREADS * STACK);
        const workers = [], results = [];
        let ready = 0;
        await new Promise((done) => {
            for (let tid = 0; tid < THREADS; tid++) {
                const w = new Worker(new URL(import.meta.url), { workerData: { bytes, memory, tid, stackTop: stacks + (tid + 1) * STACK } });
                workers.push(w);
                w.on('message', (m) => {
                    // Open the gate only when every thread is parked at it: ready
                    // is posted just before the call that parks.
                    if (m.ready) {
                        if (++ready === THREADS) setTimeout(() => {
                            const line = main.bump_alloc_go();
                            if (line) fail(`${path}: bump_alloc_test.c:${line} (the gate)`);
                        }, 100);
                        return;
                    }
                    results.push({ tid, ...m });
                    if (results.length === THREADS) done();
                });
                w.on('error', (e) => { results.push({ tid, trap: String(e) }); if (results.length === THREADS) done(); });
            }
        });
        await Promise.all(workers.map((w) => w.terminate()));
        const bad = results.filter((r) => r.trap !== undefined || r.line !== 0);
        for (const r of bad) fail(`${path}: thread ${r.tid}: ${r.trap ?? `bump_alloc_test.c:${r.line}`}`);
        if (!bad.length) console.log(`ok ${path}: ${THREADS} threads, ${ROUNDS} allocations each, ${memory.buffer.byteLength / 65536} pages`);
    }
    if (!process.argv[2]) fail('bump_alloc_test.mjs: no module given');
    process.exit(failed ? 1 : 0);
}
