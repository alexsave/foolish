/* The throw, baked (DECISIONS K14): what cn_roll.c promises, measured over
 * seeds.
 *
 *   - the same seed bakes the same bytes, and a pinned golden holds the
 *     recipe to the format
 *   - the cup roll: every throw completes; no die corner ever crosses the
 *     cup's mouth before the slam (there is no barrier, only the throw);
 *     every die ends flat on the table inside the mouth, none on another,
 *     and the last resort is a rarity
 *   - the hand is not set by the throw: every face comes up as often (the
 *     slow turn sets a die down on the face it rode on; the shake is what
 *     randomises, and the test reports how often the pre-flip face recurs)
 *   - the table roll: the dice fall out of the tipped cup, sit a second,
 *     take the five stations, and the cup comes home
 *   - a cap too small reports an incomplete bake; a bad throw bakes nothing
 *
 * The first argument scales the seed count (default 300; `make asan` passes
 * 60). */
#include "cn_check.h"
#include "../src/cn_roll.h"
#include <math.h>
#include <time.h>

static float   F[CN_ROLL_MAX_FRAMES * CN_ROLL_FRAME_FLOATS], F2[CN_ROLL_MAX_FRAMES * CN_ROLL_FRAME_FLOATS];
static uint8_t P[CN_ROLL_MAX_FRAMES];

/* the study's table: the expanded board's my-seat cup and 24-point dice */
static const float CUP_X = 179, CUP_Y = 540, CUP_R = 59, DIE = 24, RING = 34.7f;

/* a frame's pose: position and rotation columns, from the quaternion */
static void poseOf(const float *f, double p[3], double c[3][3])
{
    double x = f[3], y = f[4], z = f[5], w = f[6];
    p[0] = f[0]; p[1] = f[1]; p[2] = f[2];
    c[0][0] = 1 - 2 * (y * y + z * z); c[0][1] = 2 * (x * y + z * w);     c[0][2] = 2 * (x * z - y * w);
    c[1][0] = 2 * (x * y - z * w);     c[1][1] = 1 - 2 * (x * x + z * z); c[1][2] = 2 * (y * z + x * w);
    c[2][0] = 2 * (x * z + y * w);     c[2][1] = 2 * (y * z - x * w);     c[2][2] = 1 - 2 * (x * x + y * y);
}

/* the least local z (toward the mouth at 0) of any die corner in the cup's frame, in one frame */
static double mouthMargin(const float *frame, int dice, double s)
{
    double cp[3], cc[3][3], m = 1e9;
    poseOf(frame, cp, cc);
    for (int k = 0; k < dice; k++) {
        double dp[3], dc[3][3];
        poseOf(frame + (1 + k) * CN_ROLL_POSE_FLOATS, dp, dc);
        for (int i = 0; i < 8; i++) {
            double lx = i & 1 ? s : -s, ly = i & 2 ? s : -s, lz = i & 4 ? s : -s, w[3];
            for (int a = 0; a < 3; a++) w[a] = dp[a] + dc[0][a] * lx + dc[1][a] * ly + dc[2][a] * lz - cp[a];
            /* into the cup's frame: dot with its columns */
            double q = w[0] * cc[2][0] + w[1] * cc[2][1] + w[2] * cc[2][2];
            if (q < m) m = q;
        }
    }
    return m;
}

static int upOf(const float *pose)
{
    double p[3], c[3][3]; poseOf(pose, p, c);
    int best = 0; double bz = -2;
    for (int i = 0; i < 6; i++) { double z = (i & 1 ? -1 : 1) * c[i >> 1][2]; if (z > bz) { bz = z; best = i; } }
    return best;
}

static uint32_t fnv(const void *p, size_t n)
{
    const uint8_t *b = p; uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static void test_determinism(void)
{
    TEST("K14 the same seed bakes the same bytes");
    CnThrow t; CnRollInfo a, b;
    cn_throw_default(&t, CN_THROW_CUP, CUP_X, CUP_Y, CUP_R, DIE, RING);
    int n = cn_roll_bake(&t, 2026, F, P, CN_ROLL_MAX_FRAMES, &a);
    int m = cn_roll_bake(&t, 2026, F2, 0, CN_ROLL_MAX_FRAMES, &b);
    CHECK(n > 0 && n == m, "the same length");
    CHECK(!memcmp(F, F2, (size_t)n * CN_ROLL_FRAME_FLOATS * sizeof(float)), "the same frames");
    CHECK(!memcmp(a.up, b.up, sizeof a.up), "the same hand");
    cn_roll_bake(&t, 2027, F2, 0, CN_ROLL_MAX_FRAMES, &b);
    CHECK(memcmp(F + CN_ROLL_FRAME_FLOATS * 20, F2 + CN_ROLL_FRAME_FLOATS * 20, CN_ROLL_FRAME_FLOATS * sizeof(float)) != 0, "another seed is another throw");
    /* THE GOLDEN: the recipe is part of what a phone would show. Printed when it moves. */
    uint32_t h = fnv(F, (size_t)n * CN_ROLL_FRAME_FLOATS * sizeof(float));
    printf("  golden: seed 2026 bakes %d frames, hand %d%d%d%d%d, fnv %08x\n", n, a.up[0], a.up[1], a.up[2], a.up[3], a.up[4], h);
    CHECK(a.complete, "complete");
    CHECK(n == 191 && h == 0xa8063c38u && a.up[0] == 2 && a.up[1] == 1 && a.up[2] == 5 && a.up[3] == 1 && a.up[4] == 1,
          "the golden: seed 2026 is 191 frames, hand 21511, fnv a8063c38 (a change here is a change of recipe)");
}

static void test_cup(int seeds)
{
    TEST("K14 the cup roll over seeds");
    CnThrow t; cn_throw_default(&t, CN_THROW_CUP, CUP_X, CUP_Y, CUP_R, DIE, RING);
    const double s = DIE / 2, t_wall = t.cup_t;
    double worst = 1e9; int forced = 0, same = 0, opp = 0, hist[6] = { 0 }, slowest = 0; long total = 0;
    double ms = 0;
    for (int k = 0; k < seeds; k++) {
        CnRollInfo info;
        /* every seat shakes its own length: the lengths round to the bob's beat inside the bake */
        t.shake_s = k % 5 == 0 ? 0 : 1.5f + (k % 9) * .1f;
        clock_t c0 = clock();
        int n = cn_roll_bake(&t, 1000 + (uint64_t)k * 131, F, P, CN_ROLL_MAX_FRAMES, &info);
        ms += (double)(clock() - c0) * 1000 / CLOCKS_PER_SEC;
        CHECK(n > 0 && info.complete, "seed %d completes in %d frames", k, n);
        CHECK(info.slam > 0 && info.slam < n, "seed %d has its slam at frame %d", k, info.slam);
        if (n - info.slam > slowest) slowest = n - info.slam;
        forced += info.forced;
        /* the mouth, every frame up to the slam */
        for (int f = 0; f + 1 < info.slam; f++) { double m = mouthMargin(F + f * CN_ROLL_FRAME_FLOATS, 5, s); if (m < worst) worst = m; }
        /* the face up before the flip against the face up after */
        int flipAt = 0; while (flipAt < n && P[flipAt] != CN_RP_FLIP) flipAt++;
        const float *last = F + (n - 1) * CN_ROLL_FRAME_FLOATS;
        for (int d = 0; d < 5; d++) {
            int before = upOf(F + flipAt * CN_ROLL_FRAME_FLOATS + (1 + d) * CN_ROLL_POSE_FLOATS), after = upOf(last + (1 + d) * CN_ROLL_POSE_FLOATS);
            CHECK(after == info.up[d], "seed %d die %d: the frame's up face is the info's", k, d);
            hist[after]++; total++;
            if (after == before) same++;
            if (after == (before ^ 1)) opp++;
            /* flat, on the table, inside the mouth */
            double p[3], c[3][3]; poseOf(last + (1 + d) * CN_ROLL_POSE_FLOATS, p, c);
            CHECK(fabs(p[2] - s) < 1e-3, "seed %d die %d rests on the table", k, d);
            CHECK(fabs((after & 1 ? -1 : 1) * c[after >> 1][2] - 1) < 1e-6, "seed %d die %d is flat", k, d);
            double r = hypot(p[0] - CUP_X, p[1] - CUP_Y);
            CHECK(r + s * 1.4143 <= CUP_R - t_wall + 1.5, "seed %d die %d is under the cup, a corner at most a point into the wall (%.1f from its centre)", k, d, r);
            for (int e = d + 1; e < 5; e++) {
                double q[3], cq[3][3]; poseOf(last + (1 + e) * CN_ROLL_POSE_FLOATS, q, cq);
                CHECK(hypot(p[0] - q[0], p[1] - q[1]) >= DIE * 1.2, "seed %d dice %d and %d do not overlap", k, d, e);
            }
        }
        /* the cup is home, upright */
        double cp[3], cc[3][3]; poseOf(last, cp, cc);
        CHECK(fabs(cp[0] - CUP_X) < 1e-3 && fabs(cp[1] - CUP_Y) < 1e-3 && fabs(cp[2]) < 1e-3 && fabs(cc[2][2] - 1) < 1e-6, "seed %d: the cup is home", k);
        CHECK(P[n - 1] == CN_RP_IDLE && P[0] == CN_RP_HOLD, "seed %d: the phases run hold to idle", k);
    }
    CHECK(worst > 5, "no die corner comes within 5 points of the mouth before the slam (closest %.1f)", worst);
    CHECK(forced * 100 <= total * 2, "the last resort placed %d of %ld dice", forced, total);
    /* fair: the throw does not set the dice */
    double chi = 0; for (int i = 0; i < 6; i++) { double e = (double)total / 6; chi += (hist[i] - e) * (hist[i] - e) / e; }
    /* a half-second turn sets each die down on the face it rode on (its opposite comes up), so the
     * shake is the randomiser: what is checked is that the six faces come up evenly */
    if (seeds >= 100) CHECK(chi < 20.5, "every face comes up as often: chi-square %.1f over 5 degrees (p = .001 at 20.5)", chi);
    printf("  cup roll: %d seeds, %.2f ms a bake, closest to the mouth %.1f pt, forced %d/%ld, slowest settle %.2f s, same %ld%% opposite %ld%%, faces %d %d %d %d %d %d (chi2 %.1f)\n",
           seeds, ms / seeds, worst, forced, total, slowest / (double)CN_ROLL_HZ, same * 100 / total, opp * 100 / total, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], chi);
}

static void test_table(int seeds)
{
    TEST("K14 the table roll over seeds");
    CnThrow t; cn_throw_default(&t, CN_THROW_TABLE, CUP_X, CUP_Y, CUP_R, DIE, RING);
    const double s = DIE / 2;
    for (int k = 0; k < seeds; k++) {
        CnRollInfo info;
        int n = cn_roll_bake(&t, 5000 + (uint64_t)k * 131, F, P, CN_ROLL_MAX_FRAMES, &info);
        CHECK(n > 0 && info.complete, "seed %d completes in %d frames", k, n);
        int sit = 0, fall = 0; for (int f = 0; f < n; f++) { if (P[f] == CN_RP_SIT) sit++; if (P[f] == CN_RP_FALL) fall++; }
        CHECK(sit >= CN_ROLL_HZ - 1, "seed %d: the dice sit a second (%d frames)", k, sit);
        CHECK(fall > 6, "seed %d: the dice fall and tumble (%d frames)", k, fall);
        const float *last = F + (n - 1) * CN_ROLL_FRAME_FLOATS;
        for (int d = 0; d < 5; d++) {
            double p[3], c[3][3]; poseOf(last + (1 + d) * CN_ROLL_POSE_FLOATS, p, c);
            CHECK(fabs(p[2] - s) < 1e-3, "seed %d die %d rests on the table", k, d);
            double r = hypot(p[0] - CUP_X, p[1] - CUP_Y);
            CHECK(r > RING * .9 && r < RING * 1.1, "seed %d die %d is on the ring (%.1f)", k, d, r);
            for (int e = d + 1; e < 5; e++) {
                double q[3], cq[3][3]; poseOf(last + (1 + e) * CN_ROLL_POSE_FLOATS, q, cq);
                CHECK(hypot(p[0] - q[0], p[1] - q[1]) >= DIE * 1.3, "seed %d dice %d and %d are apart", k, d, e);
            }
        }
        double cp[3], cc[3][3]; poseOf(last, cp, cc);
        CHECK(fabs(cp[0] - CUP_X) < 1e-3 && fabs(cp[1] - CUP_Y) < 1e-3 && fabs(cp[2]) < 1e-3, "seed %d: the cup is home", k);
        /* while falling, every die is below the cup's mouth and above the table */
        for (int f = 1; f < 4; f++) for (int d = 0; d < 5; d++) {
            double p[3], c[3][3]; poseOf(F + f * CN_ROLL_FRAME_FLOATS + (1 + d) * CN_ROLL_POSE_FLOATS, p, c);
            CHECK(p[2] > 0 && p[2] < 160, "seed %d frame %d die %d is in the air (%.0f)", k, f, d, p[2]);
        }
    }
}

/* a far seat's cup: a third the size, thrown with the reach scaled, its own shake length; the dice stay in */
static void test_small(int seeds)
{
    TEST("K14 a far seat's small cup");
    CnThrow t; cn_throw_default(&t, CN_THROW_CUP, 60, 80, 22, 9, 13);
    const double s = 4.5;
    double worst = 1e9; int forced = 0;
    for (int k = 0; k < seeds; k++) {
        CnRollInfo info;
        t.shake_s = 1.5f + (k % 9) * .1f;
        int n = cn_roll_bake(&t, 7000 + (uint64_t)k * 131, F, P, CN_ROLL_MAX_FRAMES, &info);
        CHECK(n > 0 && info.complete, "seed %d completes in %d frames", k, n);
        forced += info.forced;
        for (int f = 0; f + 1 < info.slam; f++) { double m = mouthMargin(F + f * CN_ROLL_FRAME_FLOATS, 5, s); if (m < worst) worst = m; }
        int flipAt = 0; while (flipAt < n && P[flipAt] != CN_RP_FLIP) flipAt++;
        CHECK(fabs(flipAt / (double)CN_ROLL_HZ - (.3 + floor(t.shake_s * 4 + .5) / 4)) < .05, "seed %d: the turn starts after the hold and its own shake, rounded to the bob's beat (%.2f s)", k, flipAt / (double)CN_ROLL_HZ);
        const float *last = F + (n - 1) * CN_ROLL_FRAME_FLOATS;
        for (int d = 0; d < 5; d++) {
            double p[3], c[3][3]; poseOf(last + (1 + d) * CN_ROLL_POSE_FLOATS, p, c);
            CHECK(fabs(p[2] - s) < 1e-3 && hypot(p[0] - 60, p[1] - 80) + s * 1.4143 <= 22 - 22 * .06 + 1.5, "seed %d die %d rests under the small cup", k, d);
        }
    }
    CHECK(worst > 2, "no die corner comes within two points of the small mouth (closest %.1f)", worst);
    CHECK(forced * 100 <= seeds * 5 * 2, "the last resort placed %d dice", forced);
    printf("  small cup: %d seeds, closest to the mouth %.1f pt, forced %d\n", seeds, worst, forced);
}

static void test_edges(void)
{
    TEST("K14 the cap and a bad throw");
    CnThrow t; CnRollInfo info;
    cn_throw_default(&t, CN_THROW_CUP, CUP_X, CUP_Y, CUP_R, DIE, RING);
    int n = cn_roll_bake(&t, 1, F, P, 40, &info);
    CHECK(n == 40 && !info.complete, "a cap of 40 frames fills and reports incomplete");
    t.kind = 9;
    CHECK(cn_roll_bake(&t, 1, F, P, CN_ROLL_MAX_FRAMES, &info) == 0, "an unknown kind bakes nothing");
    cn_throw_default(&t, CN_THROW_TABLE, CUP_X, CUP_Y, CUP_R, DIE, RING); t.dice = 0;
    CHECK(cn_roll_bake(&t, 1, F, P, CN_ROLL_MAX_FRAMES, &info) == 0, "no dice bakes nothing");
    t.dice = 3;
    n = cn_roll_bake(&t, 1, F, P, CN_ROLL_MAX_FRAMES, &info);
    CHECK(n > 0 && info.complete, "three dice bake");
    CHECK(F[(n - 1) * CN_ROLL_FRAME_FLOATS + 4 * CN_ROLL_POSE_FLOATS + 2] < -1000, "an absent die is parked far below the table");
    CHECK(cn_roll_bake(&t, 1, 0, P, CN_ROLL_MAX_FRAMES, &info) == 0 && cn_roll_bake(&t, 1, F, P, 0, &info) == 0, "no buffer or no cap bakes nothing");
}

int main(int argc, char **argv)
{
    int seeds = argc > 1 ? atoi(argv[1]) : 300;
    if (seeds < 10) seeds = 10;
    test_determinism();
    test_cup(seeds);
    test_table(seeds / 3 > 20 ? seeds / 3 : 20);
    test_small(seeds > 20 ? seeds : 20);
    test_edges();
    return report("cn_roll_test");
}
