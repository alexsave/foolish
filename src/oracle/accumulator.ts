/* =============================================================================
 * Infinite Oracle — batch merge, standard error & convergence (§8.7)
 * Pure, worker-free: feed it dump records (from any number of workers, any
 * order) and read running per-candidate means, SE, and the convergence verdict.
 * Extracted from the controller so the headless suite can drive it directly.
 * ========================================================================== */

import {
    OracleDumpRecord, OracleDumpCandidate, OracleCandidate, OracleVerdict,
    ORACLE_TRUMP_KEEP, ORACLE_CONVERGE_MIN_N, ORACLE_CONVERGE_MAX_SE,
    canonicalMoveKey,
} from './types';

interface Welford { count: number; mean: number; m2: number; }
function welfordAdd(w: Welford, x: number): void {
    w.count++;
    const d = x - w.mean;
    w.mean += d / w.count;
    w.m2 += d * (x - w.mean);
}
/** SE of the pooled mean estimate = stddev(batch means) / √(#batch means). */
function welfordSE(w: Welford): number {
    if (w.count < 2) return Infinity;
    const variance = w.m2 / (w.count - 1);
    return Math.sqrt(variance) / Math.sqrt(w.count);
}

interface Acc {
    key: string;
    type: string;
    label: string;
    cards: string[];
    target?: string[];
    n: number;                 // cumulative nsim (worlds sampled for this candidate)
    sum: number;               // cumulative score * nsim
    bm: Welford;               // per-batch means (for SE)
    verdict: OracleVerdict;
    verdictVal?: number;
    forcedLoss: boolean;
    pruned: boolean;
    chosen: boolean;
}

const VERDICT_RANK: Record<string, number> = {
    win: 0, draw: 1, unknown: 2, none: 3, loss: 4, illegal: 5,
};

export class OracleAccumulator {
    private acc = new Map<string, Acc>();
    totalWorlds = 0;
    batches = 0;

    constructor(private opts: { deckAlive: boolean; recordedKey: string }) {}

    /** Merge one decision record (one worker's one batch). */
    add(rec: OracleDumpRecord): void {
        const cands = rec.candidates || [];
        let batchWorlds = 0;
        for (const c of cands) {
            const key = canonicalMoveKey(c.type, c.cards, c.target);
            let a = this.acc.get(key);
            if (!a) {
                a = {
                    key, type: c.type, label: c.label, cards: c.cards, target: c.target,
                    n: 0, sum: 0, bm: { count: 0, mean: 0, m2: 0 },
                    verdict: 'none', forcedLoss: false, pruned: false, chosen: false,
                };
                this.acc.set(key, a);
            }
            if (typeof c.nsim === 'number' && c.nsim > 0 && c.score != null) {
                a.n += c.nsim;
                a.sum += c.score * c.nsim;
                welfordAdd(a.bm, c.score);          // the record's mean IS this batch's mean
                if (c.nsim > batchWorlds) batchWorlds = c.nsim;
            }
            if (c.verdict && c.verdict !== 'none' && a.verdict === 'none') {
                a.verdict = c.verdict;
                a.verdictVal = c.verdict_val;
            }
            if (c.pruned) a.pruned = true;
            if (c.forced_loss) a.forcedLoss = true;
            if (c.chosen) a.chosen = true;
        }
        // nsim is uniform across candidates (racing off, common random numbers),
        // so one representative delta is the batch's true world count.
        this.totalWorlds += batchWorlds;
        this.batches++;
    }

    private trumpTax(a: Acc): number {
        if (a.type !== 'attack' || !this.opts.deckAlive) return 0;
        const nTrump = a.cards.filter((t) => t.endsWith('*')).length;
        return ORACLE_TRUMP_KEEP * nTrump;
    }

    hasWinLoss(): boolean {
        for (const a of this.acc.values())
            if (a.verdict === 'win' || a.verdict === 'loss') return true;
        return false;
    }

    /** Least-sampled and worst-SE over candidates with n > 0 (§8.7). */
    minN(): number {
        let m = Infinity;
        for (const a of this.acc.values()) if (a.n > 0) m = Math.min(m, a.n);
        return m;
    }
    maxSE(): number {
        let m = 0, any = false;
        for (const a of this.acc.values()) if (a.n > 0) { any = true; m = Math.max(m, welfordSE(a.bm)); }
        return any ? m : Infinity;
    }
    converged(): boolean {
        let any = false;
        for (const a of this.acc.values()) if (a.n > 0) { any = true; break; }
        if (!any) return false;
        return this.minN() >= ORACLE_CONVERGE_MIN_N || this.maxSE() <= ORACLE_CONVERGE_MAX_SE;
    }

    hasKey(key: string): boolean { return this.acc.has(key); }

    /** Snapshot the candidate list, sorted best-first. `exact` switches to
     *  verdict-rank ordering; otherwise ascending adjusted (mean + trump tax). */
    candidates(exact: boolean): OracleCandidate[] {
        const list: OracleCandidate[] = [];
        for (const a of this.acc.values()) {
            const mean = a.n > 0 ? a.sum / a.n : null;
            const tax = mean == null ? 0 : this.trumpTax(a);
            list.push({
                key: a.key, type: a.type, label: a.label, cards: a.cards, target: a.target,
                n: a.n,
                mean,
                se: welfordSE(a.bm),
                adjusted: mean == null ? null : mean + tax,
                verdict: a.verdict,
                verdictVal: a.verdictVal,
                forcedLoss: a.forcedLoss,
                pruned: a.pruned,
                chosen: a.chosen,
                played: a.key === this.opts.recordedKey,
            });
        }
        if (exact) {
            list.sort((x, y) =>
                (VERDICT_RANK[x.verdict] ?? 9) - (VERDICT_RANK[y.verdict] ?? 9)
                || depthOf(y) - depthOf(x));
        } else {
            list.sort((x, y) => {
                if (x.adjusted == null && y.adjusted == null) return 0;
                if (x.adjusted == null) return 1;      // scoreless rows last
                if (y.adjusted == null) return -1;
                return x.adjusted - y.adjusted;
            });
        }
        return list;
    }
}

// verdict_val encodes ±(1000 - depth); a shallower win/loss ranks stronger.
function depthOf(c: OracleCandidate): number {
    return c.verdictVal != null ? 1000 - Math.abs(c.verdictVal) : 1000;
}
