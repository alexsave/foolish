/* Chui Niu - the bot's tests. `make -C chuiniu/c/bot bot-test`.
 *
 * Every test here was mutation-checked by hand: MUTATIONS.md names the
 * break, the assertion that went red, and the restore. */
#include "cn_bot.h"
#include "../tests/cn_check.h"
#include <math.h>

static CnSeen   S;
static CnBelief B;

static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

/* a hand-built open round: n seats of five dice, total 5n, my dice given */
static void seen_start(int n, int me, const uint8_t my[5])
{
    memset(&S, 0, sizeof S);
    S.n = (uint8_t)n;
    S.me = (uint8_t)me;
    S.turn = (uint8_t)me;
    S.total = (uint8_t)(5 * n);
    S.bidder = CN_SEAT_NONE;
    for (int s = 0; s < n; s++) S.dice_n[s] = 5;
    memcpy(S.my_dice, my, 5);
}

static void seen_bid(int seat, int q, int f)
{
    CnSeenBid *b = &S.bids[S.nbids++];
    *b = (CnSeenBid){ (uint8_t)seat, (uint8_t)q, (uint8_t)f, S.bid_q, S.bid_f };
    S.bid_q = (uint8_t)q;
    S.bid_f = (uint8_t)f;
    S.bidder = (uint8_t)seat;
}

static double mean_k(int seat, int f)
{
    double m = 0.0;
    for (int k = 0; k <= S.dice_n[seat]; k++) m += k * B.pk[seat][f][k];
    return m;
}

/* ---- the binomial and the claim primitive -------------------------------------- */

static void test_binomial(void)
{
    TEST("binomial table is exact");
    CHECK(near(cn_binom_pmf(2, 1), 4.0 / 9.0, 1e-15), "pmf(2,1) %.17g", cn_binom_pmf(2, 1));
    CHECK(near(cn_binom_pmf(3, 0), 8.0 / 27.0, 1e-15), "pmf(3,0) %.17g", cn_binom_pmf(3, 0));
    CHECK(near(cn_binom_pmf(5, 2), 80.0 / 243.0, 1e-15), "pmf(5,2) %.17g", cn_binom_pmf(5, 2));
    CHECK(near(cn_binom_tail(2, 1), 5.0 / 9.0, 1e-15), "tail(2,1) %.17g", cn_binom_tail(2, 1));
    CHECK(cn_binom_tail(7, 0) == 1.0 && cn_binom_tail(7, -3) == 1.0, "need <= 0 is certain");
    CHECK(cn_binom_tail(7, 8) == 0.0, "need > m is impossible");
    /* exact rationals summed in Python's fractions */
    CHECK(near(cn_binom_tail(30, 10), 0.5682556444214808, 1e-13), "tail(30,10) %.17g", cn_binom_tail(30, 10));
    CHECK(near(cn_binom_tail(30, 20), 0.0001937593013703032, 1e-16), "tail(30,20) %.17g", cn_binom_tail(30, 20));
    for (int m = 0; m <= 30; m++) {
        double s = 0.0;
        for (int k = 0; k <= m; k++) s += cn_binom_pmf(m, k);
        CHECK(near(s, 1.0, 1e-12), "pmf(%d, .) sums to %.17g", m, s);
        for (int k = 0; k <= m + 1; k++)
            CHECK(near(cn_binom_tail(m, k), cn_binom_tail_p(m, k, 1.0 / 3.0), 1e-13),
                  "the table and the direct loop agree at m %d k %d", m, k);
    }
}

static void test_claim(void)
{
    TEST("claim primitive, closed form");
    /* U = 2, p = 1/3, m = 1: 1 - (2/3)^2 */
    CHECK(near(cn_binom_tail_p(2, 1, 1.0 / 3.0), 5.0 / 9.0, 1e-15), "5/9: %.17g", cn_binom_tail_p(2, 1, 1.0 / 3.0));
    /* U = 3, p = 1/2, m = 2: (3 + 1) / 8 */
    CHECK(near(cn_binom_tail_p(3, 2, 0.5), 0.5, 1e-15), "1/2: %.17g", cn_binom_tail_p(3, 2, 0.5));
    /* U = 4, p = 1/6, m = 4: 1/1296 */
    CHECK(near(cn_binom_tail_p(4, 4, 1.0 / 6.0), 1.0 / 1296.0, 1e-18), "1/1296: %.17g", cn_binom_tail_p(4, 4, 1.0 / 6.0));
    CHECK(cn_binom_tail_p(5, 0, 0.3) == 1.0 && cn_binom_tail_p(5, 6, 0.3) == 0.0, "the ends");

    /* THE OWNER'S CASE: 25 dice, I hold 5 with k = 2 on the face, bid 12:
     * m = 10 of U = 20. Without wilds (p = 1/6) it is exactly
     * 5.985039366041009e-4 (Python fractions): small, but NOT 1e-5, because
     * a binomial's upper tail this far out is much fatter than the normal
     * the "4 standard deviations" estimate assumes. With wild 1s (p = 1/3)
     * it is 0.0919, about 150 times larger. */
    double p6 = cn_claim_prob(25, 5, 2, 12, 1.0 / 6.0);
    double p3 = cn_claim_prob(25, 5, 2, 12, 1.0 / 3.0);
    CHECK(near(p6, 0.0005985039366041009, 1e-16), "p = 1/6: %.17g", p6);
    CHECK(near(p3, 0.09189577448726231, 1e-15), "p = 1/3: %.17g", p3);
    CHECK(p6 < 1e-3 && p3 > 100 * p6, "tiny without wilds, much larger with: %g %g", p6, p3);
}

/* ---- the opponent model --------------------------------------------------------- */

/* cn_bid_lik's O(1) normaliser against the plain sum over every legal option */
static double lik_brute(const CnHand *h, int n, int total, int pq, int pf, int q, int f, float beta)
{
    int m = total - n;
    double z = 0.0, num = 0.0;
    if (pq > 0) {
        double w = exp(beta * (1.0 - cn_binom_tail(m, pq - cn_hand_k(h, pf))));
        z += w;
        if (q == 0) num = w;
    }
    for (int Q = 1; Q <= total; Q++)
        for (int F = 2; F <= 6; F++) {
            if (pq && cn_rank(Q, F) <= cn_rank(pq, pf)) continue;
            double w = exp(beta * cn_binom_tail(m, Q - cn_hand_k(h, F)));
            z += w;
            if (Q == q && F == f) num = w;
        }
    return num / z;
}

static void test_likelihood(void)
{
    TEST("bid likelihood is the softmax over every legal option");
    const CnHand *hands;
    int nh = cn_hands(5, &hands, 0);
    CHECK(nh == 252, "252 hands of five dice, got %d", nh);
    for (int h = 0; h < nh; h += 7) {
        const int cases[][4] = { { 0, 0, 2, 3 }, { 2, 3, 3, 3 }, { 3, 6, 5, 2 }, { 4, 4, 0, 0 }, { 6, 5, 7, 6 } };
        for (int c = 0; c < 5; c++) {
            const int *x = cases[c];
            double a = cn_bid_lik(&hands[h], 5, 15, x[0], x[1], x[2], x[3], 8.0f);
            double b = lik_brute(&hands[h], 5, 15, x[0], x[1], x[2], x[3], 8.0f);
            CHECK(near(a, b, 1e-12 * (b > 1 ? b : 1)), "hand %d case %d: %.17g vs %.17g", h, c, a, b);
        }
    }
    /* holding the face makes the bid likelier: three 3s against none */
    CnHand three = { { 0, 0, 0, 3, 1, 1, 0 } }, none = { { 0, 0, 2, 0, 2, 1, 0 } };
    CHECK(cn_bid_lik(&three, 5, 15, 2, 5, 4, 3, 8.0f) > 3 * cn_bid_lik(&none, 5, 15, 2, 5, 4, 3, 8.0f),
          "a bid of four 3s is evidence of 3s");
}

static void test_belief_direction(void)
{
    TEST("belief moves the right way");
    CnBotCfg cfg;
    cn_bot_cfg_default(&cfg);
    const uint8_t my[5] = { 2, 2, 5, 6, 6 };
    seen_start(3, 0, my);
    cn_belief_build(&S, &cfg, &B);
    /* nobody has bid: every seat is the prior */
    for (int s = 0; s < 3; s++)
        for (int f = 2; f <= 6; f++)
            for (int k = 0; k <= 5; k++)
                CHECK(near(B.pk[s][f][k], cn_binom_pmf(5, k), 1e-6), "prior at seat %d face %d k %d", s, f, k);

    /* seat 1 bids 3s twice; seat 0 (me) bids 6s between; seat 2 never bids */
    seen_bid(1, 2, 3);
    seen_bid(0, 4, 6);
    seen_bid(1, 5, 3);
    cn_belief_build(&S, &cfg, &B);
    double prior_mean = 5.0 / 3.0;
    CHECK(mean_k(1, 3) > prior_mean + 0.5, "seat 1's 3s rise: %.3f", mean_k(1, 3));
    CHECK(B.pk[1][3][0] < 0.5 * cn_binom_pmf(5, 0), "seat 1 holding no 3s is less likely: %.4f", B.pk[1][3][0]);
    for (int f = 2; f <= 6; f++)
        for (int k = 0; k <= 5; k++)
            CHECK(near(B.pk[2][f][k], cn_binom_pmf(5, k), 1e-6), "seat 2 never bid: prior at face %d k %d", f, k);
    CHECK(near(mean_k(0, 3), prior_mean, 0.25), "my 3s barely move from seat 1's bids: %.3f", mean_k(0, 3));
    CHECK(mean_k(0, 6) > prior_mean + 0.2, "my own 6s bid is public evidence of 6s: %.3f", mean_k(0, 6));

    /* one more bid of 3s moves seat 1 further */
    double before = mean_k(1, 3);
    seen_bid(2, 5, 5);
    seen_bid(0, 5, 6);
    seen_bid(1, 6, 3);
    cn_belief_build(&S, &cfg, &B);
    CHECK(mean_k(1, 3) > before, "a third bid of 3s: %.3f after %.3f", mean_k(1, 3), before);

    /* the opponent model switched off keeps every seat at the prior */
    cfg.use_belief = 0;
    cn_belief_build(&S, &cfg, &B);
    CHECK(near(mean_k(1, 3), prior_mean, 1e-5), "use_belief 0 is the prior: %.4f", mean_k(1, 3));
}

static void test_sampling(void)
{
    TEST("sampling respects the belief");
    CnBotCfg cfg;
    cn_bot_cfg_default(&cfg);
    const uint8_t my[5] = { 1, 2, 4, 5, 6 };
    seen_start(3, 0, my);
    seen_bid(1, 3, 3);
    seen_bid(2, 3, 4);
    seen_bid(0, 4, 2);
    seen_bid(1, 5, 3);
    cn_belief_build(&S, &cfg, &B);
    enum { N = 200000 };
    double hist[7][6] = { { 0 } };
    uint64_t r = 12345;
    for (int i = 0; i < N; i++) {
        const CnHand *h = cn_belief_sample(&B, 1, cn_unit(&r));
        int sum = 0;
        for (int f = 1; f <= 6; f++) sum += h->c[f];
        CHECK(sum == 5, "a sampled hand has five dice, not %d", sum);
        for (int f = 2; f <= 6; f++) hist[f][cn_hand_k(h, f)] += 1.0 / N;
    }
    for (int f = 2; f <= 6; f++)
        for (int k = 0; k <= 5; k++)
            CHECK(near(hist[f][k], B.pk[1][f][k], 0.006), "face %d k %d: sampled %.4f belief %.4f", f, k,
                  hist[f][k], B.pk[1][f][k]);
}

/* the closed form over the belief, against the same question asked of
 * sampled worlds, and against the flat primitive at the prior */
static void test_belief_claim(void)
{
    TEST("belief claim is the exact convolution");
    CnBotCfg cfg;
    cn_bot_cfg_default(&cfg);
    const uint8_t my[5] = { 1, 3, 3, 5, 6 };
    seen_start(4, 0, my);
    cn_belief_build(&S, &cfg, &B);
    for (int q = 1; q <= 20; q++)
        CHECK(near(cn_belief_claim(&S, &B, 0, 3, q, 3), cn_claim_prob(20, 5, 3, q, 1.0 / 3.0), 1e-6),
              "at the prior it is the binomial, q %d", q);
    seen_bid(1, 3, 4);
    seen_bid(2, 4, 4);
    seen_bid(3, 5, 4);
    cn_belief_build(&S, &cfg, &B);
    uint64_t r = 99;
    enum { N = 100000 };
    for (int q = 4; q <= 12; q += 2) {
        int hit = 0;
        for (int i = 0; i < N; i++) {
            int c = 1;                                   /* my 1 counts toward 4s */
            for (int s = 1; s < 4; s++) c += cn_hand_k(cn_belief_sample(&B, s, cn_unit(&r)), 4);
            hit += c >= q;
        }
        double exact = cn_belief_claim(&S, &B, 0, 1, q, 4);
        CHECK(near(exact, (double)hit / N, 0.006), "q %d: exact %.4f sampled %.4f", q, exact, (double)hit / N);
    }
}

static void test_fit(void)
{
    TEST("fitted temperature reads bluffers");
    CnBotCfg cfg;
    cn_bot_cfg_default(&cfg);
    cfg.fit_beta = 1;
    memset(&S, 0, sizeof S);
    S.n = 2;
    S.total = 10;
    S.dice_n[0] = S.dice_n[1] = 5;
    /* six finished rounds: seat 0 bid the face it held, seat 1 bid a face
     * it did not hold at a high quantity */
    for (int r = 0; r < 6; r++) {
        CnSeenRound *rd = &S.rounds[S.nrounds++];
        rd->total = 10;
        rd->dice_n[0] = rd->dice_n[1] = 5;
        const uint8_t h0[5] = { 4, 4, 4, 1, 2 }, h1[5] = { 2, 2, 3, 5, 6 };
        memcpy(rd->shown[0], h0, 5);
        memcpy(rd->shown[1], h1, 5);
        rd->bid0 = S.npast;
        S.past[S.npast++] = (CnSeenBid){ 0, 4, 4, 0, 0 };
        S.past[S.npast++] = (CnSeenBid){ 1, 7, 4, 4, 4 };
        rd->nbids = 2;
    }
    float b0 = cn_fit_beta(&S, 0, &cfg), b1 = cn_fit_beta(&S, 1, &cfg);
    CHECK(b0 >= 8.0f, "an honest bidder keeps a sharp temperature: %g", b0);
    CHECK(b1 <= 2.0f, "a bluffer's bids are read as noise: %g", b1);
    cfg.fit_beta = 0;
    CHECK(cn_fit_beta(&S, 1, &cfg) == cfg.beta, "fit off is the default");
}

/* ---- the decision --------------------------------------------------------------- */

static void test_determinism_and_crn(void)
{
    TEST("rollouts are deterministic and paired");
    CnBotCfg cfg;
    cn_bot_cfg_default(&cfg);
    const uint8_t my[5] = { 1, 3, 3, 5, 6 };
    seen_start(3, 0, my);
    seen_bid(0, 2, 3);
    seen_bid(1, 3, 3);
    seen_bid(2, 3, 5);
    CnMove c1[CN_BOT_MAX_CANDS], c2[CN_BOT_MAX_CANDS];
    double v1[CN_BOT_MAX_CANDS], v2[CN_BOT_MAX_CANDS];
    int n1 = cn_bot_eval(&S, &cfg, 777, c1, v1, CN_BOT_MAX_CANDS);
    int n2 = cn_bot_eval(&S, &cfg, 777, c2, v2, CN_BOT_MAX_CANDS);
    CHECK(n1 == n2 && n1 > 3, "same candidates: %d %d", n1, n2);
    for (int i = 0; i < n1 && i < n2; i++)
        CHECK(c1[i].q == c2[i].q && c1[i].f == c2[i].f && v1[i] == v2[i], "candidate %d repeats exactly", i);
    CHECK(cn_is_call(c1[0]), "the call is the first candidate");
    int differs = 0;
    cn_bot_eval(&S, &cfg, 778, c2, v2, CN_BOT_MAX_CANDS);
    for (int i = 1; i < n1; i++) differs += v1[i] != v2[i];
    CHECK(differs > 0, "another seed samples other worlds");

    /* COMMON RANDOM NUMBERS: a candidate listed twice, or in another
     * position, gets the identical estimate, because every candidate sees
     * the same worlds and the same rollout stream */
    CnMove l[4] = { { 4, 3 }, { 4, 3 }, { 4, 5 }, { 3, 6 } };
    CnMove r[4] = { { 3, 6 }, { 4, 5 }, { 4, 3 }, { 4, 3 } };
    double vl[4], vr[4];
    cn_bot_eval_list(&S, &cfg, 31337, l, 4, vl);
    cn_bot_eval_list(&S, &cfg, 31337, r, 4, vr);
    CHECK(vl[0] == vl[1], "a duplicate candidate: %.6f vs %.6f", vl[0], vl[1]);
    CHECK(vl[0] == vr[2] && vl[0] == vr[3] && vl[2] == vr[1] && vl[3] == vr[0],
          "order does not change an estimate: %.6f %.6f %.6f", vl[0], vr[2], vr[3]);

    /* the call's value is the closed form, no sampling */
    CnMove call[1] = { { 0, 0 } };
    double vc;
    cn_bot_eval_list(&S, &cfg, 1, call, 1, &vc);
    cn_belief_build(&S, &cfg, &B);
    CHECK(near(vc, 1.0 - cn_belief_claim(&S, &B, 0, 2, 3, 5), 1e-12), "call value %.6f", vc);
}

static void test_obvious(void)
{
    TEST("obvious decisions");
    CnBotCfg cfg;
    cn_bot_cfg_default(&cfg);
    /* a bid of ten 5s among ten dice when I hold no 5 and no 1: call */
    const uint8_t my[5] = { 2, 2, 3, 4, 6 };
    seen_start(2, 0, my);
    seen_bid(1, 10, 5);
    CnMove m = cn_bot_choose(&S, &cfg, 5);
    CHECK(cn_is_call(m), "calls an impossible bid, got (%d, %d)", m.q, m.f);
    /* a bid of one 2 when I hold four 2s: never call */
    const uint8_t my2[5] = { 2, 2, 2, 1, 6 };
    seen_start(2, 0, my2);
    seen_bid(1, 1, 2);
    m = cn_bot_choose(&S, &cfg, 5);
    CHECK(!cn_is_call(m), "does not call a certain bid");
}

/* the bot plays whole games through the kernel: every move legal, and what
 * it sees is what the kernel says it may */
static void test_games(void)
{
    TEST("whole games");
    static CnGame g;
    static CnSeen sn;
    CnBotCfg cfg;
    cn_bot_cfg_default(&cfg);
    cfg.worlds = 16;
    uint64_t x = 42;
    for (int gi = 0; gi < 24; gi++) {
        uint8_t seed[32];
        for (int i = 0; i < 32; i++) seed[i] = (uint8_t)cn_splitmix(&x);
        int n = 2 + gi % 5;
        cn_new(&g, seed, n);
        int calls = 0;
        while (g.phase != CN_PH_OVER) {
            int s = g.turn;
            CnMove m;
            if (s % 2 == 0) {
                CHECK(cn_seen(&g, s, &sn), "seen replays");
                CHECK(sn.nrounds == calls, "one finished round per call: %d %d", sn.nrounds, calls);
                CHECK(sn.bid_q == g.bid_q && sn.bid_f == g.bid_f && sn.total == g.total, "the standing bid");
                CHECK(!memcmp(sn.my_dice, g.dice[s], g.dice_n[s]), "my own dice");
                if (sn.nbids) CHECK(sn.bids[sn.nbids - 1].seat == g.bidder, "the last bid's seat");
                if (calls) CHECK(!memcmp(sn.rounds[calls - 1].shown, g.shown, sizeof g.shown), "the shown hands");
                m = cn_bot_choose(&sn, &cfg, cn_splitmix(&x));
            } else {
                m = cn_random_choose(&g, &x);
            }
            CHECK(cn_apply(&g, s, m), "a legal move from seat %d: (%d, %d)", s, m.q, m.f);
            calls += cn_is_call(m);
            if (g_fails) return;
        }
    }
}

int main(void)
{
    test_binomial();
    test_claim();
    test_likelihood();
    test_belief_direction();
    test_sampling();
    test_belief_claim();
    test_fit();
    test_determinism_and_crn();
    test_obvious();
    test_games();
    printf("cn_bot_test: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
