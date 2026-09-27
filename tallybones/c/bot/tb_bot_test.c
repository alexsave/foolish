/* tb_bot_test - every check on the exact solver (DECISIONS.md T30 onwards).
 *
 *   tb_bot_test [GAMES]     GAMES simulated games for the exact-vs-simulated
 *                           check (default 200000)
 *
 * Each group names what it proves; MUTATIONS.md lists the break that turns
 * each one red. The independent recomputations here enumerate dice BY
 * POSITION with their own loops and score with tb_score_of directly, so
 * they share no combinatorics with tb_bot.c. */
#include "tb_bot.h"
#include "../src/tb_code.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            fails++;                                                       \
            if (fails <= 20) {                                             \
                printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);     \
                printf(__VA_ARGS__);                                       \
                printf("\n");                                              \
            }                                                              \
        }                                                                  \
    } while (0)

static TbBot *B;
static const uint32_t POW6[6] = { 1, 6, 36, 216, 1296, 7776 };

/* The test's own hand lookup: sort, then find the sorted dice in b->hand. */
static int own_hand(const uint8_t d[5])
{
    uint8_t s[5];
    memcpy(s, d, 5);
    for (int i = 1; i < 5; i++)
        for (int j = i; j > 0 && s[j - 1] > s[j]; j--) { uint8_t x = s[j]; s[j] = s[j - 1]; s[j - 1] = x; }
    for (int h = 0; h < TB_BOT_HANDS; h++)
        if (!memcmp(B->hand[h], s, 5)) return h;
    return -1;
}

/* Fill the positions NOT in `keep` with raw outcome `raw` (base 6). */
static void reroll(uint8_t out[5], const uint8_t in[5], int keep, uint32_t raw)
{
    for (int i = 0; i < 5; i++) {
        if (keep >> i & 1) { out[i] = in[i]; continue; }
        out[i] = (uint8_t)(1 + raw % 6);
        raw /= 6;
    }
}

/* ---- G1: the reroll distributions ---------------------------------------------- */

static void test_distributions(void)
{
    CHECK(B->out_at[TB_BOT_KEEPS] == TB_BOT_ENTRY, "entries %d", B->out_at[TB_BOT_KEEPS]);
    int pairs = 0, bad_sum = 0, bad_brute = 0;
    double worst = 0;
    for (int h = 0; h < TB_BOT_HANDS; h++) {
        CHECK(own_hand(B->hand[h]) == h && tb_bot_hand(B, B->hand[h]) == h, "hand %d", h);
        for (int mask = 0; mask < 32; mask++) {
            const uint8_t *d = B->hand[h];
            int k = tb_bot_keep(B, d, mask), r = 5 - __builtin_popcount((unsigned)mask);
            pairs++;
            uint64_t total = 0;
            double p = 0;
            for (int e = B->out_at[k]; e < B->out_at[k + 1]; e++) {
                total += B->out_count[e];
                p += (double)B->out_count[e] / POW6[r];
            }
            if (total != POW6[r]) bad_sum++;
            if (fabs(p - 1) > worst) worst = fabs(p - 1);
            /* brute force: every raw outcome of the rerolled POSITIONS */
            uint32_t count[TB_BOT_HANDS] = { 0 };
            for (uint32_t raw = 0; raw < POW6[r]; raw++) {
                uint8_t o[5];
                reroll(o, d, mask, raw);
                count[own_hand(o)]++;
            }
            int seen = 0;
            for (int e = B->out_at[k]; e < B->out_at[k + 1]; e++) {
                if (count[B->out_hand[e]] != B->out_count[e]) bad_brute++;
                seen++;
            }
            int nonzero = 0;
            for (int x = 0; x < TB_BOT_HANDS; x++) nonzero += count[x] != 0;
            if (nonzero != seen) bad_brute++;
        }
    }
    CHECK(bad_sum == 0, "%d (hand, keep) pairs whose counts do not sum to 6^r", bad_sum);
    CHECK(worst < 1e-12, "a distribution sums to 1 %+.3g", worst);
    CHECK(bad_brute == 0, "%d (hand, keep) pairs disagree with the raw enumeration", bad_brute);
    printf("G1 distributions: %d (hand, keep) pairs, %d entries, every one sums to 1 (worst %.2g)\n", pairs,
           B->out_at[TB_BOT_KEEPS], worst);
}

/* ---- G2: one roll of five dice, against hand-counted outcomes -------------------- */

static void test_one_roll(void)
{
    /* hand counts over 7776: five alike 6; a long run 2 x 5! = 240; a full
     * house 6 x 5 x C(5,3) = 300; four or more alike 6 x 5 x 5 + 6 = 156;
     * three or more 6 x C(5,3) x 25 + 156 = 1656; a short run (four in
     * sequence, a long run included) 1200 */
    static const struct { int cat; uint32_t want; } row[] = {
        { TB_C_TALLYBONES, 6 },    { TB_C_LONG_RUN, 240 },     { TB_C_FULL_HOUSE, 300 },
        { TB_C_FOUR_ALIKE, 156 },  { TB_C_THREE_ALIKE, 1656 }, { TB_C_SHORT_RUN, 1200 },
    };
    const uint8_t none[5] = { 1, 1, 1, 1, 1 };
    int k = tb_bot_keep(B, none, 0);
    for (unsigned i = 0; i < sizeof row / sizeof row[0]; i++) {
        uint32_t n = 0;
        double p = 0;
        for (int e = B->out_at[k]; e < B->out_at[k + 1]; e++)
            if (tb_score_of(B->hand[B->out_hand[e]], row[i].cat) > 0) {
                n += B->out_count[e];
                p += B->out_count[e] / 7776.0;
            }
        CHECK(n == row[i].want, "category %d: %u of 7776, want %u", row[i].cat, n, row[i].want);
        CHECK(fabs(p - row[i].want / 7776.0) < 1e-15, "category %d: p %.17g", row[i].cat, p);
    }
    printf("G2 one roll: five alike 6/7776, long run 240/7776, full house 300/7776, four alike 156/7776, "
           "three alike 1656/7776, short run 1200/7776\n");
}

/* ---- G3: whole sub-problems with hand-computed values ----------------------------- */

static double binom_ge(int n, int k, double p)
{
    double s = 0;
    for (int j = k; j <= n; j++) {
        double c = 1;
        for (int i = 0; i < j; i++) c = c * (n - i) / (i + 1);
        s += c * pow(p, j) * pow(1 - p, n - j);
    }
    return s;
}

static void test_subproblems(void)
{
    const double hit = 91.0 / 216;                      /* a die shows a face within 3 tries */
    struct { const char *what; int rem, upper; double want; } row[] = {
        /* one die, keep a 5 or 6 then a 4+: (5+6)/6 + 4/6 x ((4+5+6)/6 + 3/6 x 3.5) = 14/3 */
        { "Any alone: 5 x 14/3", 1 << TB_C_ANY, 0, 70.0 / 3 },
        { "Sixes alone: 5 x 6 x 91/216", 1 << TB_C_SIXES, 0, 30 * hit },
        { "Sixes alone at 63: no bonus left", 1 << TB_C_SIXES, 63, 30 * hit },
        { "Sixes alone at 33: + 35 x P(five sixes)", 1 << TB_C_SIXES, 33, 30 * hit + 35 * pow(hit, 5) },
        { "Ones alone at 60: + 35 x P(3+ ones)", 1 << TB_C_ONES, 60, 5 * hit + 35 * binom_ge(5, 3, hit) },
        /* the published probability of five alike in three rolls, optimal play */
        { "Tallybones alone: 50 x 2783176/6^10", 1 << TB_C_TALLYBONES, 0, 50 * 2783176.0 / 60466176.0 },
        { "nothing left", 0, 17, 0 },
    };
    for (unsigned i = 0; i < sizeof row / sizeof row[0]; i++) {
        double v = tb_bot_value(B, row[i].rem, row[i].upper);
        CHECK(fabs(v - row[i].want) < 1e-9, "%s: %.12f, want %.12f", row[i].what, v, row[i].want);
        printf("G3 %-42s %.9f\n", row[i].what, v);
    }
    /* one roll, no reroll, then the best category: v[0] over the first roll */
    struct { int rem; double want; } zero[] = {
        { 1 << TB_C_ANY, 17.5 }, { 1 << TB_C_TALLYBONES, 50 * 6 / 7776.0 }, { 1 << TB_C_LONG_RUN, 40 * 240 / 7776.0 },
        { 1 << TB_C_FULL_HOUSE, 25 * 300 / 7776.0 }, { 1 << TB_C_SHORT_RUN, 30 * 1200 / 7776.0 },
    };
    static TbBotTurn t;
    for (unsigned i = 0; i < sizeof zero / sizeof zero[0]; i++) {
        tb_bot_turn(B, zero[i].rem, 0, &t);
        double acc = 0;
        for (uint32_t raw = 0; raw < 7776; raw++) {
            uint8_t d[5], z[5] = { 0 };
            reroll(d, z, 0, raw);
            acc += t.v[0][own_hand(d)];
        }
        CHECK(fabs(acc / 7776 - zero[i].want) < 1e-12, "no reroll, mask %#x: %.12f, want %.12f", zero[i].rem,
              acc / 7776, zero[i].want);
    }
    printf("G3 no rerolls: Any 17.5, Tallybones 50 x 6/7776, Long Run 40 x 240/7776, Full House 25 x 300/7776, "
           "Short Run 30 x 1200/7776\n");
}

/* ---- G4: the policy against brute force by position -------------------------------- */

/* The test's own "score now": tb_score_of, the bonus rule, the table. */
static double own_score_now(int rem, int upper, const uint8_t d[5], int *cat)
{
    double best = -1;
    for (int c = 0; c < TB_CATS; c++) {
        if (!(rem >> c & 1)) continue;
        int s = tb_score_of(d, c), u = upper;
        double v = s;
        if (c < TB_UPPER_CATS) {
            u = upper + s;
            if (upper < 63 && u >= 63) v += 35;
            if (u > 63) u = 63;
        }
        v += tb_bot_value(B, rem & ~(1 << c), u);
        if (v > best) { best = v; if (cat) *cat = c; }
    }
    return best;
}

static void test_policy(int nstates)
{
    static const struct { int rem, upper; } st[] = {
        { TB_FULL_CARD, 0 }, { TB_FULL_CARD & ~0x3F, 0 }, { 0x3F, 50 }, { 0x1555, 20 },
        { (1 << TB_C_TALLYBONES) | (1 << TB_C_FIVES) | (1 << TB_C_SHORT_RUN), 55 }, { 0x0AAA, 62 },
        { (1 << TB_C_FULL_HOUSE) | (1 << TB_C_LONG_RUN) | (1 << TB_C_ONES), 61 },
    };
    static TbBotTurn t;
    int bad_v0 = 0, bad_ev = 0, bad_arg = 0, bad_table = 0, bad_kind = 0, n = 0;
    double worst = 0;
    for (int s = 0; s < nstates && s < (int)(sizeof st / sizeof st[0]); s++) {
        tb_bot_turn(B, st[s].rem, st[s].upper, &t);
        static double own_v[TB_ROLLS][TB_BOT_HANDS];
        for (int h = 0; h < TB_BOT_HANDS; h++) {
            own_v[0][h] = own_score_now(st[s].rem, st[s].upper, B->hand[h], 0);
            if (own_v[0][h] != t.v[0][h]) bad_v0++;
        }
        for (int left = 0; left < TB_ROLLS; left++) {
            for (int h = 0; h < TB_BOT_HANDS; h++) {
                /* the hand, in a scrambled order, so keeping is by position */
                uint8_t d[5];
                for (int i = 0; i < 5; i++) d[i] = B->hand[h][(i * 3 + h) % 5];
                double val[32], best = own_v[0][h];
                val[31] = own_v[0][h];
                for (int mask = 0; left > 0 && mask < 31; mask++) {
                    int r = 5 - __builtin_popcount((unsigned)mask);
                    double acc = 0;
                    for (uint32_t raw = 0; raw < POW6[r]; raw++) {
                        uint8_t o[5];
                        reroll(o, d, mask, raw);
                        acc += own_v[left - 1][own_hand(o)];
                    }
                    val[mask] = acc / POW6[r];
                    if (val[mask] > best) best = val[mask];
                }
                if (left > 0) own_v[left][h] = best;
                TbBotChoice ch = tb_bot_choose(B, &t, d, left);
                n++;
                if (fabs(ch.ev - best) > worst) worst = fabs(ch.ev - best);
                if (fabs(ch.ev - best) > 1e-9) bad_ev++;
                if (ch.ev != t.v[left][h]) bad_table++;
                if (ch.kind == TB_M_KEEP) {
                    if (left == 0 || ch.arg < 0 || ch.arg >= 31) bad_kind++;
                    else if (fabs(val[ch.arg] - best) > 1e-9) bad_arg++;
                } else if (ch.kind == TB_M_SCORE) {
                    int c = -1;
                    own_score_now(st[s].rem, st[s].upper, d, &c);
                    if (ch.arg < 0 || !(st[s].rem >> ch.arg & 1)) bad_kind++;
                    else if (fabs(val[31] - best) > 1e-9) bad_arg++;
                    else {
                        /* the category must reach the best score-now value */
                        double v = tb_score_of(d, ch.arg);
                        int u = st[s].upper;
                        if (ch.arg < 6) { if (u < 63 && u + v >= 63) v += 35; u = u + (int)tb_score_of(d, ch.arg); }
                        v += tb_bot_value(B, st[s].rem & ~(1 << ch.arg), u);
                        if (fabs(v - own_v[0][h]) > 1e-9) bad_arg++;
                    }
                } else bad_kind++;
            }
        }
    }
    CHECK(bad_v0 == 0, "%d hands whose score-now value differs from tb_score_of + bonus + table", bad_v0);
    CHECK(bad_ev == 0, "%d choices whose ev is not the brute-force best (worst %.3g)", bad_ev, worst);
    CHECK(bad_arg == 0, "%d choices whose move does not reach the brute-force best", bad_arg);
    CHECK(bad_table == 0, "%d choices whose ev is not the turn table's", bad_table);
    CHECK(bad_kind == 0, "%d choices of an illegal kind or argument", bad_kind);
    printf("G4 policy: %d choices against a by-position brute force, worst %.2g\n", n, worst);
}

/* ---- G5: the between-turns table ---------------------------------------------------- */

static void test_table(void)
{
    int bad_u = 0, bad_mono = 0, bad_turn = 0;
    static TbBotTurn t;
    for (int rem = 1; rem < TB_BOT_MASKS; rem++) {
        for (int u = 1; u < TB_BOT_UPPER; u++) {
            if (!(rem & 0x3F) && B->ev[rem][u] != B->ev[rem][0]) bad_u++;
            /* ev is points still to come, and at 63 the bonus is already
             * banked, so the monotone quantity is ev plus the banked bonus */
            double hi = B->ev[rem][u] + (u >= 63 ? 35 : 0), lo = B->ev[rem][u - 1];
            if (hi < lo - 1e-9) bad_mono++;
        }
        if (rem % 257 == 1) {         /* a spread of masks: the first roll by position */
            for (int u = 0; u < TB_BOT_UPPER; u += 21) {
                tb_bot_turn(B, rem, u, &t);
                double acc = 0;
                for (uint32_t raw = 0; raw < 7776; raw++) {
                    uint8_t d[5], z[5] = { 0 };
                    reroll(d, z, 0, raw);
                    acc += tb_bot_choose(B, &t, d, 2).ev;
                }
                if (fabs(acc / 7776 - B->ev[rem][u]) > 1e-9) bad_turn++;
            }
        }
    }
    CHECK(bad_u == 0, "%d states with no numbers left that depend on the upper total", bad_u);
    CHECK(bad_mono == 0, "%d states worth less with a higher upper total", bad_mono);
    CHECK(bad_turn == 0, "%d states whose value is not the policy's over the first roll", bad_turn);
    printf("G5 table: independent of the upper total once the numbers are done, monotone in it (with the banked bonus), and "
           "every sampled state is the policy's expectation over the first roll\n");
}

/* ---- G6: the reference number --------------------------------------------------------- */

static double EV;

static void test_reference(void)
{
    EV = tb_bot_value(B, TB_FULL_CARD, 0);
    /* the published optimum for the branded 13-category game without its
     * extra five-alike bonus and joker rule (T33) */
    CHECK(fabs(EV - 245.87) < 0.005, "exact EV %.6f, the published figure is about 245.87", EV);
    CHECK(fabs(EV - 245.870775) < 5e-7, "exact EV %.9f, pinned 245.870775", EV);
    printf("G6 exact expected final score %.6f (published: about 245.87)\n", EV);
}

/* ---- G7: the simulation against the exact value --------------------------------------- */

static void test_simulation(uint64_t games)
{
    uint8_t seed[32];
    memcpy(seed, "tallybones bot simulation seed..", 32);
    static TbBotSim sim, one, many;
    CHECK(tb_bot_simulate(B, seed, games, 8, &sim), "a simulated game went wrong");
    double n = (double)sim.n, mean = sim.sum / n;
    double se = sqrt((sim.sumsq - n * mean * mean) / (n - 1) / n);
    double z = (mean - EV) / se;
    CHECK(sim.n == games, "played %llu", (unsigned long long)sim.n);
    CHECK(fabs(z) < 4, "simulated mean %.4f vs exact %.4f: %.2f standard errors", mean, EV, z);
    printf("G7 %llu games by the policy: mean %.4f, standard error %.4f, exact %.4f, %+.2f SE\n",
           (unsigned long long)sim.n, mean, se, EV, z);
    /* the same games whatever the thread count */
    tb_bot_simulate(B, seed, 2000, 1, &one);
    tb_bot_simulate(B, seed, 2000, 5, &many);
    CHECK(!memcmp(&one, &many, sizeof one), "the tallies depend on the thread count");
}

/* ---- G8: the policy drives the real kernel ---------------------------------------------- */

static void test_kernel(int games)
{
    static TbGame g;
    static TbMove moves[TB_HIST_CAP];
    static TbBotTurn t[2];
    int bad_legal = 0, bad_total = 0, bad_dice = 0, played = 0;
    long sum = 0;
    for (int gi = 0; gi < games; gi++) {
        uint8_t seed[32];
        memset(seed, 0, 32);
        memcpy(seed, "kernel game", 11);
        seed[31] = (uint8_t)gi; seed[30] = (uint8_t)(gi >> 8);
        int k = 0, mine[2] = { 0, 0 };
        tb_replay(&g, seed, 2, 0, moves, 0);
        while (!g.over) {
            int s = g.turn, rem = TB_FULL_CARD & ~g.filled[s];
            for (int i = 0; i < 5; i++) if (g.dice[i] < 1 || g.dice[i] > 6) bad_dice++;
            if (g.roll == 1) tb_bot_turn(B, rem, tb_upper(&g, s), &t[s]);
            TbBotChoice ch = tb_bot_choose(B, &t[s], g.dice, TB_ROLLS - g.roll);
            TbMove m = { (uint8_t)ch.kind, (uint8_t)s, (uint8_t)ch.arg, 0 };
            if (!tb_is_legal(&g, m)) { bad_legal++; break; }
            if (ch.kind == TB_M_SCORE) mine[s] += tb_score_of(g.dice, ch.arg);
            moves[k++] = m;
            if (!tb_replay(&g, seed, 2, 0, moves, k)) { bad_legal++; break; }
        }
        for (int s = 0; s < 2; s++) {
            int up = tb_upper(&g, s);
            if (tb_total(&g, s) != mine[s] + (up >= 63 ? 35 : 0)) bad_total++;
            sum += tb_total(&g, s);
        }
        played++;
    }
    CHECK(bad_legal == 0, "%d bot moves the kernel refused", bad_legal);
    CHECK(bad_dice == 0, "%d resident dice off 1..6", bad_dice);
    CHECK(bad_total == 0, "%d cards whose kernel total differs from the bot's own tally", bad_total);
    printf("G8 kernel: %d two-seat games through tb_replay, every move legal, mean seat total %.2f\n", played,
           (double)sum / (2.0 * played));
}

int main(int argc, char **argv)
{
    uint64_t games = argc > 1 ? strtoull(argv[1], 0, 10) : 200000;
    int small = games < 100000;
    B = malloc(sizeof *B);
    if (!B || !tb_bot_build(B, 8)) { printf("FAIL build\n"); return 1; }
    test_distributions();
    test_one_roll();
    test_subproblems();
    test_policy(small ? 2 : 7);
    test_table();
    test_reference();
    test_simulation(games);
    test_kernel(small ? 10 : 200);
    free(B);
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}
