/* oracle_rank_test.c - the Infinite Oracle's row order, Best, classes and the
 * numbers shown (src/oracle_rank.c, which states the rule). Pure: crafted rows
 * in, the answer checked line by line. Part of `make tests`.
 *
 * Every number the panel shows is fusion's NOISY race (the race fusion's pick
 * comes from), so the first case is the decision that showed the bug on prod
 * (step 51 of the replay in docs/INFINITE_ORACLE_DESIGN.md "Ties, Best and the
 * classes"), one row per kind: in the noise-free race fusion's pick printed the
 * same "EF 3.76" as ten twins and sat 0.76 below the lowest number; in the
 * noisy race it is the lowest number, and the panel now says so.
 *
 * Mutation-checked (2026-11-01; each seen red by the named test, then
 * restored from a copy outside the checkout, the binary removed before every
 * build so make's whole-second clock cannot keep a stale one):
 *   - the noise-free numbers shown (v = mean): "the noisy race is what is shown";
 *   - the tie rule removed (best = i == pick only): "a tied group is all Best";
 *   - the Best block not kept together (gkey = v): "the Best rows sit
 *     together, the pick first";
 *   - the class from the lowest number shown, not the Best group: "a row's
 *     class is its gap to the Best group";
 *   - the tolerance at one standard error: "two standard errors tie";
 *   - an unmeasured error counted as infinite: "one batch ties only within the
 *     floor";
 *   - a row the noisy race did not score shown anyway (shown = scored): "a row
 *     the noisy race did not score shows no number";
 *   - the exact regime sorted by depth instead of value: "the quickest win and
 *     the longest loss lead";
 *   - fusion's bar at 0: "within the bar, octogen's pick stands";
 *   - the tax left out of the noisy gap: "the trump tax counts in the gap";
 *   - a chess.com threshold shifted (0.05 -> 0.06): "classes are chess.com's
 *     expected-points bands, scaled by the seats";
 *   - the (n - 1) scaling dropped: the same test, at 6 seats;
 *   - the seats already out ignored (seats - 1 again): "classes measure the
 *     places still open";
 *   - convergence judged on the noise-free race's error and worlds:
 *     "converged is judged on the noisy race's error";
 *   - the world floor dropped from convergence: "converged needs the world
 *     floor too";
 *   - convergence over every sampled row, not the shown ones: "a row with no
 *     number shown does not hold convergence back".
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../../../shared/c/test/check.h"
#include "../src/oracle_rank.h"

static OgRankAsk ask;
static OgRankAnswer ans;

static void reset(bool exact) { memset(&ask, 0, sizeof ask); ask.exact = exact; ask.n_players = 2; }

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
    ask.n_players = 4;                                       /* the replay is a 4-seat game */
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

    TEST("rank: the noisy race is what is shown");
    CHECK(ans.noisy, "the answer says its numbers are the noisy race's");
    CHECK(fabs(line_of(f)->ef - 3.1732) < 1e-12 && fabs(line_of(f)->se - 0.004) < 1e-12,
          "the pick shows 3.1732 +-0.004 (%g +-%g)", line_of(f)->ef, line_of(f)->se);
    CHECK(fabs(line_of(low)->ef - 3.4824) < 1e-12, "octogen's pick shows its noisy 3.4824 (%g)", line_of(low)->ef);
    {
        const int want[] = { f, twin1, played, twin2, tail, low, lowtw, mid };
        for (int i = 0; i < 8; i++)
            CHECK(ans.lines[i].row == want[i], "line %d is row %d, want %d", i, ans.lines[i].row, want[i]);
    }
    CHECK(fabsf(line_of(f)->bar - 96.0f) < 1e-4f, "the pick's bar is the longest, 96 (%g)", line_of(f)->bar);

    TEST("rank: a row's class is its gap to the Best group");
    CHECK(line_of(f)->best && line_of(f)->cls == OG_CLASS_BEST, "the pick is Best");
    /* expected-points loss on 4 seats: the EF gap over 3 */
    CHECK(!line_of(twin1)->best && line_of(twin1)->cls == OG_CLASS_EXCELLENT, "3.2325 is 0.059 behind, 0.020 points: excellent (%d)", line_of(twin1)->cls);
    CHECK(line_of(played)->cls == OG_CLASS_GOOD, "3.3116 is 0.138 behind, 0.046 points: good (%d)", line_of(played)->cls);
    CHECK(line_of(twin2)->cls == OG_CLASS_INACCURACY, "3.3439 is 0.171 behind, 0.057 points: inaccuracy (%d)", line_of(twin2)->cls);
    CHECK(line_of(low)->cls == OG_CLASS_MISTAKE, "octogen's 3.4824 is 0.309 behind, 0.103 points: mistake (%d)", line_of(low)->cls);
    CHECK(line_of(mid)->cls == OG_CLASS_MISTAKE, "3.6025 is 0.429 behind, 0.143 points: mistake (%d)", line_of(mid)->cls);

    /* ---- a tied group at the top ----------------------------------------- */
    reset(false);
    const int a = row(2.000, 0.003, 2.100, 0.003);
    const int b = row(2.006, 0.003, 2.106, 0.003);
    const int c = row(2.004, 0.003, 2.104, 0.003);
    const int d = row(2.050, 0.003, 2.140, 0.003);
    og_rank(&ask, &ans);
    TEST("rank: a tied group is all Best");
    CHECK(pick_of() == a, "octogen's pick stands (%d)", pick_of());
    CHECK(line_of(a)->best && line_of(b)->best && line_of(c)->best, "every row within two errors of the pick is Best");
    CHECK(line_of(a)->cls == OG_CLASS_BEST && line_of(b)->cls == OG_CLASS_BEST && line_of(c)->cls == OG_CLASS_BEST, "and class best");
    CHECK(!line_of(d)->best && line_of(d)->cls == OG_CLASS_GOOD, "0.04 behind is not: 0.04 points on 2 seats, good (%d)", line_of(d)->cls);
    CHECK(pos_of(a) == 0 && pos_of(c) == 1 && pos_of(b) == 2 && pos_of(d) == 3, "order a c b d");

    /* ---- fusion keeps o; the noisy race's lower row is within its bar ------- */
    reset(false);
    const int o1 = row(2.00, 0.001, 2.10, 0.01);
    const int f1 = row(2.05, 0.001, 2.08, 0.01);     /* 0.02 better, bar 0.0283 */
    og_rank(&ask, &ans);
    TEST("rank: the Best rows sit together, the pick first");
    CHECK(pick_of() == o1 && line_of(f1)->best, "o is the pick and the row below it ties");
    CHECK(pos_of(o1) == 0 && pos_of(f1) == 1, "the pick leads its block although its number is higher");

    /* ---- a row below the Best group that does not tie --------------------- */
    TEST("rank: a row's class is its gap to the Best group");
    reset(false);
    ask.deck_alive = true;
    {
        const int o = row(2.00, 0.001, 2.10, 0.001);
        const int t = row(2.05, 0.001, 2.05, 0.001);       /* lower, but taxed 2.13: not fusion's */
        ask.rows[t].attack = true; ask.rows[t].n_trump = 2;
        const int r3 = row(2.10, 0.001, 2.18, 0.001);
        og_rank(&ask, &ans);
        CHECK(pick_of() == o && !line_of(t)->best, "o is the pick, t below it does not tie");
        CHECK(line_of(t)->cls == OG_CLASS_BEST, "t is class best: nothing is better (%d)", line_of(t)->cls);
        CHECK(line_of(r3)->cls == OG_CLASS_INACCURACY, "2.18 is 0.08 behind the Best group (inaccuracy), not 0.13 behind t (mistake): %d", line_of(r3)->cls);
    }

    /* ---- the block keeps a non-member out of its middle ------------------ */
    TEST("rank: the Best rows sit together, the pick first");
    reset(false);
    const int p0 = row(3.0, 0.01, 3.000, 0.010);
    const int x  = row(3.1, 0.01, 3.025, 0.020);    /* tied: 2 * sqrt(.01^2 + .02^2) = .0447 */
    const int y  = row(3.1, 0.01, 3.021, 0.000);    /* not: 2 * .01 = .02 < .021 */
    og_rank(&ask, &ans);
    CHECK(line_of(x)->best && !line_of(y)->best, "x ties the pick, y does not");
    CHECK(pos_of(p0) == 0 && pos_of(x) == 1 && pos_of(y) == 2, "order p x y, not p y x (%d %d %d)", pos_of(p0), pos_of(x), pos_of(y));

    /* ---- the tolerance --------------------------------------------------- */
    TEST("rank: two standard errors tie");
    reset(false);
    row(3.0, 0.001, 3.0000, 0.0027);
    const int in = row(3.1, 0.001, 3.0070, 0.0027);     /* 2 * sqrt(2) * .0027 = .00764 */
    const int out = row(3.1, 0.001, 3.0080, 0.0027);
    og_rank(&ask, &ans);
    CHECK(line_of(in)->best, "0.0070 apart at se 0.0027 ties");
    CHECK(!line_of(out)->best && line_of(out)->cls == OG_CLASS_EXCELLENT, "0.0080 does not");

    TEST("rank: one batch ties only within the floor");
    reset(false);
    row(3.0, INFINITY, 3.0, INFINITY);
    const int near = row(3.0, INFINITY, 3.00005, INFINITY);
    const int far = row(3.0, INFINITY, 3.001, INFINITY);
    og_rank(&ask, &ans);
    CHECK(line_of(near)->best, "0.00005 apart is a tie before any error is measured");
    CHECK(!line_of(far)->best, "0.001 apart is not");

    TEST("rank: Mode B's error is the n-based proxy");
    reset(false);
    {
        const int r = row(3.0, -1.0, 3.0, -1.0);
        ask.rows[r].shadow_nsim = 57600;
        og_rank(&ask, &ans);
        CHECK(fabs(line_of(r)->se - 0.005) < 1e-12, "1.2 / sqrt(57600) = 0.005 (%g)", line_of(r)->se);
    }

    /* ---- what the noisy race did not score ------------------------------- */
    TEST("rank: a row the noisy race did not score shows no number");
    reset(false);
    {
        const int s0 = row(3.0, 0.01, 3.2, 0.01);
        const int bare = row(2.5, 0.01, 0.0, 0.0);
        ask.rows[bare].shadow_scored = false; ask.rows[bare].shadow_nsim = 0;
        const int s1 = row(3.1, 0.01, 3.3, 0.01);
        og_rank(&ask, &ans);
        CHECK(ans.noisy, "the noisy race ran");
        CHECK(!line_of(bare)->shown && line_of(bare)->cls == OG_CLASS_NONE && !line_of(bare)->best
              && line_of(bare)->bar == 0.0f, "no number, class, Best or bar for it");
        CHECK(pos_of(s0) == 0 && pos_of(s1) == 1 && pos_of(bare) == 2, "it sorts after every shown row (%d %d %d)",
              pos_of(s0), pos_of(s1), pos_of(bare));
    }

    TEST("rank: with no noisy race the noise-free race is shown");
    reset(false);
    {
        const int q0 = row(3.0, 0.01, 0.0, 0.0), q1 = row(3.2, 0.01, 0.0, 0.0);
        ask.rows[q0].shadow_scored = ask.rows[q1].shadow_scored = false;
        og_rank(&ask, &ans);
        CHECK(!ans.noisy, "the answer says so");
        CHECK(line_of(q0)->shown && fabs(line_of(q0)->ef - 3.0) < 1e-12 && line_of(q0)->pick && line_of(q0)->best,
              "octogen's pick shows its own number and is Best");
        CHECK(line_of(q1)->cls == OG_CLASS_BLUNDER, "0.2 behind on 2 seats is 0.2 points: blunder (%d)", line_of(q1)->cls);
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

    TEST("rank: equal numbers sort by the taxed number, then input order");
    reset(false);
    ask.deck_alive = true;
    {
        const int t = row(2.5, 0.0, 2.5, 0.0);
        ask.rows[t].attack = true; ask.rows[t].n_trump = 1;
        const int u = row(2.5, 0.0, 2.5, 0.0);
        const int v = row(2.5, 0.0, 2.5, 0.0);
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
        CHECK(!line_of(un)->best && !line_of(un)->shown && line_of(un)->cls == OG_CLASS_NONE && line_of(un)->bar == 0.0f,
              "no verdict on it");
    }

    /* ---- chess.com's thresholds, on expected points ----------------------- */
    TEST("rank: classes are chess.com's expected-points bands, scaled by the seats");
    {
        static const double T[] = { 0.02, 0.05, 0.10, 0.20 };
        static const uint8_t below[] = { OG_CLASS_EXCELLENT, OG_CLASS_GOOD, OG_CLASS_INACCURACY, OG_CLASS_MISTAKE };
        static const uint8_t at[] = { OG_CLASS_GOOD, OG_CLASS_INACCURACY, OG_CLASS_MISTAKE, OG_CLASS_BLUNDER };
        static const int seats[] = { 2, 6 };
        for (int s = 0; s < 2; s++) {
            reset(false);
            ask.n_players = (uint8_t)seats[s];
            const double span = seats[s] - 1;
            row(3.0, 0.0, 2.0, 0.0);                          /* the pick */
            int lo[4], hi[4];
            for (int k = 0; k < 4; k++) {
                lo[k] = row(3.0, 0.0, 2.0 + T[k] * span - 1e-7, 0.0);
                hi[k] = row(3.0, 0.0, 2.0 + T[k] * span + 1e-7, 0.0);
            }
            og_rank(&ask, &ans);
            for (int k = 0; k < 4; k++) {
                CHECK(line_of(lo[k])->cls == below[k], "%dp, just under %.2f points: class %d, want %d",
                      seats[s], T[k], line_of(lo[k])->cls, below[k]);
                CHECK(line_of(hi[k])->cls == at[k], "%dp, just over %.2f points: class %d, want %d",
                      seats[s], T[k], line_of(hi[k])->cls, at[k]);
            }
        }
    }

    TEST("rank: classes measure the places still open");
    {
        static const double T[] = { 0.02, 0.05, 0.10, 0.20 };
        static const uint8_t below[] = { OG_CLASS_EXCELLENT, OG_CLASS_GOOD, OG_CLASS_INACCURACY, OG_CLASS_MISTAKE };
        static const uint8_t at[] = { OG_CLASS_GOOD, OG_CLASS_INACCURACY, OG_CLASS_MISTAKE, OG_CLASS_BLUNDER };
        /* 6 seats, 2 out: places 3..6, a span of 3; 6 seats, 4 out: the last two, a span of 1 */
        static const int out_[] = { 2, 4 }, span_[] = { 3, 1 };
        for (int s = 0; s < 2; s++) {
            reset(false);
            ask.n_players = 6; ask.n_out = (uint8_t)out_[s];
            row(3.0, 0.0, 5.0, 0.0);                          /* the pick */
            int lo[4], hi[4];
            for (int k = 0; k < 4; k++) {
                lo[k] = row(3.0, 0.0, 5.0 + T[k] * span_[s] - 1e-7, 0.0);
                hi[k] = row(3.0, 0.0, 5.0 + T[k] * span_[s] + 1e-7, 0.0);
            }
            og_rank(&ask, &ans);
            for (int k = 0; k < 4; k++) {
                CHECK(line_of(lo[k])->cls == below[k], "6p, %d out, just under %.2f points: class %d, want %d",
                      out_[s], T[k], line_of(lo[k])->cls, below[k]);
                CHECK(line_of(hi[k])->cls == at[k], "6p, %d out, just over %.2f points: class %d, want %d",
                      out_[s], T[k], line_of(hi[k])->cls, at[k]);
            }
        }
        /* throwing a 6-seat game's two-player endgame: 0.9 places is 0.9 points (blunder), not 0.18 (mistake) */
        reset(false);
        ask.n_players = 6; ask.n_out = 4;
        row(3.0, 0.0, 5.05, 0.0);
        const int thrown = row(3.0, 0.0, 5.95, 0.0);
        og_rank(&ask, &ans);
        CHECK(line_of(thrown)->cls == OG_CLASS_BLUNDER, "0.9 places with two left is a blunder (%d)", line_of(thrown)->cls);
        /* one seat in (no real decision) counts as one place */
        reset(false);
        ask.n_players = 6; ask.n_out = 5;
        row(3.0, 0.0, 5.0, 0.0);
        const int one = row(3.0, 0.0, 5.03, 0.0);
        og_rank(&ask, &ans);
        CHECK(line_of(one)->cls == OG_CLASS_GOOD, "fewer than two in counts as one place: 0.03 is good (%d)", line_of(one)->cls);
    }

    /* ---- convergence: on the numbers shown -------------------------------- */
    TEST("rank: converged is judged on the noisy race's error");
    {
        reset(false);
        const int r0 = row(3.0, 0.020, 3.10, 0.0049), r1 = row(3.1, 0.020, 3.20, 0.0050);
        ask.rows[r0].nsim = ask.rows[r1].nsim = 32768;                 /* noise-free: half the worlds, wide */
        ask.rows[r0].shadow_nsim = ask.rows[r1].shadow_nsim = 65536;
        og_rank(&ask, &ans);
        CHECK(ans.converged, "every shown error <= 0.005 over 65,536 noisy worlds: converged");
        for (int i = 0; i < ans.n_lines; i++)
            CHECK(ans.lines[i].se <= OG_RANK_CONVERGE_MAX_SE, "the +- printed at convergence is at most the checkpoint (%g)", ans.lines[i].se);
        ask.rows[r1].shadow_se = 0.0051;
        ask.rows[r0].se = ask.rows[r1].se = 0.001;
        ask.rows[r0].nsim = ask.rows[r1].nsim = 200000;
        og_rank(&ask, &ans);
        CHECK(!ans.converged, "a noisy error of 0.0051 is not converged, however tight the noise-free race");
    }
    TEST("rank: converged needs the world floor too");
    {
        reset(false);
        const int r0 = row(3.0, 0.001, 3.10, 0.004);
        ask.rows[r0].shadow_nsim = 65535;
        og_rank(&ask, &ans);
        CHECK(!ans.converged, "65,535 noisy worlds is under the floor");
        ask.rows[r0].shadow_nsim = 65536;
        og_rank(&ask, &ans);
        CHECK(ans.converged, "65,536 is at it");
        ask.rows[r0].shadow_se = INFINITY;
        og_rank(&ask, &ans);
        CHECK(!ans.converged, "an unmeasured error never converges");
    }
    TEST("rank: a row with no number shown does not hold convergence back");
    {
        reset(false);
        const int r0 = row(3.0, 0.001, 3.10, 0.004);
        ask.rows[r0].shadow_nsim = 70000;
        const int bare = row(2.5, 0.001, 0.0, 0.0);
        ask.rows[bare].shadow_scored = false; ask.rows[bare].shadow_nsim = 0;
        og_rank(&ask, &ans);
        CHECK(ans.converged, "only shown rows count");
        reset(false);
        og_rank(&ask, &ans);
        CHECK(!ans.converged, "an empty answer is not converged");
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
