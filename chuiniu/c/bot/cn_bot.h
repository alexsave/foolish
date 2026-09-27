/* Chui Niu - a belief-constrained Monte Carlo bot, for offline evaluation only.
 *
 * chuiniu/docs/BOT.md is the design; this header is the contract.
 *
 * NOT SHIPPED. Nothing here is reachable from the iOS bridge or the lobby.
 * It is a C module the arena (cn_arena.c) links next to the kernel, the way
 * foolish's ladder lets bots play each other offline.
 *
 * WHAT THE BOT MAY READ. A bot decides from a CnSeen, never from a CnGame:
 * cn_seen() is the one function that looks inside the kernel's struct, and it
 * copies out only what the seat could see at the table - its own dice, every
 * seat's dice count, every move with the seat that made it, and the dice that
 * were shown at each past call. Everything below it is public by
 * construction.
 *
 * THE PIECES, in the order a decision runs them:
 *   1. cn_fit_beta     - per seat, a bluff temperature fitted to that seat's
 *                        past bids against the hands later shown at the call
 *   2. cn_belief_build - per seat, the exact posterior over its hand (as a
 *                        multiset of faces) given its bids this round
 *   3. cn_bot_eval     - for each candidate move, rollouts to the end of the
 *                        round over sampled worlds, common random numbers
 *   4. cn_bot_choose   - the best candidate
 *
 * Every constant a result depends on is in CnBotCfg, and cn_bot_cfg_default
 * is the one place its values are written. */
#ifndef CN_BOT_H
#define CN_BOT_H

#include "../src/cn.h"
#include <stdint.h>

/* ---- what a seat saw ------------------------------------------------------ */

#define CN_SEEN_MAX_ROUNDS CN_MAX_ROUNDS
#define CN_SEEN_MAX_BIDS   (CN_MAX_DICE * CN_BID_FACES)   /* a round's bids, at most */

typedef struct {
    uint8_t seat, q, f;
    uint8_t prev_q, prev_f;          /* the bid it raised, 0 0 on an opening bid */
} CnSeenBid;

/* One finished round: its bids and the hands shown when it was called. */
typedef struct {
    uint8_t   total;                 /* dice on the table during the round */
    uint8_t   dice_n[CN_MAX_SEATS];
    uint8_t   shown[CN_MAX_SEATS][CN_START_DICE];
    uint16_t  bid0, nbids;           /* its bids in CnSeen.bids[bid0 ..)   */
} CnSeenRound;

typedef struct {
    uint8_t     me, n, total, turn;
    uint8_t     bid_q, bid_f, bidder;
    uint8_t     dice_n[CN_MAX_SEATS];
    uint8_t     my_dice[CN_START_DICE];
    /* this round's bids, oldest first */
    uint16_t    nbids;
    CnSeenBid   bids[CN_SEEN_MAX_BIDS];
    /* the finished rounds, oldest first; their bids share one pool */
    uint16_t    nrounds, npast;
    CnSeenRound rounds[CN_SEEN_MAX_ROUNDS];
    CnSeenBid   past[CN_MAX_MOVES];
} CnSeen;

/* What seat `me` saw of `g`, rebuilt by replaying g's history (the shown
 * hands of past rounds are the reveals at each call). 1, or 0 when g does
 * not replay. */
int cn_seen(const CnGame *g, int me, CnSeen *out);

/* ---- constants ------------------------------------------------------------ */

#define CN_BETA_GRID 6               /* the temperatures cn_fit_beta picks among */

typedef struct {
    int   worlds;                    /* sampled worlds per decision              */
    float beta;                      /* the bluff temperature assumed with no data */
    float ro_beta;                   /* the rollout policy's temperature, every seat */
    int   fit_beta;                  /* 1: fit each seat's temperature from reveals */
    int   use_belief;                /* 0: ablation - every hidden hand from the prior */
    int   observe;                   /* rollout bids: 0 read by nobody, 1 by everyone, 2 by me only */
    int   opp_reads;                 /* 0: rollout opponents judge bids by the prior alone */
    int   rollout_steps;             /* a cap on moves per rollout (a round ends sooner) */
} CnBotCfg;

void cn_bot_cfg_default(CnBotCfg *c);
extern const float cn_beta_grid[CN_BETA_GRID];

/* ---- the binomial ------------------------------------------------------------
 *
 * With 1s wild, each die counts toward a named face with probability 2/6 =
 * 1/3, so the count among m unseen dice is Binomial(m, 1/3). */
double cn_binom_pmf(int m, int k);            /* P(X = k), 0 outside 0..m     */
double cn_binom_tail(int m, int need);        /* P(X >= need), 1 at need <= 0 */

/* THE CLAIM PRIMITIVE, in closed form. A bid of `q` on a face, my own hand of
 * `d` dice with `k` of them counting toward it, `total` dice on the table:
 * U = total - d dice I cannot see, each counting with probability p, and
 * the bid is true when X >= max(0, q - k), X ~ Binomial(U, p):
 *     sum_{i=m}^{U} C(U, i) p^i (1 - p)^(U - i)
 * A direct loop in doubles (U <= 30), for any p; cn_binom_tail is the same
 * sum at p = 1/3, tabled. */
double cn_binom_tail_p(int U, int m, double p);
double cn_claim_prob(int total, int d, int k, int q, double p);

/* ---- a hand as a multiset ----------------------------------------------------
 *
 * The order of a seat's dice never matters, only how many of each face it
 * holds, so a hand of n dice is one of C(n + 5, 5) count vectors (252 at
 * n = 5). A belief is a probability for each. */
#define CN_HANDS_MAX 252

typedef struct {
    uint8_t c[CN_FACES + 1];         /* c[1..6]: dice showing that face          */
} CnHand;

/* Every hand of n dice (0..5) in a fixed order, with its prior probability
 * (multinomial over six equally likely faces). Returns the count. */
int cn_hands(int n, const CnHand **hands, const double **prior);

/* Dice of the hand that count toward face f (2..6): f's plus the wild 1s. */
static inline int cn_hand_k(const CnHand *h, int f) { return h->c[f] + h->c[CN_WILD]; }

/* ---- the opponent model -------------------------------------------------------
 *
 * THE BIDDER'S POLICY, as the bot models it: a seat on turn holding hand h,
 * facing standing bid b among `total` dice, picks each legal option x with
 * probability  exp(beta * u(x)) / Z(h),  where
 *   u(raise (q, f)) = P(the claim is true | h)   = tail(total - n, q - k_h(f))
 *   u(call)         = P(b is false | h)          = 1 - u(raise b)
 * and Z(h) sums exp(beta * u) over every legal option. The likelihood of what
 * the seat did is that probability; cn_bid_lik computes it. */
double cn_bid_lik(const CnHand *h, int n, int total, int prev_q, int prev_f,
                  int q, int f, float beta);

/* The maximum-likelihood temperature for `seat` over its bids in every
 * finished round, scored against the hand it showed at that round's call
 * (the grid cn_beta_grid, a small prior toward cfg->beta). cfg->beta when the
 * seat has not bid yet or fitting is off. */
float cn_fit_beta(const CnSeen *s, int seat, const CnBotCfg *cfg);

typedef struct {
    int      nh[CN_MAX_SEATS];                    /* hands of each seat's size */
    const CnHand *hands[CN_MAX_SEATS];
    double   post[CN_MAX_SEATS][CN_HANDS_MAX];     /* normalised posterior      */
    double   cdf[CN_MAX_SEATS][CN_HANDS_MAX];      /* its running sum, for sampling */
    /* the per-face marginal: pk[s][f][k] = P(seat s holds k dice counting
     * toward face f), f 2..6, k 0..5. What the rollouts read. */
    float    pk[CN_MAX_SEATS][CN_FACES + 1][CN_START_DICE + 1];
    float    beta[CN_MAX_SEATS];
} CnBelief;

/* The belief every seat's bids this round support, EVERY live seat including
 * `me` (the others' view of me is what the rollout opponents read). */
void cn_belief_build(const CnSeen *s, const CnBotCfg *cfg, CnBelief *b);

/* Draw a hand for `seat` from its posterior, given a uniform u in [0, 1). */
const CnHand *cn_belief_sample(const CnBelief *b, int seat, double u);

/* The same claim question with the belief in place of the flat p: seat
 * `seat` holding `k` counting dice for face f judges (q, f) against the
 * other live seats' posterior count distributions b->pk, convolved exactly
 * (each seat is a full distribution over 0..n, not a binomial with a
 * refined p, so no Poisson-binomial is needed). Closed form, no sampling. */
double cn_belief_claim(const CnSeen *s, const CnBelief *b, int seat, int k, int q, int f);

/* ---- the decision -------------------------------------------------------------- */

#define CN_BOT_MAX_CANDS 16

/* The candidates for the seat on turn and each one's estimated value (the
 * chance I do not lose this round's die), on the same sampled worlds with
 * the same rollout random stream (common random numbers). `seed` fixes the
 * whole estimate. Returns the candidate count. */
int cn_bot_eval(const CnSeen *s, const CnBotCfg *cfg, uint64_t seed,
                CnMove *cands, double *value, int cap);

/* Rollout values for an explicit candidate list (the test seam for the
 * common random numbers). */
void cn_bot_eval_list(const CnSeen *s, const CnBotCfg *cfg, uint64_t seed,
                      const CnMove *cands, int nc, double *value);

CnMove cn_bot_choose(const CnSeen *s, const CnBotCfg *cfg, uint64_t seed);

/* ---- the baseline (cn_bot_random.c) --------------------------------------------- */

/* Uniform over every legal option: the call when allowed, and every raise. */
CnMove cn_random_choose(const CnGame *g, uint64_t *rng);

/* ---- small shared helpers --------------------------------------------------------- */

static inline uint64_t cn_splitmix(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static inline double cn_unit(uint64_t *x) { return (double)(cn_splitmix(x) >> 11) * (1.0 / 9007199254740992.0); }

#endif
