/* oracle_rank_test.c - the Infinite Oracle's row order, Best and classes
 * (src/oracle_rank.c, which states the rule). Pure: crafted rows in, the
 * answer checked line by line. Part of `make tests`.
 *
 * The first case is the decision that showed the bug on prod (step 51 of the
 * replay in docs/INFINITE_ORACLE_DESIGN.md "Ties, Best and the classes"),
 * trimmed to one row per kind: fusion overrules octogen's 2.9975 for a 3.7569
 * whose twins print the same "EF 3.76 +-0.00", and before the rule moved here
 * one of the twins read BEST and the rest BLUNDER.
 *
 * Mutation-checked (2026-11-01; each seen red by the named test, then
 * restored from a copy outside the checkout):
 *   - the tie rule removed (best = i == pick only): "a tied group is all Best"
 *     and "the pick's twins read Best" go red;
 *   - the Best block not kept together (gkey = mean for every row): "the Best
 *     rows sit together, the pick first" goes red;
 *   - the class measured from the lowest row for every row (the old yardstick):
 *     "a row's class is its gap to the Best group" goes red;
 *   - the tolerance at one standard error instead of FUSION_SHADOW/10 = two:
 *     "two standard errors tie" goes red;
 *   - an unmeasured error counted as infinite: "one batch ties only within the
 *     floor" goes red;
 *   - the exact regime sorted by depth instead of value: "the quickest win and
 *     the longest loss lead" goes red;
 *   - fusion's bar at 0: "within the bar, octogen's pick stands" goes red;
 *   - the tax left out of the noisy gap: "the trump tax counts in the gap" red.
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../../../shared/c/test/check.h"
#include "../src/oracle_rank.h"

static OgRankAsk ask;
static OgRankAnswer ans;

static void reset(bool exact) { memset(&ask, 0, sizeof ask); ask.exact = exact; }

/* A sampled row: mean and error in the noise-free race, mean and error in the
 * noisy one. Returns its index. */
static int row(double mean, double se, double smean, double sse) {
    OgRankRow *r = &ask.rows[ask.n_rows];
    r->scored = true; r->nsim = 58000; r->mean = mean; r->se = se;
    r->shadow_scored = true; r->shadow_nsim = 29000; r->shadow_mean = smean; r->shadow_se = sse;
    r->verdict = OG_VERDICT_NONE;
    return ask.n_rows++;
}

static const OgRankLine *line_of(int r) {
    for (int i = 0; i < ans.n_lines; i++) if (ans.lines[i].row == r) return &ans.lines[i];
    return NULL;
}
static int pos_of(int r) {
    for (int i = 0; i < ans.n_lines; i++) if (ans.lines[i].row == r) return i;
    return -1;
}
static int pick_of(void) {
    int p = -1, n = 0;
    for (int i = 0; i < ans.n_lines; i++) if (ans.lines[i].pick) { p = ans.lines[i].row; n++; }
    return n == 1 ? p : -1 - n;
}

int main(void)
{
    /* ---- the prod decision ---------------------------------------------- */
    reset(false);
    const int played = row(3.8583, 0.0016, 3.3116, 0.004);
    const int low    = row(2.9975, 0.0025, 3.4824, 0.004);   /* octogen's pick */
    const int f      = row(3.7569, 0.0027, 3.1732, 0.004);   /* fusion's */
    const int twin1  = row(3.7559, 0.0027, 3.2325, 0.004);
    const int twin2  = row(3.7602, 0.0027, 3.3439, 0.004);
    const int mid    = row(3.3179, 0.0021, 3.6025, 0.004);
    const int lowtw  = row(3.0002, 0.0024, 3.4872, 0.004);
    const int tail   = row(3.9649, 0.0009, 3.4240, 0.004);
    og_rank(&ask, &ans);

    TEST("rank: fusion overrules octogen beyond its bar");
    CHECK(pick_of() == f, "pick %d, want %d", pick_of(), f);

    TEST("rank: a tied group is all Best");
    CHECK(line_of(f)->best && line_of(twin1)->best && line_of(twin2)->best, "the 3.76 twins all read Best");
    CHECK(line_of(f)->cls == OG_CLASS_BEST && line_of(twin1)->cls == OG_CLASS_BEST && line_of(twin2)->cls == OG_CLASS_BEST,
          "and all class best (%d %d %d)", line_of(f)->cls, line_of(twin1)->cls, line_of(twin2)->cls);
    CHECK(line_of(twin1)->bar == line_of(f)->bar || fabsf(line_of(twin1)->bar - line_of(f)->bar) < 0.2f, "bars alike");
    CHECK(!line_of(low)->best && !line_of(played)->best && !line_of(tail)->best, "nothing outside the group is Best");

    TEST("rank: the Best rows sit together, the pick first");
    {
        const int want[] = { low, lowtw, mid, f, twin1, twin2, played, tail };
        for (int i = 0; i < 8; i++)
            CHECK(ans.lines[i].row == want[i], "line %d is row %d, want %d", i, ans.lines[i].row, want[i]);
    }

    TEST("rank: a row's class is its gap to the Best group");
    CHECK(line_of(played)->cls == OG_CLASS_GOOD, "3.8583 is 0.10 behind fusion's 3.7569: good, not blunder (%d)", line_of(played)->cls);
    CHECK(line_of(tail)->cls == OG_CLASS_INACCURACY, "3.9649 is 0.21 behind: inaccuracy (%d)", line_of(tail)->cls);

    TEST("rank: the lowest number keeps its own yardstick");
    CHECK(line_of(low)->cls == OG_CLASS_BEST, "the lowest row still reads best (%d)", line_of(low)->cls);
    CHECK(line_of(lowtw)->cls == OG_CLASS_BEST, "and so does its twin (%d)", line_of(lowtw)->cls);
    CHECK(line_of(mid)->cls == OG_CLASS_INACCURACY, "3.3179 is 0.32 behind the lowest: inaccuracy (%d)", line_of(mid)->cls);
    CHECK(fabsf(line_of(low)->bar - 96.0f) < 1e-4f, "the lowest row's bar is 96 (%g)", line_of(low)->bar);

    /* ---- fusion agrees with octogen: one group at the top ---------------- */
    reset(false);
    const int a = row(2.000, 0.003, 2.10, 0.004);
    const int b = row(2.006, 0.003, 2.11, 0.004);
    const int c = row(2.004, 0.003, 2.12, 0.004);
    const int d = row(2.050, 0.003, 2.15, 0.004);
    og_rank(&ask, &ans);
    TEST("rank: the pick's twins read Best");
    CHECK(pick_of() == a, "octogen's pick stands (%d)", pick_of());
    CHECK(line_of(a)->best && line_of(b)->best && line_of(c)->best, "every twin of the top row is Best");
    CHECK(!line_of(d)->best && line_of(d)->cls == OG_CLASS_EXCELLENT, "0.05 behind is not (%d)", line_of(d)->cls);
    CHECK(pos_of(a) == 0 && pos_of(c) == 1 && pos_of(b) == 2 && pos_of(d) == 3, "order a c b d");

    /* ---- the block keeps a non-member out of its middle ------------------ */
    reset(false);
    const int p0 = row(3.000, 0.010, 3.0, 0.004);
    const int x  = row(3.025, 0.020, 3.1, 0.004);    /* tied: 2 * sqrt(.01^2 + .02^2) = .0447 */
    const int y  = row(3.021, 0.000, 3.1, 0.004);    /* not: 2 * .01 = .02 < .021 */
    og_rank(&ask, &ans);
    TEST("rank: the Best rows sit together, the pick first");
    CHECK(line_of(x)->best && !line_of(y)->best, "x ties the pick, y does not");
    CHECK(pos_of(p0) == 0 && pos_of(x) == 1 && pos_of(y) == 2, "order p x y, not p y x (%d %d %d)", pos_of(p0), pos_of(x), pos_of(y));

    /* ---- the tolerance --------------------------------------------------- */
    TEST("rank: two standard errors tie");
    reset(false);
    row(3.0000, 0.0027, 3.0, 0.004);
    const int in = row(3.0070, 0.0027, 3.1, 0.004);    /* 2 * sqrt(2) * .0027 = .00764 */
    const int out = row(3.0080, 0.0027, 3.1, 0.004);
    og_rank(&ask, &ans);
    CHECK(line_of(in)->best, "0.0070 apart at se 0.0027 ties");
    CHECK(!line_of(out)->best && line_of(out)->cls == OG_CLASS_EXCELLENT, "0.0080 does not");

    TEST("rank: one batch ties only within the floor");
    reset(false);
    row(3.0, INFINITY, 3.0, INFINITY);
    const int near = row(3.00005, INFINITY, 3.0, INFINITY);
    const int far = row(3.001, INFINITY, 3.0, INFINITY);
    og_rank(&ask, &ans);
    CHECK(line_of(near)->best, "0.00005 apart is a tie before any error is measured");
    CHECK(!line_of(far)->best, "0.001 apart is not");

    TEST("rank: Mode B's error is the n-based proxy");
    reset(false);
    {
        const int r = row(3.0, -1.0, 3.0, -1.0);
        ask.rows[r].nsim = 57600;
        og_rank(&ask, &ans);
        CHECK(fabs(line_of(r)->se - 0.005) < 1e-12, "1.2 / sqrt(57600) = 0.005 (%g)", line_of(r)->se);
    }

    /* ---- fusion's bar ---------------------------------------------------- */
    TEST("rank: within the bar, octogen's pick stands");
    reset(false);
    {
        const int o = row(2.00, 0.001, 2.10, 0.01);
        row(2.05, 0.001, 2.08, 0.01);                /* 0.02 better, bar 2 * .01414 = .0283 */
        og_rank(&ask, &ans);
        CHECK(pick_of() == o, "pick %d", pick_of());
        reset(false);
        row(2.00, 0.001, 2.10, 0.01);
        const int g = row(2.05, 0.001, 2.06, 0.01);  /* 0.04 better: overrules */
        og_rank(&ask, &ans);
        CHECK(pick_of() == g, "pick %d, want %d", pick_of(), g);
        reset(false);
        const int o2 = row(2.00, 0.001, 2.10, INFINITY);
        row(2.05, 0.001, 1.50, INFINITY);
        og_rank(&ask, &ans);
        CHECK(pick_of() == o2, "one batch is not evidence (%d)", pick_of());
    }

    TEST("rank: the trump tax counts in the gap");
    reset(false);
    ask.deck_alive = true;
    {
        const int o = row(2.00, 0.001, 2.10, 0.01);
        const int t = row(2.05, 0.001, 2.05, 0.01);  /* 0.05 better untaxed, beyond the bar */
        ask.rows[t].attack = true; ask.rows[t].n_trump = 1;   /* +0.04: 0.01 better, inside */
        og_rank(&ask, &ans);
        CHECK(pick_of() == o, "pick %d", pick_of());
    }

    TEST("rank: equal means sort by the taxed mean, then input order");
    reset(false);
    ask.deck_alive = true;
    {
        const int t = row(2.5, 0.0, 2.5, 0.01);
        ask.rows[t].attack = true; ask.rows[t].n_trump = 1;
        const int u = row(2.5, 0.0, 2.5, 0.01);
        const int v = row(2.5, 0.0, 2.5, 0.01);
        og_rank(&ask, &ans);
        CHECK(pos_of(u) == 0 && pos_of(v) == 1 && pos_of(t) == 2, "u v t (%d %d %d)", pos_of(u), pos_of(v), pos_of(t));
        CHECK(line_of(t)->best && line_of(u)->pick, "all three tie, u is the pick");
    }

    /* ---- unscored rows ---------------------------------------------------- */
    TEST("rank: an unsampled row sorts last and is never Best");
    reset(false);
    {
        const int un = ask.n_rows++;            /* scored = false */
        const int s = row(3.0, 0.01, 3.0, 0.01);
        og_rank(&ask, &ans);
        CHECK(pos_of(s) == 0 && pos_of(un) == 1, "sampled first");
        CHECK(!line_of(un)->best && line_of(un)->cls == OG_CLASS_NONE && line_of(un)->bar == 0.0f, "no verdict on it");
    }

    /* ---- the exact regime ------------------------------------------------- */
    TEST("rank: the quickest win and the longest loss lead");
    reset(true);
    {
        const int none = row(3.0, 0.01, 3.0, 0.01);
        const int l2 = row(3.0, 0.01, 3.0, 0.01), l10 = row(3.0, 0.01, 3.0, 0.01);
        const int w10 = row(3.0, 0.01, 3.0, 0.01), w2 = row(3.0, 0.01, 3.0, 0.01);
        const int dr = row(3.0, 0.01, 3.0, 0.01);
        struct { int r; uint8_t v; int32_t val; } V[] = {
            { l2, OG_VERDICT_LOSS, -998 }, { l10, OG_VERDICT_LOSS, -990 },
            { w10, OG_VERDICT_WIN, 990 }, { w2, OG_VERDICT_WIN, 998 }, { dr, OG_VERDICT_DRAW, 0 },
        };
        for (int i = 0; i < 5; i++) {
            ask.rows[V[i].r].verdict = V[i].v; ask.rows[V[i].r].has_verdict_val = true; ask.rows[V[i].r].verdict_val = V[i].val;
        }
        og_rank(&ask, &ans);
        const int want[] = { w2, w10, dr, none, l10, l2 };
        for (int i = 0; i < 6; i++)
            CHECK(ans.lines[i].row == want[i], "line %d is row %d, want %d", i, ans.lines[i].row, want[i]);
        for (int i = 0; i < 6; i++)
            CHECK(!ans.lines[i].best && !ans.lines[i].pick && ans.lines[i].cls == OG_CLASS_NONE, "no Best or classes");
    }

    return report("oracle_rank");
}
