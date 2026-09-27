/* Tallybones - the exact single-player solver ("the bot").
 *
 * An internal simulation and evaluation tool: it is not in the app, not in
 * the lobby, not in any Swift. tallybones/docs/DECISIONS.md T30 onwards.
 *
 * THE GAME HAS NO INTERACTION BETWEEN SEATS (T4, T5), so the bot models no
 * opponent: it maximises the expected value of its own final score, and it
 * does so EXACTLY, by backward induction over every state, with every
 * expectation taken over a combinatorial distribution. No randomness is used
 * anywhere in the solver; the only RNG in this module is the one that plays
 * simulated games (tb_bot_play), and the solver never calls it.
 *
 * SCORING IS THE KERNEL'S: every category value comes from tb_score_of
 * (tallybones/c/src/tb.c), and the bonus from TB_BONUS_AT / TB_BONUS, so the
 * bot and the game cannot disagree about what a hand is worth.
 *
 * THE STATES.
 *   Between turns: (remaining categories, a 13-bit mask; the numbers-half
 *   total so far, capped at 63). The table `ev` holds, for every such state,
 *   the expected points STILL TO COME under optimal play (the bonus counted
 *   at the moment the numbers half first reaches 63). 2^13 x 64 doubles.
 *   Within a turn: (the dice multiset in hand, one of 252; rolls left 0..2).
 *
 * THE DISTRIBUTIONS. Keeping is by position, but only the kept multiset
 * matters, so every keep is collapsed to one of the 462 sub-multisets of
 * size 0..5. For each, the 6^r raw outcomes of the r rerolled dice are
 * enumerated and collapsed by resulting hand into integer counts over the
 * denominator 6^r: 4368 (keep, hand) entries in all.
 *
 * KEEPING ALL FIVE IS STOPPING. The kernel has no KEEP(31) (T7): a player
 * who likes the dice scores them. So the option "keep all five" is valued
 * as scoring now, which is exactly as good as deferring (more rolls never
 * lower a value), and the policy then returns the category. */
#ifndef TB_BOT_H
#define TB_BOT_H

#include <stdint.h>
#include "../src/tb.h"

#define TB_BOT_HANDS  252            /* multisets of 5 dice from 6 faces    */
#define TB_BOT_KEEPS  462            /* multisets of 0..5 dice              */
#define TB_BOT_ENTRY  4368           /* (keep, resulting hand) pairs        */
#define TB_BOT_MASKS  (1 << TB_CATS) /* 8192 remaining-category masks       */
#define TB_BOT_UPPER  (TB_BONUS_AT + 1)  /* upper totals 0..63, 63 = "made it" */

typedef struct {
    /* ---- the combinatorics, built once by tb_bot_build ---- */
    uint8_t  hand[TB_BOT_HANDS][TB_DICE];      /* each hand's dice, ascending      */
    int16_t  hand_keep[TB_BOT_HANDS];          /* the hand as a keep index         */
    int8_t   score[TB_BOT_HANDS][TB_CATS];     /* tb_score_of, tabulated           */
    uint8_t  keep_n[TB_BOT_KEEPS];             /* dice kept                        */
    uint8_t  keep_c[TB_BOT_KEEPS][TB_FACES];   /* kept dice of each face 1..6      */
    int16_t  keep_hand[TB_BOT_KEEPS];          /* the hand, when 5 kept, else -1   */
    int16_t  keep_sub[TB_BOT_KEEPS][TB_FACES]; /* the keep minus one face f+1, -1  */
    uint16_t out_at[TB_BOT_KEEPS + 1];         /* keep k's entries: [out_at[k], out_at[k+1]) */
    uint8_t  out_hand[TB_BOT_ENTRY];           /* the resulting hand               */
    uint16_t out_count[TB_BOT_ENTRY];          /* raw outcomes giving it, over 6^r */
    int16_t  key_keep[6 * 6 * 6 * 6 * 6 * 6];  /* base-6 face counts -> keep index */
    /* ---- the between-turns table ---- */
    double   ev[TB_BOT_MASKS][TB_BOT_UPPER];
} TbBot;

/* One turn's tables for one between-turns state: v[r][h] is the value of
 * holding hand h with r rolls left; ek[r][k] the value of keeping keep k and
 * rerolling the rest with r rolls left (ek[r] of a full keep = v[0]: stop). */
typedef struct {
    int    rem, upper;
    double v[TB_ROLLS][TB_BOT_HANDS];
    double ek[TB_ROLLS][TB_BOT_KEEPS];
} TbBotTurn;

/* Build every table (the combinatorics, then the whole induction). `threads`
 * workers solve the masks of one level at once (1 = no threads); the result
 * is bit-identical for every thread count. 1, or 0 on a thread failure. */
int tb_bot_build(TbBot *b, int threads);

/* The expected points still to come from a between-turns state. `rem` is
 * the categories NOT yet scored; `upper` the numbers half so far (any value;
 * 63 and above are the same state). */
double tb_bot_value(const TbBot *b, int rem, int upper);

/* One turn's tables (tb_bot_build calls exactly this for every state). */
void tb_bot_turn(const TbBot *b, int rem, int upper, TbBotTurn *t);

/* THE POLICY. For the dice in hand (by position, each 1..6) and the rolls
 * left (0..2, = TB_ROLLS - TbGame.roll), the optimal move in the kernel's
 * alphabet: TB_M_KEEP with arg = the positions to KEEP (bit i = die i, 0..30,
 * the unmarked dice reroll), or TB_M_SCORE with arg = the category. `ev` is
 * the exact expected points still to come, this turn's included. Ties go to
 * scoring now, then to the lowest mask, then to the lowest category. */
typedef struct { int kind, arg; double ev; } TbBotChoice;
TbBotChoice tb_bot_choose(const TbBot *b, const TbBotTurn *t, const uint8_t dice[TB_DICE], int rolls_left);

/* The best category for five dice at (rem, upper), and its value: points
 * now plus the bonus if this crosses 63 plus ev[] of the state after. */
double tb_bot_best_cat(const TbBot *b, int rem, int upper, int hand, int *cat);

/* The hand index of five dice (any order), -1 for a die off 1..6. */
int tb_bot_hand(const TbBot *b, const uint8_t dice[TB_DICE]);
/* The keep index of the dice at the positions in `mask`. */
int tb_bot_keep(const TbBot *b, const uint8_t dice[TB_DICE], int mask);

/* THE SIMULATION. One game played by the policy with deal_rng
 * (shared/c/deal_rng.h, ChaCha): game g's stream is seeded with `seed` whose
 * bytes 24..31 are replaced by g little-endian, and every die is
 * 1 + deal_rng_bounded(r, 6), rolled in position order. The final score is
 * totalled independently of the solver: the kernel's tb_score_of per
 * category, plus 35 when the six numbers categories sum to 63 or more. */
int tb_bot_play(const TbBot *b, const uint8_t seed[32], uint64_t g, TbBotTurn *scratch);

#define TB_BOT_MAX_SCORE 400
typedef struct {
    uint64_t n, sum, sumsq;
    uint32_t hist[TB_BOT_MAX_SCORE];
} TbBotSim;
/* Games 0..n-1 over `threads` workers; the tallies are integers, so the
 * result is the same for every thread count. 1, or 0 on a thread failure. */
int tb_bot_simulate(const TbBot *b, const uint8_t seed[32], uint64_t n, int threads, TbBotSim *out);

#endif
