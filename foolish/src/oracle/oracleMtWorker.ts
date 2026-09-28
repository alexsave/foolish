/* =============================================================================
 * Infinite Oracle - Mode B worker trampoline (§8b.4)
 * The ONLY worker code in Mode B: instantiate oracle-mt.wasm over the shared
 * memory, set this thread's stack pointer + TLS base, and call the C thread loop
 * (which never returns - it parks on memory.atomic.wait until the control
 * instance publishes a job). All coordination lives in C; there is nothing else
 * to do here.
 *
 * The one message back is 'ready', posted after the TLS base is installed and
 * immediately before the loop is entered. The controller waits for all N of them
 * rather than sleeping a guessed interval; the loop re-checks the generation
 * before it parks, so a job armed in the gap is not missed either way.
 * ========================================================================== */

interface MtExports {
    __stack_pointer: WebAssembly.Global;
    __wasm_init_tls(ptr: number): void;
    wasm_mt_thread_main(tid: number, stackLow: number): void;
}

self.onmessage = (e: MessageEvent) => {
    const { bytes, memory, tid, stackTop, stackLow, tlsPtr } = e.data as {
        bytes: Uint8Array; memory: WebAssembly.Memory;
        tid: number; stackTop: number; stackLow: number; tlsPtr: number;
    };
    try {
        const mod = new WebAssembly.Module(bytes as BufferSource);
        const inst = new WebAssembly.Instance(mod, { env: { memory } });
        const ex = inst.exports as unknown as MtExports;
        ex.__stack_pointer.value = stackTop;   // BEFORE any other export call
        ex.__wasm_init_tls(tlsPtr);
        self.postMessage({ t: 'ready', tid });
        ex.wasm_mt_thread_main(tid, stackLow); // never returns
    } catch (err) {
        self.postMessage({ t: 'error', tid, message: err instanceof Error ? err.message : String(err) });
    }
};
