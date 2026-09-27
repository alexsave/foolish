/* Chui Niu - the belief bot. See cn_bot.h and chuiniu/docs/BOT.md.
 *
 * Tables are _Thread_local and built on first use, so the arena may run
 * games on several threads with no shared mutable state. */
#include "cn_bot.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

const float cn_beta_grid[CN_BETA_GRID] = { 0.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f };

/* The log-likelihood bonus the default temperature gets in the fit, so a
 * seat with one or two bids on record keeps the default unless its bids
 * clearly say otherwise (one nat: the data must be e times likelier). */
#define CN_FIT_PRIOR_NATS 1.0

/* The "comfortable" raise candidate on each face is the highest quantity the
 * belief still makes at least this likely (closed form, cn_belief_claim). */
#define CN_RAISE_SAFE 0.5

void cn_bot_cfg_default(CnBotCfg *c)
{
    c->worlds = 96;
    c->beta = 8.0f;
    c->fit_beta = 0;         /* measured: fitting lost to a fixed 8 (BOT.md) */
    c->use_belief = 1;
    c->ro_beta = 8.0f;
    c->observe = 2;          /* level 1: I read the rollout's bids, nobody reads mine */
    c->opp_reads = 0;        /* ... and rollout opponents judge by the prior */
    c->rollout_steps = 64;
}

/* ---- the binomial ------------------------------------------------------------ */

#define M_MAX CN_MAX_DICE
static _Thread_local int    t_binom_ready;
static _Thread_local double t_pmf[M_MAX + 1][M_MAX + 1];
static _Thread_local double t_tail[M_MAX + 1][M_MAX + 2];   /* t_tail[m][k] = P(X >= k), k 0..m+1 */

static void binom_init(void)
{
    if (t_binom_ready) return;
    for (int m = 0; m <= M_MAX; m++) {
        /* C(m, k) (1/3)^k (2/3)^(m-k), by the recurrence on k */
        double p = pow(2.0 / 3.0, m);
        for (int k = 0; k <= M_MAX; k++) {
            t_pmf[m][k] = k <= m ? p : 0.0;
            if (k < m) p = p * (double)(m - k) / (double)(k + 1) * 0.5;
        }
        double acc = 0.0;
        t_tail[m][m + 1] = 0.0;
        for (int k = m; k >= 0; k--) { acc += t_pmf[m][k]; t_tail[m][k] = acc; }
        if (t_tail[m][0] > 1.0) t_tail[m][0] = 1.0;
    }
    t_binom_ready = 1;
}

double cn_binom_pmf(int m, int k)
{
    binom_init();
    if (m < 0 || m > M_MAX || k < 0 || k > m) return 0.0;
    return t_pmf[m][k];
}

double cn_binom_tail(int m, int need)
{
    binom_init();
    if (need <= 0) return 1.0;
    if (m < 0 || need > m) return 0.0;
    return t_tail[m][need];
}

double cn_binom_tail_p(int U, int m, double p)
{
    if (m <= 0) return 1.0;
    if (U < 0 || m > U || U > CN_MAX_DICE) return 0.0;
    double t[CN_MAX_DICE + 1];
    /* term i = C(U, i) p^i (1 - p)^(U - i), walked up from i = 0 by the ratio
     * (U - i) / (i + 1) * p / (1 - p); summed from the top down so the small
     * tail terms are not lost against the large ones */
    if (p <= 0.0) return 0.0;
    if (p >= 1.0) return 1.0;
    double x = 1.0;
    for (int i = 0; i < U; i++) x *= 1.0 - p;
    t[0] = x;
    for (int i = 0; i < U; i++) t[i + 1] = t[i] * (double)(U - i) / (double)(i + 1) * p / (1.0 - p);
    double acc = 0.0;
    for (int i = U; i >= m; i--) acc += t[i];
    return acc > 1.0 ? 1.0 : acc;
}

double cn_claim_prob(int total, int d, int k, int q, double p)
{
    return cn_binom_tail_p(total - d, q - k, p);
}

/* ---- hands as multisets --------------------------------------------------------- */

static _Thread_local int    t_hands_ready;
static _Thread_local int    t_nh[CN_START_DICE + 1];
static _Thread_local CnHand t_hands[CN_START_DICE + 1][CN_HANDS_MAX];
static _Thread_local double t_prior[CN_START_DICE + 1][CN_HANDS_MAX];

static void hands_init(void)
{
    if (t_hands_ready) return;
    static const double fact[] = { 1, 1, 2, 6, 24, 120 };
    for (int n = 0; n <= CN_START_DICE; n++) {
        int k = 0;
        /* c1..c6 >= 0 summing to n, c1 slowest */
        int c[7] = { 0 };
        for (c[1] = 0; c[1] <= n; c[1]++)
        for (c[2] = 0; c[1] + c[2] <= n; c[2]++)
        for (c[3] = 0; c[1] + c[2] + c[3] <= n; c[3]++)
        for (c[4] = 0; c[1] + c[2] + c[3] + c[4] <= n; c[4]++)
        for (c[5] = 0; c[1] + c[2] + c[3] + c[4] + c[5] <= n; c[5]++) {
            c[6] = n - c[1] - c[2] - c[3] - c[4] - c[5];
            CnHand *h = &t_hands[n][k];
            memset(h, 0, sizeof *h);
            double w = fact[n];
            for (int f = 1; f <= 6; f++) { h->c[f] = (uint8_t)c[f]; w /= fact[c[f]]; }
            t_prior[n][k] = w / pow(6.0, n);
            k++;
        }
        t_nh[n] = k;
    }
    t_hands_ready = 1;
}

int cn_hands(int n, const CnHand **hands, const double **prior)
{
    hands_init();
    if (n < 0 || n > CN_START_DICE) return 0;
    if (hands) *hands = t_hands[n];
    if (prior) *prior = t_prior[n];
    return t_nh[n];
}

/* ---- the opponent model: one bid's likelihood ------------------------------------
 *
 * THE FAST FORM. u(raise (Q, F)) depends on Q and the hand only through
 * need = Q - k_h(F), so for a seat with m unseen dice and temperature beta,
 * E[need] = exp(beta * tail(m, need)) is one table, and the sum of E over a
 * run of quantities on one face is a difference of its running sum. Z(h) is
 * then five differences and the call term: O(1) per hand, not O(options). */

#define NEED_LO (-CN_START_DICE)                     /* q 0 with k 5 */
#define NEED_N  (CN_MAX_DICE - NEED_LO + 2)

typedef struct {
    int    m;
    float  beta;
    double e[NEED_N];        /* e[need - NEED_LO]                      */
    double cum[NEED_N + 1];  /* cum[i] = e[0] + ... + e[i - 1]         */
} LikTab;

static void liktab_init(LikTab *t, int m, float beta)
{
    t->m = m;
    t->beta = beta;
    t->cum[0] = 0.0;
    for (int i = 0; i < NEED_N; i++) {
        t->e[i] = exp((double)beta * cn_binom_tail(m, i + NEED_LO));
        t->cum[i + 1] = t->cum[i] + t->e[i];
    }
}

static inline double lt_e(const LikTab *t, int need)
{
    int i = need - NEED_LO;
    if (i < 0) i = 0;
    if (i >= NEED_N) i = NEED_N - 1;
    return t->e[i];
}

/* sum of e over need lo..hi inclusive */
static inline double lt_sum(const LikTab *t, int lo, int hi)
{
    if (hi < lo) return 0.0;
    int a = lo - NEED_LO, b = hi - NEED_LO + 1;
    if (a < 0) a = 0;
    if (b > NEED_N) b = NEED_N;
    return t->cum[b] - t->cum[a];
}

static inline int min_q_face(int total, int pq, int pf, int f)
{
    int q = pq == 0 ? 1 : f > pf ? pq : pq + 1;
    return q <= total ? q : 0;
}

static double lik_fast(const LikTab *t, const CnHand *h, int total, int pq, int pf, int q, int f)
{
    double z = 0.0;
    if (pq > 0) z += exp((double)t->beta * (1.0 - cn_binom_tail(t->m, pq - cn_hand_k(h, pf))));
    for (int F = CN_FACE_LO; F <= CN_FACES; F++) {
        int lo = min_q_face(total, pq, pf, F);
        if (!lo) continue;
        int k = cn_hand_k(h, F);
        z += lt_sum(t, lo - k, total - k);
    }
    double num = q == 0 ? exp((double)t->beta * (1.0 - cn_binom_tail(t->m, pq - cn_hand_k(h, pf))))
                        : lt_e(t, q - cn_hand_k(h, f));
    return z > 0.0 ? num / z : 0.0;
}

double cn_bid_lik(const CnHand *h, int n, int total, int prev_q, int prev_f, int q, int f, float beta)
{
    LikTab t;
    liktab_init(&t, total - n, beta);
    return lik_fast(&t, h, total, prev_q, prev_f, q, f);
}

/* ---- what a seat saw ------------------------------------------------------------------ */

int cn_seen(const CnGame *g, int me, CnSeen *out)
{
    static _Thread_local CnGame r;
    memset(out, 0, offsetof(CnSeen, rounds));
    out->nrounds = out->npast = 0;
    if (!cn_new(&r, g->seed, g->n)) return 0;
    uint16_t round_bid0 = 0;
    for (int i = 0; i < g->hist_n; i++) {
        CnMove m = g->hist[i];
        int seat = r.turn, pq = r.bid_q, pf = r.bid_f;
        int total = r.total;
        uint8_t dn[CN_MAX_SEATS];
        memcpy(dn, r.dice_n, sizeof dn);
        if (!cn_apply(&r, seat, m)) return 0;
        if (!cn_is_call(m)) {
            out->past[out->npast++] = (CnSeenBid){ (uint8_t)seat, m.q, m.f, (uint8_t)pq, (uint8_t)pf };
            continue;
        }
        /* a call: the round is over and every hand is shown (public) */
        CnSeenRound *rd = &out->rounds[out->nrounds++];
        rd->total = (uint8_t)total;
        memcpy(rd->dice_n, dn, sizeof dn);
        memcpy(rd->shown, r.shown, sizeof rd->shown);
        rd->bid0 = round_bid0;
        rd->nbids = (uint16_t)(out->npast - round_bid0);
        round_bid0 = out->npast;
    }
    /* the open round's bids are the pool's tail after the last call */
    out->nbids = (uint16_t)(out->npast - round_bid0);
    memcpy(out->bids, &out->past[round_bid0], out->nbids * sizeof(CnSeenBid));
    out->npast = round_bid0;

    out->me = (uint8_t)me;
    out->n = g->n;
    out->total = g->total;
    out->turn = g->turn;
    out->bid_q = g->bid_q;
    out->bid_f = g->bid_f;
    out->bidder = g->bidder;
    memcpy(out->dice_n, g->dice_n, sizeof out->dice_n);
    /* the one hidden row a seat may read: its own */
    if (me >= 0 && me < g->n) memcpy(out->my_dice, g->dice[me], sizeof out->my_dice);
    return 1;
}

static void hand_of(const uint8_t *dice, int n, CnHand *h)
{
    memset(h, 0, sizeof *h);
    for (int i = 0; i < n; i++) if (dice[i] >= 1 && dice[i] <= 6) h->c[dice[i]]++;
}

/* ---- fitting a seat's temperature ------------------------------------------------------ */

float cn_fit_beta(const CnSeen *s, int seat, const CnBotCfg *cfg)
{
    if (!cfg->fit_beta) return cfg->beta;
    double ll[CN_BETA_GRID] = { 0 };
    int seen = 0;
    for (int r = 0; r < s->nrounds; r++) {
        const CnSeenRound *rd = &s->rounds[r];
        int n = rd->dice_n[seat];
        if (!n) continue;
        CnHand h;
        hand_of(rd->shown[seat], n, &h);
        for (int b = 0; b < CN_BETA_GRID; b++) {
            LikTab t;
            int built = 0;
            for (int i = 0; i < rd->nbids; i++) {
                const CnSeenBid *x = &s->past[rd->bid0 + i];
                if (x->seat != seat) continue;
                if (!built) { liktab_init(&t, rd->total - n, cn_beta_grid[b]); built = 1; }
                double l = lik_fast(&t, &h, rd->total, x->prev_q, x->prev_f, x->q, x->f);
                ll[b] += log(l > 1e-300 ? l : 1e-300);
                seen += b == 0;
            }
        }
    }
    if (!seen) return cfg->beta;
    int best = -1;
    double bestv = -INFINITY;
    for (int b = 0; b < CN_BETA_GRID; b++) {
        double v = ll[b] + (cn_beta_grid[b] == cfg->beta ? CN_FIT_PRIOR_NATS : 0.0);
        if (v > bestv) { bestv = v; best = b; }
    }
    return cn_beta_grid[best];
}

/* ---- the belief ------------------------------------------------------------------------ */

void cn_belief_build(const CnSeen *s, const CnBotCfg *cfg, CnBelief *b)
{
    memset(b->pk, 0, sizeof b->pk);
    for (int seat = 0; seat < s->n; seat++) {
        int n = s->dice_n[seat];
        const double *prior;
        b->nh[seat] = cn_hands(n, &b->hands[seat], &prior);
        b->beta[seat] = cn_fit_beta(s, seat, cfg);
        double *post = b->post[seat];
        for (int h = 0; h < b->nh[seat]; h++) post[h] = prior[h];
        if (cfg->use_belief && n > 0) {
            LikTab t;
            liktab_init(&t, s->total - n, b->beta[seat]);
            for (int i = 0; i < s->nbids; i++) {
                const CnSeenBid *x = &s->bids[i];
                if (x->seat != seat) continue;
                for (int h = 0; h < b->nh[seat]; h++)
                    post[h] *= lik_fast(&t, &b->hands[seat][h], s->total, x->prev_q, x->prev_f, x->q, x->f);
            }
        }
        double z = 0.0;
        for (int h = 0; h < b->nh[seat]; h++) z += post[h];
        if (!(z > 0.0)) {                                   /* nothing fits: the prior */
            for (int h = 0; h < b->nh[seat]; h++) post[h] = prior[h];
            z = 1.0;
        }
        double acc = 0.0;
        for (int h = 0; h < b->nh[seat]; h++) {
            post[h] /= z;
            acc += post[h];
            b->cdf[seat][h] = acc;
            for (int f = CN_FACE_LO; f <= CN_FACES; f++)
                b->pk[seat][f][cn_hand_k(&b->hands[seat][h], f)] += (float)post[h];
        }
    }
}

const CnHand *cn_belief_sample(const CnBelief *b, int seat, double u)
{
    int lo = 0, hi = b->nh[seat] - 1;
    double x = u * b->cdf[seat][hi];
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (b->cdf[seat][mid] > x) hi = mid; else lo = mid + 1;
    }
    return &b->hands[seat][lo];
}

double cn_belief_claim(const CnSeen *s, const CnBelief *b, int seat, int k, int q, int f)
{
    double d[CN_MAX_DICE + 1] = { 1.0 }, e[CN_MAX_DICE + 1];
    int len = 1;
    for (int o = 0; o < s->n; o++) {
        if (o == seat || !s->dice_n[o]) continue;
        memset(e, 0, sizeof e);
        for (int a = 0; a < len; a++)
            for (int j = 0; j <= s->dice_n[o]; j++) e[a + j] += d[a] * b->pk[o][f][j];
        len += s->dice_n[o];
        memcpy(d, e, sizeof d);
    }
    double acc = 0.0;
    for (int x = q - k < 0 ? 0 : q - k; x < len; x++) acc += d[x];
    return acc > 1.0 ? 1.0 : acc;
}

/* ---- the rollout -------------------------------------------------------------------------
 *
 * A LIGHT ROUND, not a CnGame: a rollout ends at the round's call, so all it
 * needs is the standing bid, whose turn, each seat's counting dice in the
 * sampled world, and the public marginals every seat reads to judge a bid. */

#define G_LEN (CN_MAX_DICE + 2)

typedef struct {
    int     n, total, me;
    uint8_t dn[CN_MAX_SEATS];
    float   beta[CN_MAX_SEATS];
    float   ro_beta;
    /* public: P(seat s holds k counting dice for face f) */
    float   pk[CN_MAX_SEATS][CN_FACES + 1][CN_START_DICE + 1];
    /* my own reading of the others, when only I read rollout bids (observe 2) */
    float   pkm[CN_MAX_SEATS][CN_FACES + 1][CN_START_DICE + 1];
    int     observe;
    /* derived: g[t][f][x] = P(the other seats hold >= x counting dice for f) */
    float   g[CN_MAX_SEATS][CN_FACES + 1][G_LEN];
    uint8_t dirty[CN_MAX_SEATS][CN_FACES + 1];
} Pub;

static void pub_tail(Pub *p, int t, int f)
{
    double d[G_LEN] = { 1.0 }, e[G_LEN];
    int len = 1;                                      /* d[0 .. len) */
    for (int s = 0; s < p->n; s++) {
        if (s == t || !p->dn[s]) continue;
        int ns = p->dn[s];
        memset(e, 0, sizeof e);
        for (int a = 0; a < len; a++)
            for (int k = 0; k <= ns; k++) e[a + k] += d[a] * (t == p->me ? p->pkm : p->pk)[s][f][k];
        len += ns;
        memcpy(d, e, sizeof d);
    }
    double acc = 0.0;
    for (int x = G_LEN - 1; x >= 0; x--) {
        if (x < len) acc += d[x];
        p->g[t][f][x] = (float)acc;
    }
    p->dirty[t][f] = 0;
}

/* P(bid (q, f) is true) as seat t judges it: its own counting dice k plus
 * the others' under the public marginals */
static inline float pub_true(Pub *p, int t, int k, int q, int f)
{
    if (p->dirty[t][f]) pub_tail(p, t, f);
    int x = q - k;
    if (x <= 0) return 1.0f;
    if (x >= G_LEN) return 0.0f;
    return p->g[t][f][x];
}

/* Seat t bid (q, f): every other seat updates its marginal for t's face f by
 * the bid's likelihood without the normaliser (BOT.md: why the rollout's
 * update is the cheap one). */
static void pub_observe(Pub *p, int t, int q, int f)
{
    if (!p->observe) return;
    if (p->observe == 2 && t == p->me) return;           /* nobody else reads my bids */
    float (*pk)[CN_FACES + 1][CN_START_DICE + 1] = p->observe == 2 ? p->pkm : p->pk;
    int n = p->dn[t];
    int m = p->total - n;
    double w[CN_START_DICE + 1], z = 0.0;
    for (int k = 0; k <= n; k++) {
        w[k] = pk[t][f][k] * exp((double)p->beta[t] * cn_binom_tail(m, q - k));
        z += w[k];
    }
    if (!(z > 0.0)) return;
    for (int k = 0; k <= n; k++) pk[t][f][k] = (float)(w[k] / z);
    for (int s = 0; s < p->n; s++)
        if (s != t && (p->observe == 1 || s == p->me)) p->dirty[s][f] = 1;
}

static int next_live(const Pub *p, int from)
{
    for (int i = 1; i <= p->n; i++) {
        int s = (from + i) % p->n;
        if (p->dn[s]) return s;
    }
    return from;
}

/* The rollout policy: the same softmax the belief assumes every bidder
 * plays (cn_bid_lik), over a short list (the call, and for each face its
 * least legal quantity and one more), judged by the seat's own dice and the
 * public marginals. Returns the move. */
static CnMove ro_policy(Pub *p, int t, const uint8_t k[CN_FACES + 1], int bq, int bf, uint64_t *rng)
{
    CnMove opt[1 + 2 * CN_BID_FACES];
    double w[1 + 2 * CN_BID_FACES], z = 0.0;
    int no = 0;
    const double beta = p->ro_beta;
    if (bq > 0) {
        opt[no] = (CnMove){ 0, 0 };
        w[no] = exp(beta * (1.0 - pub_true(p, t, k[bf], bq, bf)));
        z += w[no++];
    }
    for (int f = CN_FACE_LO; f <= CN_FACES; f++) {
        int q0 = min_q_face(p->total, bq, bf, f);
        if (!q0) continue;
        for (int q = q0; q <= q0 + 1 && q <= p->total; q++) {
            opt[no] = (CnMove){ (uint8_t)q, (uint8_t)f };
            w[no] = exp(beta * pub_true(p, t, k[f], q, f));
            z += w[no++];
        }
    }
    double x = cn_unit(rng) * z;
    for (int i = 0; i < no - 1; i++) {
        if (x < w[i]) return opt[i];
        x -= w[i];
    }
    return opt[no - 1];
}

/* One rollout from my candidate move `first` to the round's call, in the
 * world k[][]. 1 when I keep my die. */
static int rollout(Pub *p, const uint8_t k[CN_MAX_SEATS][CN_FACES + 1], int bq, int bf, int bidder,
                   CnMove first, uint64_t rng, int cap)
{
    int t = p->me;
    CnMove m = first;
    for (int step = 0; ; step++) {
        if (cn_is_call(m)) {
            int count = 0;
            for (int s = 0; s < p->n; s++) if (p->dn[s]) count += k[s][bf];
            int loser = count >= bq ? t : bidder;
            return loser != p->me;
        }
        bq = m.q; bf = m.f; bidder = t;
        pub_observe(p, t, bq, bf);
        t = next_live(p, t);
        if (step >= cap || min_q_face(p->total, bq, bf, CN_FACES) == 0) m = (CnMove){ 0, 0 };  /* top bid: only the call */
        else m = ro_policy(p, t, k[t], bq, bf, &rng);
    }
}

/* ---- the decision ---------------------------------------------------------------------------- */

static int add_cand(CnMove *c, int n, int cap, CnMove m)
{
    for (int i = 0; i < n; i++) if (c[i].q == m.q && c[i].f == m.f) return n;
    if (n < cap) c[n++] = m;
    return n;
}

static void pub_build(Pub *p, const CnSeen *s, const CnBelief *b, const CnBotCfg *cfg)
{
    memset(p, 0, sizeof *p);
    p->n = s->n;
    p->total = s->total;
    p->me = s->me;
    memcpy(p->dn, s->dice_n, sizeof p->dn);
    memcpy(p->beta, b->beta, sizeof p->beta);
    p->ro_beta = cfg->ro_beta;
    memcpy(p->pk, b->pk, sizeof p->pk);
    memcpy(p->pkm, b->pk, sizeof p->pkm);
    if (!cfg->opp_reads)
        for (int s2 = 0; s2 < p->n; s2++)
            for (int f = CN_FACE_LO; f <= CN_FACES; f++)
                for (int k = 0; k <= CN_START_DICE; k++)
                    p->pk[s2][f][k] = (float)cn_binom_pmf(p->dn[s2], k);
    p->observe = cfg->observe;
    for (int t = 0; t < p->n; t++)
        for (int f = CN_FACE_LO; f <= CN_FACES; f++)
            if (p->dn[t]) pub_tail(p, t, f);
}

static _Thread_local CnBelief t_belief;

static void eval_worlds(const CnSeen *s, const CnBotCfg *cfg, uint64_t seed, const CnBelief *b,
                        const Pub *base, const CnMove *cands, int nc, double *value)
{
    Pub p;
    CnHand mine;
    hand_of(s->my_dice, s->dice_n[s->me], &mine);
    for (int c = 0; c < nc; c++) value[c] = 0.0;

    uint64_t wseed = seed;
    for (int w = 0; w < cfg->worlds; w++) {
        /* COMMON RANDOM NUMBERS: one world and one rollout stream per w, the
         * same for every candidate, so candidates are compared paired */
        uint64_t world_rng = cn_splitmix(&wseed);
        uint64_t ro_rng = cn_splitmix(&wseed);
        uint8_t k[CN_MAX_SEATS][CN_FACES + 1];
        memset(k, 0, sizeof k);
        for (int st = 0; st < s->n; st++) {
            if (!s->dice_n[st]) continue;
            const CnHand *h = st == s->me ? &mine : cn_belief_sample(b, st, cn_unit(&world_rng));
            for (int f = CN_FACE_LO; f <= CN_FACES; f++) k[st][f] = (uint8_t)cn_hand_k(h, f);
        }
        for (int c = 0; c < nc; c++) {
            if (cn_is_call(cands[c])) continue;              /* exact, below */
            memcpy(&p, base, sizeof p);
            value[c] += rollout(&p, (const uint8_t (*)[CN_FACES + 1])k, s->bid_q, s->bid_f, s->bidder,
                                cands[c], ro_rng, cfg->rollout_steps);
        }
    }
    for (int c = 0; c < nc; c++) value[c] /= cfg->worlds > 0 ? cfg->worlds : 1;
    /* THE CALL NEEDS NO ROLLOUT: it ends the round now, and I keep my die
     * exactly when the bid is false, so its value is the closed form over
     * the same belief the worlds are drawn from (their expectation, with no
     * sampling noise) */
    for (int c = 0; c < nc; c++)
        if (cn_is_call(cands[c]))
            value[c] = 1.0 - cn_belief_claim(s, b, s->me, cn_hand_k(&mine, s->bid_f), s->bid_q, s->bid_f);
}

void cn_bot_eval_list(const CnSeen *s, const CnBotCfg *cfg, uint64_t seed,
                      const CnMove *cands, int nc, double *value)
{
    static _Thread_local Pub base;
    cn_belief_build(s, cfg, &t_belief);
    pub_build(&base, s, &t_belief, cfg);
    eval_worlds(s, cfg, seed, &t_belief, &base, cands, nc, value);
}

int cn_bot_eval(const CnSeen *s, const CnBotCfg *cfg, uint64_t seed, CnMove *cands, double *value, int cap)
{
    static _Thread_local Pub p;
    int nc = 0;
    const int bq = s->bid_q, bf = s->bid_f, total = s->total;
    if (bq > 0) nc = add_cand(cands, nc, cap, (CnMove){ 0, 0 });

    /* my view of each face: my counting dice plus the public belief of the
     * others, for the "most I can claim at even odds" candidate */
    cn_belief_build(s, cfg, &t_belief);
    pub_build(&p, s, &t_belief, cfg);
    CnHand mine;
    hand_of(s->my_dice, s->dice_n[s->me], &mine);

    for (int f = CN_FACE_LO; f <= CN_FACES; f++) {
        int q0 = min_q_face(total, bq, bf, f);
        if (!q0) continue;
        nc = add_cand(cands, nc, cap, (CnMove){ (uint8_t)q0, (uint8_t)f });
        if (q0 + 1 <= total) nc = add_cand(cands, nc, cap, (CnMove){ (uint8_t)(q0 + 1), (uint8_t)f });
        int k = cn_hand_k(&mine, f), qe = 0;
        for (int q = q0; q <= total; q++)
            if (cn_belief_claim(s, &t_belief, s->me, k, q, f) >= CN_RAISE_SAFE) qe = q;
        if (qe > q0 + 1) nc = add_cand(cands, nc, cap, (CnMove){ (uint8_t)qe, (uint8_t)f });
    }
    eval_worlds(s, cfg, seed, &t_belief, &p, cands, nc, value);
    return nc;
}

CnMove cn_bot_choose(const CnSeen *s, const CnBotCfg *cfg, uint64_t seed)
{
    CnMove c[CN_BOT_MAX_CANDS];
    double v[CN_BOT_MAX_CANDS];
    int nc = cn_bot_eval(s, cfg, seed, c, v, CN_BOT_MAX_CANDS);
    int best = 0;
    for (int i = 1; i < nc; i++) if (v[i] > v[best]) best = i;
    return c[best];
}
