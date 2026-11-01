// Infinite Oracle - the row order, Best and the classes (oracle_rank.h,
// docs/INFINITE_ORACLE_DESIGN.md "Ties, Best and the classes").
//
// THE RULE, Monte Carlo regime:
//
//   o     octogen's pick: the first row by expected finish (mean), then the mean
//         plus the trump-keep tax, then input order.
//   pick  fusion's pick: o, unless the noisy race's lowest row f (its mean plus
//         tax; the first such row in o's order on a tie) beats o's by more than
//         FUSION_SHADOW tenths of their combined standard error. Fusion's own
//         rule (octogen_strategy.c og_fusion_shadow) over everything sampled.
//   tie   two rows tie when their means differ by no more than
//         max(OG_RANK_TIE_FLOOR, k * sqrt(se_a^2 + se_b^2)), k = FUSION_SHADOW/10:
//         the same two standard errors fusion holds its own pick to. An error
//         not measured yet (one batch, se infinite) counts as 0 here, so before
//         the run has an error only rows within the floor tie.
//   Best  every row that ties with the pick. All of them read Best; `pick`
//         marks the one fusion plays.
//   class a row tied with the pick or with o is best. Any other row measures its
//         gap to the nearest of those two numbers at or below it: the pick's
//         mean when its own is at least that, else o's (the lowest). Then the
//         thresholds: < 0.05 excellent, < 0.15 good, < 0.35 inaccuracy, < 0.7
//         mistake, else blunder. When pick is o, as it is on most decisions,
//         that is simply the gap to the best row. When fusion overrules, the
//         rows above its pick keep the lowest number's yardstick, so the
//         lowest row still reads best (it has the lowest expected finish) and
//         the pick's group reads Best (fusion plays one of them).
//   order by mean, then the taxed mean, then input order; except that the Best
//         rows sit together as one block where their lowest mean puts it, the
//         pick first, the rest of the block by mean. The played move is never
//         moved: it is wherever its mean puts it.
//   bar   the X-ray pages' formula: 96 - 84 * (mean - lowest) / span, span the
//         spread of the means floored at 0.4, clamped to [12, 100].
//
// Exact regime (a proven win or loss): by verdict, win, draw, unknown, none,
// loss, illegal; within a verdict by its value, larger first (+-(1000 - depth):
// the quickest win and the longest loss lead); then the mean; then input order.
// No Best and no classes: the verdicts are the answer.
#include "oracle_rank.h"

#ifdef FOOLISH_ORACLE_BUILD
#include "octogen_strategy.h"

#ifdef OG_EXPLAIN_BUILD
_Static_assert(OG_RANK_MAX_ROWS == OG_ANSWER_MAX_ROWS, "one row per answer row");
#endif

#define OG_RANK_K (FUSION_SHADOW / 10.0)

// Per call, not per thread: only the host's own thread ranks (Mode B's main
// thread, Mode A's), never a sampling thread.
static struct {
    const OgRankAsk *ask;
    double mean[OG_RANK_MAX_ROWS], adj[OG_RANK_MAX_ROWS], se[OG_RANK_MAX_ROWS];
    double sadj[OG_RANK_MAX_ROWS], sse[OG_RANK_MAX_ROWS];
    double gkey[OG_RANK_MAX_ROWS];
    uint8_t prio[OG_RANK_MAX_ROWS];
    uint16_t ord[OG_RANK_MAX_ROWS], tmp[OG_RANK_MAX_ROWS];
} W;

// The error as measured, or Mode B's n-based proxy when the host has none.
static double og_rank_se(double se, int32_t n) {
    if (se >= 0.0) return se;
    return n > 0 ? OG_RANK_SE_PROXY / __builtin_sqrt((double)n) : __builtin_inf();
}

// The error a tie is judged at: an unmeasured one is 0, never infinite.
static double og_rank_tie_se(double se) { return __builtin_isfinite(se) ? se : 0.0; }

static bool og_rank_tie(int a, int b) {
    const double d = W.mean[a] - W.mean[b];
    const double sa = og_rank_tie_se(W.se[a]), sb = og_rank_tie_se(W.se[b]);
    double tol2 = OG_RANK_K * OG_RANK_K * (sa * sa + sb * sb);
    if (tol2 < OG_RANK_TIE_FLOOR * OG_RANK_TIE_FLOOR) tol2 = OG_RANK_TIE_FLOOR * OG_RANK_TIE_FLOOR;
    return d * d <= tol2;
}

static int og_rank_cmp_d(double a, double b) { return a < b ? -1 : a > b ? 1 : 0; }

// o's order: scored rows first, by mean, taxed mean, input order.
static int og_rank_cmp_base(int a, int b) {
    const OgRankRow *ra = &W.ask->rows[a], *rb = &W.ask->rows[b];
    if (ra->scored != rb->scored) return ra->scored ? -1 : 1;
    if (ra->scored) {
        int c = og_rank_cmp_d(W.mean[a], W.mean[b]);
        if (!c) c = og_rank_cmp_d(W.adj[a], W.adj[b]);
        if (c) return c;
    }
    return a - b;
}

// The display order: the Best block keyed at its lowest mean, pick first.
static int og_rank_cmp_shown(int a, int b) {
    const OgRankRow *ra = &W.ask->rows[a], *rb = &W.ask->rows[b];
    if (ra->scored != rb->scored) return ra->scored ? -1 : 1;
    if (ra->scored) {
        int c = og_rank_cmp_d(W.gkey[a], W.gkey[b]);
        if (!c) c = (int)W.prio[a] - (int)W.prio[b];
        if (!c) c = og_rank_cmp_d(W.mean[a], W.mean[b]);
        if (!c) c = og_rank_cmp_d(W.adj[a], W.adj[b]);
        if (c) return c;
    }
    return a - b;
}

static int og_rank_verdict_order(uint8_t v) {
    switch (v) {
    case OG_VERDICT_WIN: return 0;
    case OG_VERDICT_DRAW: return 1;
    case OG_VERDICT_UNKNOWN: return 2;
    case OG_VERDICT_NONE: return 3;
    case OG_VERDICT_LOSS: return 4;
    case OG_VERDICT_ILLEGAL: return 5;
    default: return 6;
    }
}

static int og_rank_cmp_exact(int a, int b) {
    const OgRankRow *ra = &W.ask->rows[a], *rb = &W.ask->rows[b];
    int c = og_rank_verdict_order(ra->verdict) - og_rank_verdict_order(rb->verdict);
    if (c) return c;
    if (ra->has_verdict_val != rb->has_verdict_val) return ra->has_verdict_val ? -1 : 1;
    if (ra->has_verdict_val && ra->verdict_val != rb->verdict_val)
        return ra->verdict_val > rb->verdict_val ? -1 : 1;
    return og_rank_cmp_base(a, b);
}

// A stable bottom-up merge sort of W.ord (every comparator ends on input order
// anyway, so stability is belt and braces, not the tie-break).
static void og_rank_sort(int n, int (*cmp)(int, int)) {
    for (int w = 1; w < n; w *= 2) {
        for (int lo = 0; lo < n; lo += 2 * w) {
            int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) W.tmp[k++] = cmp(W.ord[j], W.ord[i]) < 0 ? W.ord[j++] : W.ord[i++];
            while (i < mid) W.tmp[k++] = W.ord[i++];
            while (j < hi) W.tmp[k++] = W.ord[j++];
        }
        for (int i = 0; i < n; i++) W.ord[i] = W.tmp[i];
    }
}

static uint8_t og_rank_class(double gap) {
    if (gap < OG_RANK_TIE_FLOOR) return OG_CLASS_BEST;
    if (gap < 0.05) return OG_CLASS_EXCELLENT;
    if (gap < 0.15) return OG_CLASS_GOOD;
    if (gap < 0.35) return OG_CLASS_INACCURACY;
    if (gap < 0.7) return OG_CLASS_MISTAKE;
    return OG_CLASS_BLUNDER;
}

void og_rank(const OgRankAsk *ask, OgRankAnswer *out) {
    const int n = ask->n_rows < OG_RANK_MAX_ROWS ? ask->n_rows : OG_RANK_MAX_ROWS;
    const double keep = OG_TRUMP_KEEP_DEFAULT / 1000.0;
    W.ask = ask;
    for (int i = 0; i < n; i++) {
        const OgRankRow *r = &ask->rows[i];
        const double tax = (r->attack && ask->deck_alive) ? keep * r->n_trump : 0.0;
        W.mean[i] = r->mean;
        W.adj[i] = r->mean + tax;
        W.se[i] = og_rank_se(r->se, r->nsim);
        W.sadj[i] = r->shadow_mean + tax;
        W.sse[i] = og_rank_se(r->shadow_se, r->shadow_nsim);
        W.ord[i] = (uint16_t)i;
    }
    out->n_lines = (uint16_t)n;
    for (int i = 0; i < n; i++) {
        OgRankLine *l = &out->lines[i];
        l->row = (uint16_t)i; l->cls = OG_CLASS_NONE; l->best = false; l->pick = false;
        l->bar = 0.0f; l->se = W.se[i]; l->shadow_se = W.sse[i];
    }
    if (n == 0) return;

    if (ask->exact) {
        og_rank_sort(n, og_rank_cmp_exact);
        for (int i = 0; i < n; i++) {
            OgRankLine *l = &out->lines[i];
            l->row = W.ord[i]; l->se = W.se[l->row]; l->shadow_se = W.sse[l->row];
        }
        return;
    }

    og_rank_sort(n, og_rank_cmp_base);
    const int o = W.ord[0];
    if (!ask->rows[o].scored) {             // nothing sampled yet: input order, no verdicts
        return;
    }
    // fusion's pick
    int f = o;
    for (int i = 0; i < n; i++) {
        const int r = W.ord[i];
        if (!ask->rows[r].scored || !ask->rows[r].shadow_scored) continue;
        if (!ask->rows[f].shadow_scored || W.sadj[r] < W.sadj[f]) f = r;
    }
    int pick = o;
    if (f != o && ask->rows[o].shadow_scored && ask->rows[f].shadow_scored) {
        const double gap = W.sadj[f] - W.sadj[o];
        const double se2 = W.sse[f] * W.sse[f] + W.sse[o] * W.sse[o];
        if (gap < 0.0 && gap * gap > OG_RANK_K * OG_RANK_K * se2) pick = f;
    }
    // Best, the classes and the bars
    double hi = W.mean[o], gmin = W.mean[pick];
    for (int i = 0; i < n; i++) {
        if (!ask->rows[i].scored) continue;
        if (W.mean[i] > hi) hi = W.mean[i];
        if (og_rank_tie(i, pick) && W.mean[i] < gmin) gmin = W.mean[i];
    }
    const double lo = W.mean[o];
    const double span = hi - lo > 0.4 ? hi - lo : 0.4;
    static uint8_t cls[OG_RANK_MAX_ROWS];
    static float bar[OG_RANK_MAX_ROWS];
    static bool best[OG_RANK_MAX_ROWS];
    for (int i = 0; i < n; i++) {
        best[i] = false; cls[i] = OG_CLASS_NONE; bar[i] = 0.0f;
        W.gkey[i] = W.mean[i]; W.prio[i] = 2;
        if (!ask->rows[i].scored) continue;
        best[i] = i == pick || og_rank_tie(i, pick);
        if (best[i] || og_rank_tie(i, o)) cls[i] = OG_CLASS_BEST;
        else cls[i] = og_rank_class(W.mean[i] - (W.mean[i] >= W.mean[pick] ? W.mean[pick] : W.mean[o]));
        double w = 96.0 - (W.mean[i] - lo) / span * 84.0;
        bar[i] = (float)(w < 12.0 ? 12.0 : w > 100.0 ? 100.0 : w);
        if (best[i]) { W.gkey[i] = gmin; W.prio[i] = i == pick ? 0 : 1; }
    }
    og_rank_sort(n, og_rank_cmp_shown);
    for (int i = 0; i < n; i++) {
        const int r = W.ord[i];
        OgRankLine *l = &out->lines[i];
        l->row = (uint16_t)r; l->cls = cls[r]; l->best = best[r]; l->pick = r == pick;
        l->bar = bar[r]; l->se = W.se[r]; l->shadow_se = W.sse[r];
    }
}
#endif  // FOOLISH_ORACLE_BUILD
