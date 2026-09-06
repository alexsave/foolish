/* One Mode A fleet member, in plain JS (scripts/oracle_bench.mts).
 * Replays exactly the per-batch wasm work src/oracle/oracleBridge.ts does -
 * import state, strategy keys, logs, seed, reset the dump, choose, then read and
 * JSON.parse the dump - against its own private oracle.wasm instance. The only
 * thing it skips is the JS marshal's arithmetic, replaced by a memcpy of the
 * bytes that marshal produced on the main thread. */
import { parentPort, workerData } from 'node:worker_threads';

const { bytes, stateBytes, seat, numPlayers, logsWire, memoryOn, env, seconds, tid } = workerData;
const STRAT_OCTOGEN = 20;

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
        ex.wasm_choose_move(STRAT_OCTOGEN, seat);
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
