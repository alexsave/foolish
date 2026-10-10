/* =============================================================================
 * Infinite Oracle - one wasm instance's bridge (worker-side, §8.5)
 * Talks to a single oracle.wasm instance: instantiate, set the knobs, and run
 * one deliberation batch in the exact load-bearing call order (§4.3). The
 * position and the memory are the kernel's bytes (OracleJob.state, .logsWire),
 * written into the instance unchanged, and the answer is the kernel's struct
 * (c/src/oracle_answer.h OgAnswer), read through the reader structgen writes
 * from it. Nothing here knows an offset, and nothing is formatted or parsed.
 * Never shared between workers.
 * ========================================================================== */

import {
    memOf, readOgAnswer, writeOgKnobs, type OgAnswer_Snap, type Card_Snap,
    MOVE_ATTACK, MOVE_COVER, MOVE_PASS, MOVE_PICKUP, MOVE_GOOD, MOVE_WAIT,
    OG_VERDICT_NONE, OG_VERDICT_UNKNOWN, OG_VERDICT_ILLEGAL,
    OG_VERDICT_WIN, OG_VERDICT_LOSS, OG_VERDICT_DRAW,
    OG_SOLVER_NONE, OG_SOLVER_WIN, OG_SOLVER_EVAL,
} from '@sdk/ts/gen/oracle_answer.oracle.ts';
import type { OracleExports } from '@sdk/ts/gen/exports.oracle.ts';
import {
    OracleJob, OracleKnobs, OracleRecord, OracleRecordCandidate, OracleVerdict, oracleCardToken,
} from './types';

const STRAT_OCTOGEN = 20;

export type BatchResult =
    | { record: OracleRecord }
    | { error: 'empty' | 'state' };

/** The instance's linear memory, as bytes: a fresh view on every call (a choose may grow it). */
const bytesOf = (ex: OracleExports) => new Uint8Array(ex.memory.buffer);

// The names src/oracle keys its rows by, against the kernel's own ids.
const MOVE_NAME: Record<number, string> = {
    [MOVE_ATTACK]: 'attack', [MOVE_COVER]: 'cover', [MOVE_PASS]: 'pass',
    [MOVE_PICKUP]: 'pickup', [MOVE_GOOD]: 'good', [MOVE_WAIT]: 'wait',
};
const VERDICT_NAME: Record<number, OracleVerdict> = {
    [OG_VERDICT_NONE]: 'none', [OG_VERDICT_UNKNOWN]: 'unknown', [OG_VERDICT_ILLEGAL]: 'illegal',
    [OG_VERDICT_WIN]: 'win', [OG_VERDICT_LOSS]: 'loss', [OG_VERDICT_DRAW]: 'draw',
};
const SOLVER_NAME: Record<number, OracleRecord['solver']['result']> = {
    [OG_SOLVER_NONE]: 'none', [OG_SOLVER_WIN]: 'win', [OG_SOLVER_EVAL]: 'eval',
};

/** The kernel's answer as the record the accumulator, the worker and the
 *  overlay read: cards become the tokens rows are keyed by, codes become their
 *  names, and each sampled row is handed its own storylines. */
export function recordOfAnswer(a: OgAnswer_Snap): OracleRecord {
    const tok = (c: Card_Snap) => oracleCardToken(c, a.trump);
    const candidates = a.rows.map((r): OracleRecordCandidate => {
        const c: OracleRecordCandidate = {
            type: MOVE_NAME[r.type] ?? '?',
            label: r.label,
            cards: r.cards.map(tok),
            score: r.scored ? r.score : null,
            nsim: r.nsim,
            alive: r.alive ? 1 : 0,
            forced_loss: r.forcedLoss ? 1 : 0,
            verdict: VERDICT_NAME[r.verdict] ?? 'none',
            chosen: r.chosen ? 1 : 0,
        };
        if (r.type === MOVE_COVER) c.target = r.targets.map(tok);
        if (r.pruned) c.pruned = 1;
        if (r.hasVerdictVal) c.verdict_val = r.verdictVal;
        return c;
    });
    for (const w of a.why) {
        candidates[w.row].why = {
            agg: { n: w.n, mepk: w.mepk, oppk: w.oppk, metr: w.metr, opptr: w.opptr, rnds: w.rnds },
            replies: w.replies.map((r) => ({ type: r.type, card: r.card, n: r.n })),
            paths: w.paths.map((p) => ({ seq: [...p.sym], n: p.n, fin: p.fin })),
        };
    }
    return {
        seat: a.seat,
        deck: a.deck,
        defender: a.defender,
        trump: a.trump,
        belief: {
            pinned: a.pinned.map((p) => p.cards.map(tok)),
            poolCount: a.nPool,
            voids: a.voids.map((v) => v.cards.map(tok)),
            floor: [...a.floor],
        },
        hand: a.hand.map(tok),
        opp_counts: [...a.handCount],
        table: a.table.map((b) => ({ attack: tok(b.attack), defense: b.covered ? tok(b.defense) : null })),
        solver: { applied: a.solverApplied ? 1 : 0, result: SOLVER_NAME[a.solverResult] ?? 'none' },
        candidates,
    };
}

export class OracleInstance {
    ex!: OracleExports;

    async init(bytes: Uint8Array): Promise<void> {
        const src = await WebAssembly.instantiate(bytes as BufferSource, {});
        const instance = (src as WebAssembly.WebAssemblyInstantiatedSource).instance;
        this.ex = instance.exports as unknown as OracleExports;
        this.ex.wasm_init();
    }

    /** Set the OG_* knobs (§8.5 step 1); octogen reads them at its next
     *  choose. Every knob not in the struct goes back to its default. Only
     *  called when the knobs are dirty (first batch or a retune). */
    setKnobs(knobs: OracleKnobs): void {
        const ex = this.ex;
        writeOgKnobs(memOf(ex.memory.buffer), ex.wasm_og_knobs_ptr(), knobs);
        ex.wasm_og_knobs_apply();
    }

    /** One deliberation batch (§8.5 steps 2-8). Imports the position fresh,
     *  seeds, chooses, and reads the answer. Order is load-bearing (§4.3). */
    analyzeOnce(
        job: Pick<OracleJob, 'state' | 'seat' | 'numPlayers' | 'logsWire' | 'memoryOn'>,
        seed: number,
    ): BatchResult {
        const ex = this.ex;
        const seat = job.seat;

        // 2. a fresh import every batch: the board as the acting seat saw it,
        //    every card it could not see hidden. The kernel judges it first.
        bytesOf(ex).set(job.state, ex.wasm_io_ptr());
        if (ex.wasm_import_state(job.state.length, 1) < 0) return { error: 'state' };

        // 3. strategy keys: one i8 -1 per seat. Inert for this module (no
        //    espresso_prod), but the call reads num_players bytes unconditionally.
        {
            const buf = bytesOf(ex);
            const q = ex.wasm_io_ptr();
            for (let i = 0; i < job.numPlayers; i++) buf[q + i] = 0xff;
            ex.wasm_import_strategy_keys();
        }

        // 4. session logs (memory ON only): the u16 count header + records, whole.
        if (job.memoryOn && job.logsWire.length > 2) {
            bytesOf(ex).set(job.logsWire, ex.wasm_io_ptr());
            ex.wasm_import_logs();
        }

        // 5. seed  6. forget the last answer  7. choose
        ex.wasm_set_strategy_seed(seed >>> 0);
        ex.wasm_og_answer_reset();
        ex.wasm_choose_move(STRAT_OCTOGEN, seat);

        // 8. read the answer, over a view taken AFTER the choose (it may have
        //    grown linear memory via the TT bump alloc, detaching earlier ones).
        //    The snapshot is a copy: the struct is rewritten by the next batch.
        const at = ex.wasm_og_answer();
        if (!at) return { error: 'empty' };
        return { record: recordOfAnswer(readOgAnswer(memOf(ex.memory.buffer), at)) };
    }
}
