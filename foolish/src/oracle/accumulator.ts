/* =============================================================================
 * Infinite Oracle — batch merge, standard error & convergence (§8.7)
 * Pure, worker-free: feed it batch records (from any number of workers, any
 * order) and read running per-candidate means, SE, and the convergence verdict.
 * Extracted from the controller so the headless suite can drive it directly.
 * ========================================================================== */

import {
    OracleRecord, OracleCandidate, OracleVerdict, OracleCandPaths,
    OraclePathStat, OracleReplyStat, OracleRanker, OracleTable,
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
    sn: number; ssum: number; sbm: Welford;   // the same three over fusion's noisy race
    verdict: OracleVerdict;
    verdictVal?: number;
    forcedLoss: boolean;
    pruned: boolean;
    chosen: boolean;
    // ---- "why" pools (merged across batches) ----
    whyN: number;                                          // playouts folded
    whyMepk: number; whyOppk: number;                      // sum of mean*n (weighted)
    whyMetr: number; whyOpptr: number; whyRnds: number;
    pathPool: Map<string, { seq: number[]; n: number; finSum: number }>;
    replyPool: Map<number, { type: number; card: number; n: number }>;
}

export class OracleAccumulator {
    private acc = new Map<string, Acc>();
    totalWorlds = 0;
    batches = 0;

    constructor(private opts: { table: OracleTable; recordedKey: string; ranker: OracleRanker }) {}

    /** Decision-static context captured from the first record that carries a
     *  belief block (identical every batch - the belief is a function of the
     *  marshaled position, not of the sampling). */
    belief: OracleRecord['belief'] | null = null;
    beliefCtx: {
        hand: string[]; oppCounts: number[];
        table: { attack: string; defense: string | null }[];
        defender: number; trump: number;
    } | null = null;

    /** Merge one decision record (one worker's one batch), each sampled row
     *  with the storylines behind its score. */
    add(rec: OracleRecord): void {
        const cands = rec.candidates || [];
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
        let batchWorlds = 0, shadowWorlds = 0;
        for (const c of cands) {
            const key = canonicalMoveKey(c.type, c.cards, c.target);
            let a = this.acc.get(key);
            if (!a) {
                a = {
                    key, type: c.type, label: c.label, cards: c.cards, target: c.target,
                    n: 0, sum: 0, bm: { count: 0, mean: 0, m2: 0 },
                    sn: 0, ssum: 0, sbm: { count: 0, mean: 0, m2: 0 },
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
            if (c.shadow_nsim > 0 && c.shadow_score != null) {
                a.sn += c.shadow_nsim;
                a.ssum += c.shadow_score * c.shadow_nsim;
                welfordAdd(a.sbm, c.shadow_score);
                if (c.shadow_nsim > shadowWorlds) shadowWorlds = c.shadow_nsim;
            }
            if (c.verdict && c.verdict !== 'none' && a.verdict === 'none') {
                a.verdict = c.verdict;
                a.verdictVal = c.verdict_val;
            }
            if (c.pruned) a.pruned = true;
            if (c.forced_loss) a.forcedLoss = true;
            if (c.chosen) a.chosen = true;
            const w = c.why;
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
        // so one representative delta is the batch's true world count. The
        // count shown is the noisy race's, whose numbers the panel prints, and
        // the noise-free race's only on a decision with no noisy race.
        this.totalWorlds += shadowWorlds || batchWorlds;
        this.batches++;
    }

    hasWinLoss(): boolean {
        for (const a of this.acc.values())
            if (a.verdict === 'win' || a.verdict === 'loss') return true;
        return false;
    }

    /** The convergence checkpoint (§8.7), the kernel's, on the numbers the
     *  panel shows (c/src/oracle_rank.c). */
    converged(): boolean { return this.opts.ranker.rank(this.rows(), false, this.opts.table).converged; }

    hasKey(key: string): boolean { return this.acc.has(key); }

    /** Snapshot the candidate list in the kernel's order, with its Best,
     *  pick, classes and bars (c/src/oracle_rank.c). `exact` switches it to the
     *  verdict ordering. */
    candidates(exact: boolean): OracleCandidate[] {
        return this.opts.ranker.rank(this.rows(), exact, this.opts.table).rows;
    }

    /** Every accumulated row, unranked. */
    private rows(): OracleCandidate[] {
        const list: OracleCandidate[] = [];
        for (const a of this.acc.values()) {
            const mean = a.n > 0 ? a.sum / a.n : null;
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
                shadowN: a.sn,
                shadowMean: a.sn > 0 ? a.ssum / a.sn : null,
                shadowSe: welfordSE(a.sbm),
                ef: null, best: false, pick: false, cls: null, bar: 0,
                verdict: a.verdict,
                verdictVal: a.verdictVal,
                forcedLoss: a.forcedLoss,
                pruned: a.pruned,
                chosen: a.chosen,
                played: a.key === this.opts.recordedKey,
                why,
            });
        }
        return list;
    }
}
