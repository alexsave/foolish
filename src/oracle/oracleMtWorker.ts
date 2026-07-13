/* =============================================================================
 * Infinite Oracle — Mode B worker trampoline (§8b.4)
 * The ONLY worker code in Mode B: instantiate oracle-mt.wasm over the shared
 * memory, set this thread's stack pointer + TLS base, and call the C thread loop
 * (which never returns — it parks on memory.atomic.wait until the control
 * instance publishes a job). All coordination lives in C; there is nothing else
 * to do here.
 * ========================================================================== */

interface MtExports {
    __stack_pointer: WebAssembly.Global;
    __wasm_init_tls(ptr: number): void;
    wasm_mt_thread_main(tid: number): void;
}

self.onmessage = (e: MessageEvent) => {
    const { bytes, memory, tid, stackTop, tlsPtr } = e.data as {
        bytes: Uint8Array; memory: WebAssembly.Memory;
        tid: number; stackTop: number; tlsPtr: number;
    };
    const mod = new WebAssembly.Module(bytes as BufferSource);
    const inst = new WebAssembly.Instance(mod, { env: { memory } });
    const ex = inst.exports as unknown as MtExports;
    ex.__stack_pointer.value = stackTop;   // BEFORE any other export call
    ex.__wasm_init_tls(tlsPtr);
    ex.wasm_mt_thread_main(tid);           // never returns
};
