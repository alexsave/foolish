/* Tallybones - the rules, and nothing else.
 *
 * tallybones/docs/DECISIONS.md is the specification (T2 to T7, T11). Every
 * rule a host could ask about is answered here, so a host never re-decides
 * one: which categories a seat may still score, what a category scores with
 * five dice, whose turn it is, whether a reroll is left, when the game ends
 * and who won.
 *
 * THE SHAPE IS PICK 'EM UP'S (pickemup/c/src/pk.h): a fixed-size game struct
 * with no pointers, one menu the other legality questions are written in
 * terms of, and the history as the only thing that is stored.
 *
 * ONE MOVE IS ONE BUBBLE. A turn is up to three bubbles from the same seat in
 * a row: [KEEP] [KEEP] SCORE (T3, T11). The only other move is LEAVE, which
 * any seat still in the game may send at any time (T5).
 *
 * THE DICE ARE NEVER STORED OR SENT. Every die is derived from the seed and
 * the encoded history (T6, T11): roll 1 of a turn from the history as it
 * stood when the turn began, a reroll from the history THROUGH the KEEP that
 * committed to it. The derivation lives in one place (tb.c, `roll`), and it
 * runs only while a RESIDENT history is replayed (tb_new, tb_replay, the
 * envelope's decode). A DRAFT is the resident game plus one pending move,
 * applied with nothing derived: the dice it rerolls read 0, unknown, and no
 * function in this file or in tb_api.h returns values for it.
 *
 * NO DYNAMIC ALLOCATION AND NO LIBC beyond memcpy / memset / memcmp: the same
 * files build for wasm32 (-nostdlib, shared/c/wasm) and into an xcframework. */
#ifndef TB_H
#define TB_H

#include <stdint.h>

#define TB_MAX_SEATS   8
#define TB_MIN_SEATS   2
#define TB_DICE        5
#define TB_FACES       6
#define TB_CATS        13
#define TB_ROLLS       3            /* rolls a turn may take                   */
#define TB_SEAT_NONE   0xFF
#define TB_ALL_KEPT    31           /* keeping all five is not a reroll (T7)  */
#define TB_FULL_CARD   0x1FFF       /* all 13 categories filled               */

/* ---- the scorecard (T4) --------------------------------------------------
 *
 * THE NUMBERING IS PART OF THE FORMAT: a SCORE digit is an index into the
 * seat's open categories in this order, so renumbering a category decodes
 * every existing game differently. */
enum {
    TB_C_ONES = 0, TB_C_TWOS, TB_C_THREES, TB_C_FOURS, TB_C_FIVES, TB_C_SIXES,
    TB_C_THREE_ALIKE,       /* three matching: the sum of all five        */
    TB_C_FOUR_ALIKE,        /* four matching: the sum of all five         */
    TB_C_FULL_HOUSE,        /* three of one and two of another: 25        */
    TB_C_SHORT_RUN,         /* four in sequence: 30                       */
    TB_C_LONG_RUN,          /* five in sequence: 40                       */
    TB_C_TALLYBONES,        /* five matching: 50                          */
    TB_C_ANY,               /* no requirement: the sum of all five        */
};
#define TB_UPPER_CATS   6
#define TB_BONUS_AT     63          /* a numbers half of 63 or more ...          */
#define TB_BONUS        35          /* ... earns 35                              */
#define TB_FULL_HOUSE   25
#define TB_SHORT_RUN    30
#define TB_LONG_RUN     40
#define TB_TALLYBONES   50

/* What `cat` scores with these five dice (each 1..6); 0 when they do not
 * satisfy it, which is how a bad turn is spent (T4). -1 for a die off 1..6
 * or a category off the card. Pure: no game, no seed. */
int tb_score_of(const uint8_t dice[TB_DICE], int cat);

/* ---- moves ---------------------------------------------------------------
 *
 * THE MOVE ALPHABET HAS NO VALUE FIELD (T11.3): a KEEP carries a 5-bit mask
 * of dice POSITIONS, a SCORE a category, a LEAVE nothing but its seat. A
 * history that scores dice other than the derived ones cannot be written. */
enum { TB_M_NONE = 0, TB_M_KEEP = 1, TB_M_SCORE, TB_M_LEAVE };
typedef struct { uint8_t kind, seat, arg, pad0; } TbMove;
_Static_assert(sizeof(TbMove) == 4, "a move is kind, seat, one argument, nothing else");

/* ---- the table ------------------------------------------------------------ */

/* 13 turns a seat, at most three bubbles a turn, and a leave for every seat
 * that can go before the game ends (the last two cannot both leave). */
#define TB_MAX_BUBBLES (TB_MAX_SEATS * TB_CATS * TB_ROLLS + TB_MAX_SEATS - 1)   /* 319 */
#define TB_HIST_CAP    TB_MAX_BUBBLES

typedef struct {
    uint8_t  n;                          /* seats, 2..8                              */
    uint8_t  turn;                       /* whose turn, TB_SEAT_NONE once over       */
    uint8_t  roll;                       /* rolls made this turn, 1..3               */
    uint8_t  over;                       /* 1 once the game has ended                */
    uint8_t  left;                       /* bit s: seat s left (T5)                  */
    uint8_t  starter;                    /* the seat that started it                 */
    uint8_t  kept;                       /* the newest KEEP of this turn, 0 at roll 1 */
    uint8_t  draft;                      /* 1: `pending` is applied, nothing derived  */
    uint8_t  dice[TB_DICE];              /* 1..6, 0 unknown (a draft's rerolls)      */
    uint8_t  pad0[3];
    uint16_t turns;                      /* completed turns: the index of this one   */
    uint16_t hist_n;                     /* resident moves (a draft's is not in it)  */
    uint16_t filled[TB_MAX_SEATS];       /* bit c: category c is scored               */
    uint8_t  score[TB_MAX_SEATS][TB_CATS];
    uint8_t  pad1[3];
    TbMove   pending;                    /* the draft's move, when draft             */
    TbMove   hist[TB_HIST_CAP];
    uint8_t  seed[32];
} TbGame;

/* A new game at `n` seats (2..8) started by `starter`, seat 0 to roll first,
 * with roll 1 of turn 0 derived from the seed and the empty history. 1, or 0
 * for an `n` or a starter out of range. */
int tb_new(TbGame *g, const uint8_t seed[32], int n, int starter);

/* ---- legality -------------------------------------------------------------
 *
 * Every move there is now. THE ORDER IS PART OF THE FORMAT (tb_code.h): the
 * turn seat's KEEP 0..30 while a roll is left, then its SCORE of each open
 * category ascending, then a LEAVE for each seat still in, ascending. It
 * depends on no die, so the wire cannot either. The count; never more than
 * `cap` written. 0 once over, or for a draft. */
#define TB_MENU_MAX (TB_ALL_KEPT + TB_CATS + TB_MAX_SEATS)      /* 52 */
int tb_menu(const TbGame *g, TbMove *out, int cap);
int tb_is_legal(const TbGame *g, TbMove m);

/* Is seat s still in the game (seated, not left)? */
int tb_is_in(const TbGame *g, int s);

/* ---- the draft (T11) -------------------------------------------------------
 *
 * `out` = `g` with `m` applied as a DRAFT: its effects on the card and the
 * turn are there, and every die it rolls reads 0, because nothing is
 * derived. `g` must be resident (not a draft) and `m` legal. 1, or 0 with
 * `out` untouched. The only way a draft's dice become values is to send it:
 * the envelope's decode is the resident replay. */
int tb_draft(TbGame *out, const TbGame *g, TbMove m);

/* ---- the card -------------------------------------------------------------- */

int tb_upper(const TbGame *g, int s);       /* the numbers half                */
int tb_bonus(const TbGame *g, int s);       /* 35 or 0                         */
/* The bonus can no longer change: it is earned, or every numbers row is
 * filled short of 63. The card shows the bonus line open until then. */
int tb_bonus_known(const TbGame *g, int s);
int tb_total(const TbGame *g, int s);       /* both halves and the bonus       */

/* The winners once over: bit s for every seat still in with the highest
 * total (a tie is shared, T5). 0 while the game is live. */
int tb_winners(const TbGame *g);

/* A hash of everything that is the game and nothing that is not. */
uint64_t tb_hash(const TbGame *g);

#endif
