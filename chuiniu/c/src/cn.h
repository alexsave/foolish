/* Chui Niu - the rules, and nothing else.
 *
 * chuiniu/docs/DECISIONS.md (R1 onward, K1 onward) is the specification.
 * Every rule a host could ask about is answered here, so a host never
 * re-decides one.
 *
 * THE SHAPE IS PICK 'EM UP'S (pickemup/c/src/pk.h): a fixed-size game struct
 * with no pointers, one legality function the others are written in terms
 * of, apply returns 1 or 0 and leaves the game untouched on 0, and every
 * derived thing (the plan, the envelope's decode) is a replay of the history
 * through the one apply path.
 *
 * ONE MOVE IS ONE BUBBLE. Liar's dice is strictly sequential: the seat on
 * turn either raises the standing bid or calls it, and that is the whole
 * bubble. hist[] holds every move since the start and nothing else; the
 * dice are never in it (K2: they are derived, cn_dice.c).
 *
 * NO DYNAMIC ALLOCATION AND NO LIBC beyond memcpy / memset / memcmp: the
 * same files build for wasm32 (-nostdlib, shared/c/wasm) and into an
 * xcframework. */
#ifndef CN_H
#define CN_H

#include <stdint.h>

/* ---- the table (R1, R2) --------------------------------------------------- */

#define CN_MIN_SEATS   2
#define CN_MAX_SEATS   6
#define CN_START_DICE  5              /* dice each seat starts with (R1)       */
#define CN_FACES       6
#define CN_WILD        1              /* 1s count toward every face (R2)       */
#define CN_FACE_LO     2              /* the lowest face a bid names (R2)      */
#define CN_BID_FACES   5              /* faces 2..6                            */
#define CN_MAX_DICE    (CN_MAX_SEATS * CN_START_DICE)
#define CN_SEAT_NONE   0xFF

/* A BID'S RANK is its place in the one order R2 defines: quantity first,
 * then face. (q, f) is higher than (q', f') exactly when its rank is. */
static inline int cn_rank(int q, int f) { return (q - 1) * CN_BID_FACES + (f - CN_FACE_LO); }

/* THE LONGEST GAME, and why every game is at most this many moves. Every
 * round loses exactly one die (R3), so a game of n seats plays at most
 * 5n - 1 rounds, with D = 5n, 5n - 1, ..., 2 dice on the table. A round with
 * D dice holds at most 5D bids (every rank once, R7 caps the quantity at D)
 * and one call. Summed: 5 (T(5n) - 1) + (5n - 1), T the triangular number.
 * At six seats, 2,349. */
#define CN_TRI(m)          ((m) * ((m) + 1) / 2)
#define CN_MAX_MOVES_OF(n) (CN_BID_FACES * (CN_TRI((n) * CN_START_DICE) - 1) + (n) * CN_START_DICE - 1)
#define CN_MAX_MOVES       CN_MAX_MOVES_OF(CN_MAX_SEATS)
#define CN_MAX_ROUNDS      (CN_MAX_DICE - 1)

/* ---- moves ------------------------------------------------------------------ */

/* q 1..dice on the table and f 2..6 is a bid; q 0 is the call (f 0). */
typedef struct { uint8_t q, f; } CnMove;

static inline int cn_is_call(CnMove m) { return m.q == 0; }

/* The game's phase once started. The lobby is the envelope's (cn_msg.h). */
enum {
    CN_PH_BIDDING = 1,     /* a round is open and its opening bid is made        */
    CN_PH_REVEALED,        /* the newest move was a call; the next round is rolled
                              and its opener has not bid yet (R4)                */
    CN_PH_OVER,            /* one seat holds dice                                */
};
/* A round that has just opened with no call before it (the first) reads as
 * BIDDING with no standing bid: bid_q 0. */

typedef struct {
    uint8_t  n;                          /* seats, 2..6                              */
    uint8_t  phase;                      /* CN_PH_*                                  */
    uint8_t  round;                      /* the current round, 0-based               */
    uint8_t  turn;                       /* whose move, CN_SEAT_NONE when over       */
    uint8_t  bid_q, bid_f;               /* the standing bid; 0 0 at a round's open  */
    uint8_t  bidder;                     /* who made it, or CN_SEAT_NONE             */
    uint8_t  winner;                     /* CN_SEAT_NONE until over                  */
    uint8_t  total;                      /* dice on the table                        */
    uint8_t  last_seat;                  /* who made the newest move, or NONE        */
    uint8_t  dice_n[CN_MAX_SEATS];
    /* this round's dice in roll order (cn_dice.c). KERNEL-ONLY: another
     * seat's dice are never a display value while the round is open;
     * cn_view is what a host reads. */
    uint8_t  dice[CN_MAX_SEATS][CN_START_DICE];

    /* the newest call, what REVEALED and OVER show: the dice as they stood
     * when it was made, every seat's */
    uint8_t  call_seat;                  /* the caller, or CN_SEAT_NONE (no call yet) */
    uint8_t  call_bidder;
    uint8_t  call_q, call_f;
    uint8_t  call_count;                 /* dice showing call_f or a 1               */
    uint8_t  call_loser;
    uint8_t  shown_n[CN_MAX_SEATS];
    uint8_t  shown[CN_MAX_SEATS][CN_START_DICE];

    uint16_t call_at;                    /* the newest call's move number (1-based), 0 none */
    uint16_t round_at;                   /* hist index where the current round opened */
    uint16_t hist_n;                     /* moves so far                              */
    CnMove   hist[CN_MAX_MOVES];
    uint8_t  seed[32];
} CnGame;

/* ---- the start ------------------------------------------------------------------
 *
 * Every seat at five dice, round 0 rolled from the seed (K2), seat 0 on turn
 * (R4). Depends on the seed and `n` and nothing else. 1, or 0 for an `n`
 * out of 2..6. */
int  cn_new(CnGame *g, const uint8_t seed[32], int n);

/* ---- legality (R2, R3, R5, R7) ----------------------------------------------------
 *
 * Everything the seat on turn may do now. THE ORDER IS PART OF THE FORMAT
 * (the body's digit is an index into it): the call first when it is legal,
 * then every legal bid by ascending rank. Returns the count; never more than
 * `cap` written (`out` may be NULL to count). */
int  cn_legal(const CnGame *g, CnMove *out, int cap);
int  cn_is_legal(const CnGame *g, int seat, CnMove m);

/* The lowest bid the seat on turn may make: 1 and (q, f), or 0 when there is
 * none (the standing bid is the top one, only the call is left). */
int  cn_min_raise(const CnGame *g, int *q, int *f);

/* The least quantity the seat on turn may bid on face `f` (2..6), or 0
 * when no bid on that face is legal (R2, R7). The bid picker's table: a
 * host never ranks two bids (DECISIONS I3). */
int  cn_min_quantity(const CnGame *g, int f);

/* May the seat on turn call? Never on a round's opening bid (R3, R5). */
int  cn_can_call(const CnGame *g);

/* ---- apply and replay --------------------------------------------------------------- */

int  cn_apply(CnGame *g, int seat, CnMove m);     /* 1 applied, 0 refused (g untouched) */

/* Rebuild `out` from `g`'s seed and seat count through hist[0..k). 1, or 0
 * when the history does not replay. */
int  cn_replay(CnGame *out, const CnGame *g, int k);

/* A hash of everything that is the game and nothing that is not (the unused
 * tail of hist). */
uint64_t cn_hash(const CnGame *g);

/* ---- small helpers ------------------------------------------------------------------ */

/* The next seat after `from` that still holds dice, in seat order. */
int  cn_next_live(const CnGame *g, int from);
/* Dice on the table showing `f` or a wild 1 (R2). */
int  cn_count(const CnGame *g, int f);
/* Seats still holding dice. */
int  cn_live_seats(const CnGame *g);

/* ---- the dice (K2, K3; cn_dice.c) ----------------------------------------------------- */

/* SHA-256("chuiniu.log.1|" || q0 f0 q1 f1 ... ) over hist[0..k). */
void cn_log_digest(const CnMove *hist, int k, uint8_t out[32]);
/* SHA-256("chuiniu.dice.1|" || seed || round as u16 little-endian || log). */
void cn_round_key(const uint8_t seed[32], int round, const uint8_t log[32], uint8_t out[32]);
/* Seat `seat`'s first `count` dice of the round keyed `key`: deal_rng at
 * block `seat` << 32, one bounded(6) draw per die in order, plus 1. */
void cn_roll(const uint8_t key[32], int seat, int count, uint8_t *out);
/* Every live seat's dice for g's current round, from g's own state. */
void cn_roll_round(CnGame *g);

#endif
