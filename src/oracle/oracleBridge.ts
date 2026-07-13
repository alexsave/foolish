/* =============================================================================
 * Infinite Oracle — one wasm instance's bridge (worker-side, §8.5)
 * Talks to a single oracle.wasm instance: instantiate, install env, and run one
 * deliberation batch in the exact load-bearing call order (§4.3). Reuses the
 * engine.ts marshal helpers (NOT bots.ts — that singleton would hijack the
 * replay screen's rules instance). Never shared between workers.
 * ========================================================================== */

import { Game } from '@shared/types.ts';
import { __marshalGame, __mem, __setResident } from '@shared/wasm/engine.ts';
import { OracleGameState, OracleDumpRecord } from './types';

const STRAT_OCTOGEN = 20;

interface OracleExports {
    memory: WebAssembly.Memory;
    wasm_init(): void;
    wasm_io_ptr(): number;
    wasm_import_state(): void;
    wasm_import_strategy_keys(): void;
    wasm_import_logs(): void;
    wasm_clearenv(): void;
    wasm_setenv_from_io(): void;
    wasm_og_reload_flags(): void;
    wasm_set_strategy_seed(s: number): void;
    wasm_choose_move(strat: number, seat: number): number;
    wasm_og_explain_ptr(): number;
    wasm_og_explain_len(): number;
    wasm_og_explain_reset(): void;
}

export type BatchResult =
    | { record: OracleDumpRecord }
    | { error: 'empty' | 'parse' | 'overflow' };

export class OracleInstance {
    ex!: OracleExports;
    private decoder = new TextDecoder();

    async init(bytes: Uint8Array): Promise<void> {
        const src = await WebAssembly.instantiate(bytes as BufferSource, {});
        const instance = (src as WebAssembly.WebAssemblyInstantiatedSource).instance;
        this.ex = instance.exports as unknown as OracleExports;
        this.ex.wasm_init();
    }

    /** Rewrite the OG_* env table and force octogen to re-read it (§8.5 step 1).
     *  Only called when the env is dirty (first batch or W1 retune). */
    writeEnv(env: Record<string, string>): void {
        const ex = this.ex;
        ex.wasm_clearenv();
        const base = ex.wasm_io_ptr();
        for (const [k, v] of Object.entries(env)) {
            const buf = __mem(ex as never);
            let q = base;
            for (let i = 0; i < k.length; i++) buf[q++] = k.charCodeAt(i) & 0xff;
            buf[q++] = 0;
            for (let i = 0; i < v.length; i++) buf[q++] = v.charCodeAt(i) & 0xff;
            buf[q++] = 0;
            ex.wasm_setenv_from_io();
        }
        ex.wasm_og_reload_flags();
    }

    /** One deliberation batch (§8.5 steps 2-9). Marshals fresh, seeds, chooses,
     *  reads + parses the first dump line. Order is load-bearing (§4.3). */
    analyzeOnce(
        blob: OracleGameState,
        seat: number,
        logsWire: Uint8Array,
        memoryOn: boolean,
        seed: number,
    ): BatchResult {
        const ex = this.ex;

        // 2. fresh marshal every batch — never consume a stale resident mark.
        __setResident(null);
        __marshalGame(ex as never, blob as unknown as Game);

        // 3. strategy keys: one i8 -1 per seat. Inert for this module (no
        //    espresso_prod), but the call reads num_players bytes unconditionally.
        {
            const buf = __mem(ex as never);
            const q = ex.wasm_io_ptr();
            for (let i = 0; i < blob.players.length; i++) buf[q + i] = 0xff;
            ex.wasm_import_strategy_keys();
        }

        // 4. session logs (memory ON only). logsWire already carries the u16
        //    count header + records — write it whole at the io ptr.
        if (memoryOn && logsWire.length > 2) {
            const buf = __mem(ex as never);
            buf.set(logsWire, ex.wasm_io_ptr());
            ex.wasm_import_logs();
        }

        // 5. seed  6. reset dump  7. choose
        ex.wasm_set_strategy_seed(seed >>> 0);
        ex.wasm_og_explain_reset();
        ex.wasm_choose_move(STRAT_OCTOGEN, seat);

        // 8. read the dump — refresh the view EVERY read (choose may have grown
        //    linear memory via the TT bump alloc, invalidating cached buffers).
        const len = ex.wasm_og_explain_len();
        if (len <= 0) return { error: 'empty' };
        const ptr = ex.wasm_og_explain_ptr();
        const buf = __mem(ex as never);
        const text = this.decoder.decode(buf.subarray(ptr, ptr + len));
        const nl = text.indexOf('\n');
        const line = nl >= 0 ? text.slice(0, nl) : text;

        // 9. a parse failure or an {"overflow":1} line is a REAL signal (§6.3):
        //    surface it, don't retry-loop a malformed dump.
        if (line.indexOf('"overflow"') >= 0) return { error: 'overflow' };
        try {
            return { record: JSON.parse(line) as OracleDumpRecord };
        } catch {
            return { error: 'parse' };
        }
    }
}
