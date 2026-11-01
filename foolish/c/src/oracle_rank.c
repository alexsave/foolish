// Infinite Oracle - the row order, Best, the classes and the numbers shown
// (oracle_rank.h, docs/INFINITE_ORACLE_DESIGN.md "Ties, Best and the classes").
//
// THE NUMBER SHOWN is fusion's noisy race: each row's EF, its +-, its bar, its
// class, the ties and the order are all the noisy race's mean and standard
// error, the race fusion's pick comes from, so Best, the classes and the
// numbers can never disagree. The noise-free race is not shown; it only names
// octogen's pick o for fusion's rule.
//
//   rows the noisy race did not score: when it scored no row at all (a decision
//         with no noisy race: one candidate, a proven win), there is nothing
//         to disagree with, and the noise-free race is shown instead
//         (OgRankAnswer.noisy = false). Otherwise a row it did not score shows
//         no number: no EF, no bar, no class and never Best (`shown` false),
//         and it sorts after every row that has one, by the noise-free mean.
//   o     octogen's pick: the first row by noise-free mean, then that mean plus
//         the trump-keep tax, then input order.
//   pick  fusion's pick: o, unless the noisy race's lowest row f (its mean plus
//         tax; the first such row in o's order on a tie) beats o's by more than
//         FUSION_SHADOW tenths of their combined standard error. Fusion's own
//         rule (octogen_strategy.c og_fusion_shadow) over everything sampled.
//   tie   two shown rows tie when their numbers differ by no more than
//         max(OG_RANK_TIE_FLOOR, k * sqrt(se_a^2 + se_b^2)), k = FUSION_SHADOW/10:
//         the same two standard errors fusion holds its own pick to. An error
//         not measured yet (one batch, se infinite) counts as 0 here, so before
//         the run has an error only rows within the floor tie. When fusion
//         keeps o, the noisy race's lowest row is within fusion's bar of it, so
//         (up to the tax, which the bar counts and the tie does not) it ties:
//         the Best group sits at the top of the list.
//   Best  every shown row that ties with the pick; `pick` marks the one fusion
//         plays. A pick with no number shown (never seen in practice: o is
//         pinned in the noisy race) is marked but makes no Best group.
//   class a row tied with the pick is best. Any other shown row is classed by
//         its EXPECTED-POINTS LOSS, as chess.com classifies moves. With k of n
//         seats already out the mover can only finish k+1..n, so the expected
//         finish EF maps to expected points E = (n - EF) / (n - k - 1) (the
//         best place still open = 1.0, last = 0.0), and the loss against the
//         Best group's lowest number is (EF - that) / (n - k - 1): throwing a
//         two-player endgame of a 6-seat game (5th -> 6th) loses 1.0, not 0.2.
//         Fewer than two seats in counts as one place. The classes are
//         chess.com's table (og_rank_class). A row below the Best group that
//         does not tie (the tax can produce one) loses nothing: best too.
//   order shown rows by number, then the number plus tax, then input order;
//         the Best rows one block where their lowest number puts it, the pick
//         first. Then the unshown rows by noise-free mean, then the unsampled.
//         The played move is never moved out of its place.
//   bar   the X-ray pages' formula over the shown numbers: 96 - 84 * (v -
//         lowest) / span, span their spread floored at 0.4, clamped [12, 100].
//   converged  every shown row has OG_RANK_CONVERGE_MIN_N worlds behind its
//         number and an error of at most OG_RANK_CONVERGE_MAX_SE: the noisy
//         race's, since that is what is shown. An unmeasured (infinite) error
//         never passes.
//
// Exact regime (a proven win or loss): by verdict, win, draw, unknown, none,
// loss, illegal; within a verdict by its value, larger first (+-(1000 - depth):
// the quickest win and the longest loss lead); then the noise-free mean; then
// input order. No Best and no classes: the verdicts are the answer.
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
    // what is shown: the noisy race's number, its taxed twin and its error
    bool shown[OG_RANK_MAX_ROWS];
    double v[OG_RANK_MAX_ROWS], vadj[OG_RANK_MAX_ROWS], vse[OG_RANK_MAX_ROWS];
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
    const double d = W.v[a] - W.v[b];
    const double sa = og_rank_tie_se(W.vse[a]), sb = og_rank_tie_se(W.vse[b]);
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

// The display order: shown rows (the Best block keyed at its lowest number,
// pick first), then sampled rows with no number shown, then the unsampled.
static int og_rank_tier(int a) { return W.shown[a] ? 0 : W.ask->rows[a].scored ? 1 : 2; }
static int og_rank_cmp_shown(int a, int b) {
    int c = og_rank_tier(a) - og_rank_tier(b);
    if (c) return c;
    if (og_rank_tier(a) == 1) return og_rank_cmp_base(a, b);
    if (og_rank_tier(a) == 0) {
        c = og_rank_cmp_d(W.gkey[a], W.gkey[b]);
        if (!c) c = (int)W.prio[a] - (int)W.prio[b];
        if (!c) c = og_rank_cmp_d(W.v[a], W.v[b]);
        if (!c) c = og_rank_cmp_d(W.vadj[a], W.vadj[b]);
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

// A row's class from its expected-points loss, chess.com's thresholds
// (support.chess.com/en/articles/8572705, "How are moves classified?"): Best
// 0.00, Excellent 0.00-0.02, Good 0.02-0.05, Inaccuracy 0.05-0.10, Mistake
// 0.10-0.20, Blunder 0.20-1.00. Each band includes its lower bound. The one
// table; the host only names the classes.
static const struct { double below; uint8_t cls; } OG_RANK_CLASSES[] = {
    { 0.02, OG_CLASS_EXCELLENT },
    { 0.05, OG_CLASS_GOOD },
    { 0.10, OG_CLASS_INACCURACY },
    { 0.20, OG_CLASS_MISTAKE },
};
static uint8_t og_rank_class(double gap, int n_players, int n_out) {
    if (gap < OG_RANK_TIE_FLOOR) return OG_CLASS_BEST;
    const int places = n_players - n_out - 1;           // the places still open, less one
    const double loss = gap / (double)(places > 1 ? places : 1);
    for (int k = 0; k < (int)(sizeof OG_RANK_CLASSES / sizeof *OG_RANK_CLASSES); k++)
        if (loss < OG_RANK_CLASSES[k].below) return OG_RANK_CLASSES[k].cls;
    return OG_CLASS_BLUNDER;
}

// One answer line for row r, with what is shown of it.
static void og_rank_line(OgRankLine *l, int r) {
    l->row = (uint16_t)r; l->shown = W.shown[r];
    l->ef = W.shown[r] ? W.v[r] : 0.0; l->se = W.shown[r] ? W.vse[r] : 0.0;
    l->shadow_se = W.sse[r];
    l->cls = OG_CLASS_NONE; l->best = false; l->pick = false; l->bar = 0.0f;
}

void og_rank(const OgRankAsk *ask, OgRankAnswer *out) {
    const int n = ask->n_rows < OG_RANK_MAX_ROWS ? ask->n_rows : OG_RANK_MAX_ROWS;
    const double keep = OG_TRUMP_KEEP_DEFAULT / 1000.0;
    W.ask = ask;
    bool noisy = false;
    for (int i = 0; i < n; i++) noisy |= ask->rows[i].scored && ask->rows[i].shadow_scored;
    for (int i = 0; i < n; i++) {
        const OgRankRow *r = &ask->rows[i];
        const double tax = (r->attack && ask->deck_alive) ? keep * r->n_trump : 0.0;
        W.mean[i] = r->mean;
        W.adj[i] = r->mean + tax;
        W.se[i] = og_rank_se(r->se, r->nsim);
        W.sadj[i] = r->shadow_mean + tax;
        W.sse[i] = og_rank_se(r->shadow_se, r->shadow_nsim);
        W.shown[i] = r->scored && (!noisy || r->shadow_scored);
        W.v[i] = noisy ? r->shadow_mean : r->mean;
        W.vadj[i] = noisy ? W.sadj[i] : W.adj[i];
        W.vse[i] = noisy ? W.sse[i] : W.se[i];
        W.ord[i] = (uint16_t)i;
    }
    out->n_lines = (uint16_t)n;
    out->noisy = noisy;
    // Converged: judged on what is shown, so the +- the panel prints at
    // "Converged" is at most the checkpoint. Not in the exact regime, whose
    // answer is the verdicts (the host stops on those itself).
    bool conv = false;
    for (int i = 0; i < n; i++) {
        if (!W.shown[i]) continue;
        const int32_t wn = noisy ? ask->rows[i].shadow_nsim : ask->rows[i].nsim;
        if (wn < OG_RANK_CONVERGE_MIN_N || !(W.vse[i] <= OG_RANK_CONVERGE_MAX_SE)) { conv = false; break; }
        conv = true;
    }
    out->converged = conv;
    for (int i = 0; i < n; i++) og_rank_line(&out->lines[i], i);
    if (n == 0) return;

    if (ask->exact) {
        og_rank_sort(n, og_rank_cmp_exact);
        for (int i = 0; i < n; i++) og_rank_line(&out->lines[i], W.ord[i]);
        return;
    }

    og_rank_sort(n, og_rank_cmp_base);
    const int o = W.ord[0];
    if (!ask->rows[o].scored) return;       // nothing sampled yet: input order, no verdicts
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
    // Best, the classes and the bars, all on the shown numbers
    static uint8_t cls[OG_RANK_MAX_ROWS];
    static float bar[OG_RANK_MAX_ROWS];
    static bool best[OG_RANK_MAX_ROWS];
    bool any = false;
    double lo = 0.0, hi = 0.0, gmin = W.v[pick];
    for (int i = 0; i < n; i++) {
        best[i] = W.shown[i] && W.shown[pick] && (i == pick || og_rank_tie(i, pick));
        if (!W.shown[i]) continue;
        if (!any || W.v[i] < lo) lo = W.v[i];
        if (!any || W.v[i] > hi) hi = W.v[i];
        any = true;
        if (best[i] && W.v[i] < gmin) gmin = W.v[i];
    }
    const double span = hi - lo > 0.4 ? hi - lo : 0.4;
    for (int i = 0; i < n; i++) {
        cls[i] = OG_CLASS_NONE; bar[i] = 0.0f;
        W.gkey[i] = W.v[i]; W.prio[i] = 2;
        if (!W.shown[i]) continue;
        cls[i] = best[i] ? OG_CLASS_BEST : og_rank_class(W.shown[pick] ? W.v[i] - gmin : W.v[i] - lo, ask->n_players, ask->n_out);
        double w = 96.0 - (W.v[i] - lo) / span * 84.0;
        bar[i] = (float)(w < 12.0 ? 12.0 : w > 100.0 ? 100.0 : w);
        if (best[i]) { W.gkey[i] = gmin; W.prio[i] = i == pick ? 0 : 1; }
    }
    og_rank_sort(n, og_rank_cmp_shown);
    for (int i = 0; i < n; i++) {
        const int r = W.ord[i];
        OgRankLine *l = &out->lines[i];
        og_rank_line(l, r);
        l->cls = cls[r]; l->best = best[r]; l->pick = r == pick; l->bar = bar[r];
    }
}
#endif  // FOOLISH_ORACLE_BUILD
