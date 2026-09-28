/* Mode B worker trampoline, node:worker_threads edition (§8b.4).
 * The browser twin is src/oracle/oracleMtWorker.ts; both do the same three
 * things and nothing else - instantiate over the shared memory, install this
 * thread's stack pointer and TLS base, enter the C loop, which never returns. */
import { parentPort, workerData } from 'node:worker_threads';

const mod = new WebAssembly.Module(workerData.bytes);
parentPort.once('message', ({ memory }) => {
    try {
        const inst = new WebAssembly.Instance(mod, { env: { memory } });
        inst.exports.__stack_pointer.value = workerData.stackTop;   // BEFORE any call
        inst.exports.__wasm_init_tls(workerData.tlsPtr);
        parentPort.postMessage({ t: 'ready', tid: workerData.tid });
        inst.exports.wasm_mt_thread_main(workerData.tid, workerData.stackLow);
    } catch (e) {
        parentPort.postMessage({ t: 'error', tid: workerData.tid, message: String((e && e.message) || e) });
    }
});
