/* =============================================================================
 * Infinite Oracle — batch merge, standard error & convergence (§8.7)
 * Pure, worker-free: feed it dump records (from any number of workers, any
 * order) and read running per-candidate means, SE, and the convergence verdict.
 * Extracted from the controller so the headless suite can drive it directly.
 * ========================================================================== */

import {
    OracleDumpRecord, OracleCandidate, OracleVerdict, OracleCandPaths,
    OraclePathStat, OracleReplyStat,
    ORACLE_TRUMP_KEEP, ORACLE_CONVERGE_MIN_N, ORACLE_CONVERGE_MAX_SE,
    canonicalMoveKey,
} from './types';
import { decodePathsBlob } from './pathsBlob';

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
    // ---- "why" sidecar pools (merged across batches) ----
    whyN: number;                                          // playouts folded
    whyMepk: number; whyOppk: number;                      // Σ mean·n (weighted)
    whyMetr: number; whyOpptr: number; whyRnds: number;
    pathPool: Map<string, { seq: number[]; n: number; finSum: number }>;
    replyPool: Map<number, { type: number; card: number; n: number }>;
}

const VERDICT_RANK: Record<string, number> = {
    win: 0, draw: 1, unknown: 2, none: 3, loss: 4, illegal: 5,
};

export class OracleAccumulator {
    private acc = new Map<string, Acc>();
    totalWorlds = 0;
    batches = 0;

    constructor(private opts: { deckAlive: boolean; recordedKey: string }) {}

    /** Decision-static context captured from the first record that carries a
     *  belief block (identical every batch — the belief is a function of the
     *  marshaled position, not of the sampling). */
    belief: OracleDumpRecord['belief'] | null = null;
    beliefCtx: {
        hand: string[]; oppCounts: number[];
        table: { attack: string; defense: string | null }[];
        defender: number; trump: number;
    } | null = null;

    /** Merge one decision record (one worker's one batch), plus its optional
     *  binary paths sidecar. */
    add(rec: OracleDumpRecord, pathsBlob?: ArrayBuffer): void {
        const cands = rec.candidates || [];
        const why = pathsBlob ? decodePathsBlob(pathsBlob) : null;
        if (!this.belief && rec.belief) {
            this.belief = rec.belief;
            this.beliefCtx = {
                hand: rec.hand ?? [],
                oppCounts: rec.opp_counts ?? [],
                table: rec.table ?? [],
                defender: rec.defender,
                trump: rec.trump,
            };
        }
        let batchWorlds = 0;
        for (let ci = 0; ci < cands.length; ci++) {
            const c = cands[ci];
            const key = canonicalMoveKey(c.type, c.cards, c.target);
            let a = this.acc.get(key);
            if (!a) {
                a = {
                    key, type: c.type, label: c.label, cards: c.cards, target: c.target,
                    n: 0, sum: 0, bm: { count: 0, mean: 0, m2: 0 },
                    verdict: 'none', forcedLoss: false, pruned: false, chosen: false,
                    whyN: 0, whyMepk: 0, whyOppk: 0, whyMetr: 0, whyOpptr: 0, whyRnds: 0,
                    pathPool: new Map(), replyPool: new Map(),
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
            // sidecar entries key by candidate INDEX within this record
            const w = why?.get(ci);
            if (w && w.agg.n > 0) {
                a.whyN += w.agg.n;
                a.whyMepk += w.agg.mepk * w.agg.n;
                a.whyOppk += w.agg.oppk * w.agg.n;
                a.whyMetr += w.agg.metr * w.agg.n;
                a.whyOpptr += w.agg.opptr * w.agg.n;
                a.whyRnds += w.agg.rnds * w.agg.n;
                for (const p of w.paths) {
                    const pk = p.seq.join('');
                    const e = a.pathPool.get(pk);
                    if (e) { e.n += p.n; e.finSum += p.fin * p.n; }
                    else a.pathPool.set(pk, { seq: p.seq, n: p.n, finSum: p.fin * p.n });
                }
                for (const r of w.replies) {
                    const rk = r.type * 64 + r.card;
                    const e = a.replyPool.get(rk);
                    if (e) e.n += r.n;
                    else a.replyPool.set(rk, { type: r.type, card: r.card, n: r.n });
                }
            }
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
            let why: OracleCandidate['why'];
            if (a.whyN > 0) {
                const paths: OraclePathStat[] = [...a.pathPool.values()]
                    .sort((x, y) => y.n - x.n)
                    .slice(0, 6)
                    .map((p) => ({ seq: p.seq, n: p.n, fin: p.finSum / p.n }));
                const replies: OracleReplyStat[] = [...a.replyPool.values()]
                    .sort((x, y) => y.n - x.n)
                    .slice(0, 3);
                why = {
                    agg: {
                        n: a.whyN,
                        mepk: a.whyMepk / a.whyN,
                        oppk: a.whyOppk / a.whyN,
                        metr: a.whyMetr / a.whyN,
                        opptr: a.whyOpptr / a.whyN,
                        rnds: a.whyRnds / a.whyN,
                    },
                    replies,
                    paths,
                } satisfies OracleCandPaths;
            }
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
                why,
            });
        }
        if (exact) {
            list.sort((x, y) =>
                (VERDICT_RANK[x.verdict] ?? 9) - (VERDICT_RANK[y.verdict] ?? 9)
                || depthOf(y) - depthOf(x));
        } else {
            // Sort by the true expected finish (mean) so the order matches the
            // displayed numbers + bars; the trump tax is only an invisible
            // tie-break for exactly-equal means (octogen's trump-conservation).
            list.sort((x, y) => {
                if (x.mean == null && y.mean == null) return 0;
                if (x.mean == null) return 1;          // scoreless rows last
                if (y.mean == null) return -1;
                return (x.mean - y.mean) || ((x.adjusted ?? x.mean) - (y.adjusted ?? y.mean));
            });
        }
        return list;
    }
}

// verdict_val encodes ±(1000 - depth); a shallower win/loss ranks stronger.
function depthOf(c: OracleCandidate): number {
    return c.verdictVal != null ? 1000 - Math.abs(c.verdictVal) : 1000;
}
