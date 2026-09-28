/* Pick 'Em Up - the rules, and nothing else.
 *
 * pickemup/docs/RULES_AND_KERNEL.md is the specification; section numbers
 * below are that file's. Every rule a host could ask about is answered here,
 * so a host never re-decides one.
 *
 * THE SHAPE IS UTTT'S (uttt/c/src/uttt.h): a fixed-size game struct with no
 * pointers, one legality function the others are written in terms of, apply
 * returns 1 or 0 and leaves the game untouched on 0, and undo is a replay of
 * the history, never an unwinding.
 *
 * WHAT IS NOT HERE, ON PURPOSE. The wire (pk_code, pk_msg), the iOS bridge
 * (pk_api) and the seat resolver belong to the envelope. They reach the game
 * only through pk_new, pk_legal_turn, pk_apply, pk_seal and the read-only
 * fields below, so they can be added without this header changing:
 *   - encode walks hist[] and asks pk_legal_turn for each digit's base;
 *   - decode is pk_new followed by pk_apply / pk_seal of each choice;
 *   - the header's bubbles, turns and TIP_SAID come from the fields below.
 *
 * NO DYNAMIC ALLOCATION AND NO LIBC beyond memcpy / memset: the same files
 * build for wasm32 (-nostdlib, shared/c/wasm) and into an xcframework.
 * Every struct Swift will read is plain integers, no pointers, no bitfields,
 * so shared/tools/structgen can generate its access. */
#ifndef PK_H
#define PK_H

#include <stdint.h>

/* ---- cards (3.2) ---------------------------------------------------------
 *
 *   id  0..95    suited: suit = id / 24, k = id % 24
 *                  k  0..17  number (k / 2) + 1, copy k % 2
 *                  k 18..19  Skip
 *                  k 20..21  Reverse
 *                  k 22..23  +2
 *   id 96..99    Wild
 *   id 100..103  Wild +4
 *
 * THE ID ORDER IS PART OF THE FORMAT: the shuffle permutes 0..103 in this
 * order, so renumbering a card deals every existing game differently. */
#define PK_DECK        104
#define PK_SUITS       4
#define PK_NO_SUIT     4            /* a wild's own suit, and "no suit chosen" */
#define PK_R_SKIP      10
#define PK_R_REVERSE   11
#define PK_R_PLUS2     12
#define PK_R_WILD      13
#define PK_R_WILD4     14
#define PK_CARD_HIDDEN 0xFE         /* a masked card in a view or event */
#define PK_CARD_NONE   0xFF

static inline int pk_suit(uint8_t c) { return c < 96 ? c / 24 : PK_NO_SUIT; }
int pk_rank(uint8_t c);             /* 1..9, or PK_R_*; 0 for an id off the deck */
static inline int pk_is_wild(uint8_t c) { return c >= 96 && c < PK_DECK; }
static inline int pk_is_number(uint8_t c) { return c < 96 && c % 24 < 18; }

/* ---- the table (3.3) ----------------------------------------------------- */

#define PK_MAX_SEATS     8
#define PK_HAND_CAP      PK_DECK       /* physical limit, never a rule (D23) */
#define PK_MAX_ACTIONS   1500          /* long-game stop (1.11, D23)         */
#define PK_MAX_BUBBLES   750
#define PK_HIST_CAP      (2 * PK_MAX_ACTIONS + PK_MAX_BUBBLES)   /* DECISION D30 */
#define PK_SEAT_NONE     0xFF
#define PK_HAND_SIZE     7             /* cards dealt to each seat           */

enum { PK_OVER_NO = 0, PK_OVER_OUT, PK_OVER_STUCK, PK_OVER_LONG };

/* Who is looking (pk_view, pk_plan): a seat 0..n-1, or one of these. */
#define PK_VIEW_SPECTATOR (-1)
#define PK_VIEW_ALL       (-2)     /* tests and the finished board only */

/* Directions as a host reads them (views and events). The game itself keeps
 * dir as +1 / -1 so next() is one multiply. */
enum { PK_DIR_CW = 0, PK_DIR_ACW = 1 };

/* ---- actions (3.4) -------------------------------------------------------
 *
 * DRAW, PLAY and PASS are TURN ACTIONS. SAY_IT and CALL_OUT are BUBBLE-LEVEL:
 * they belong to the bubble, not to a moment in it (D5d), so the history
 * keeps them in the bubble's BUBBLE record whenever they were tapped.
 * BUBBLE and CONTINUE never come from a host; they are how hist[] stores
 * bubble boundaries. */
enum {
    PK_A_DRAW = 1,      /* take the top of the deck (reshuffling first if it must) */
    PK_A_PLAY,          /* a = hand position, b = suit for a wild (0..3), else PK_NO_SUIT */
    PK_A_PASS,
    PK_A_SAY_IT,        /* "Last card!" - bubble-level                              */
    PK_A_CALL_OUT,      /* a = target seat - bubble-level                           */
    PK_A_BUBBLE,        /* history only: a = sender, b = PK_BR_* bits, c = call target */
    PK_A_CONTINUE,      /* history only: the sender went on to their next turn (D7) */
};
typedef struct { uint8_t kind, a, b, c; } PkAct;

/* The BUBBLE record's b bits. SEALED is DECISION D31: it makes hist[] say by
 * itself whether its last bubble is a draft, so a replay needs nothing else. */
enum { PK_BR_SAID = 1, PK_BR_CALL = 2, PK_BR_SEALED = 4 };

typedef struct {
    /* the table */
    uint8_t  n;                          /* seats, 2..8                              */
    uint8_t  turn;                       /* whose turn, PK_SEAT_NONE when over       */
    int8_t   dir;                        /* +1 clockwise, -1 anticlockwise           */
    uint8_t  live_suit;                  /* 0..3                                     */
    uint8_t  deck[PK_DECK];              /* deck[deck_n-1] is the top                */
    uint8_t  deck_n;
    uint8_t  stack[PK_DECK];             /* stack[stack_n-1] is the top              */
    uint8_t  stack_n;
    /* acquisition order (D24). KERNEL-ONLY: another seat's count is never a
     * display value (D22); pk_view is what a host reads. */
    uint8_t  hand[PK_MAX_SEATS][PK_HAND_CAP];
    uint8_t  hand_n[PK_MAX_SEATS];

    /* the last card (1.8) */
    uint8_t  exposed;                    /* bit s: on one card, not said, window open */
    uint8_t  said;                       /* bit s: said it, still on one card (LAST)  */

    /* the end */
    uint8_t  over;                       /* PK_OVER_*                                 */
    uint8_t  winner;                     /* seat, or PK_SEAT_NONE                     */
    uint8_t  idle_passes;                /* bare passes in a row (1.10)               */
    uint8_t  starter;                    /* who started it (LOBBY_START), or NONE     */

    /* the bubble being composed (all zero between bubbles) */
    uint8_t  b_open;                     /* 1 while a bubble is open                  */
    uint8_t  b_sender;
    uint8_t  b_said;                     /* "Last card!" in this bubble               */
    uint8_t  b_call;                     /* caught seat, or PK_SEAT_NONE              */
    uint8_t  b_exposed_at_open;          /* `exposed` when the bubble opened (D5d)    */
    uint8_t  b_said_at_open;             /* `said` when the bubble opened (D32)       */
    uint8_t  b_had_terminal;             /* a play or pass happened in this bubble    */
    uint8_t  b_created;                  /* exposures this bubble's plays created     */
    uint8_t  t_drew;                     /* the current turn has drawn (D10)          */
    uint8_t  pad0;

    /* counters the header repeats (4.2) and the stop reads */
    uint16_t reshuffles;                 /* r of the last reshuffle, 0 = none yet     */
    uint16_t actions;                    /* turn actions: draws, plays, passes        */
    uint16_t turns;                      /* completed turns (plays + passes)          */
    uint16_t bubbles;                    /* sealed bubbles                            */
    uint16_t b_floor;                    /* history index undo may not go below (D8)  */
    uint16_t b_rec;                      /* hist index of the open bubble's record    */

    /* the history the coder writes, the replay rebuilds, and undo truncates */
    uint16_t hist_n;
    PkAct    hist[PK_HIST_CAP];

    uint8_t  seed[32];
} PkGame;

/* ---- deal (3.5) ----------------------------------------------------------
 *
 * Shuffle from the seed, deal seven round-robin from seat 1, turn up a start
 * card, burying every non-number at the bottom (D14). Seat 1 plays first
 * (D19). Depends on the seed and `n` and nothing else. 1, or 0 for an `n`
 * out of 2..8. */
int  pk_new(PkGame *g, const uint8_t seed[32], int n);

/* The Fisher-Yates every shuffle uses, keyed at ChaCha block `block` of the
 * seed's keystream. Returns the first block it did NOT read, so a test can
 * prove two shuffles read disjoint stretches (7.3.5). */
uint64_t pk_shuffle(uint8_t *a, int m, const uint8_t seed[32], uint64_t block);

/* Where reshuffle r (1-based) starts reading: block r << 32 (D21). */
uint64_t pk_reshuffle_block(int r);

/* ---- legality (3.6) ------------------------------------------------------
 *
 * Everything `seat` may do now. THE ORDER IS PART OF THE FORMAT (4.4): the
 * turn actions first - DRAW if legal, then every legal PLAY by ascending hand
 * position (a wild that is not the last card as four entries in suit order
 * 0..3), then PASS if legal - and after them SAY_IT, then CALL_OUT by
 * ascending target. Returns the count; never more than `cap` written. */
int  pk_legal(const PkGame *g, int seat, PkAct *out, int cap);

/* Only the turn actions, the prefix of pk_legal: the coder's action digit. */
int  pk_legal_turn(const PkGame *g, int seat, PkAct *out, int cap);

int  pk_is_legal(const PkGame *g, int seat, PkAct a);

/* Is a PLAY at hand position `p` legal (with some suit)? The dimming of a
 * card in the hand. */
int  pk_can_play(const PkGame *g, int seat, int p);

/* May this draft be sealed into a bubble now? Non-empty, and not mid-turn
 * (the long-game stop excepted, 3.7). */
int  pk_can_seal(const PkGame *g);

/* Has the open bubble ended its turn (only SAY_IT, CALL_OUT and seal left)? */
int  pk_turn_ended(const PkGame *g);

/* ---- apply, seal and undo (3.7) ------------------------------------------ */

int  pk_apply(PkGame *g, int seat, PkAct a);   /* 1 applied, 0 refused (g untouched) */
int  pk_seal(PkGame *g);                       /* close the bubble; 1 or 0            */
int  pk_undo(PkGame *g);                       /* take back the newest undoable thing */
int  pk_unsay(PkGame *g);                      /* drop this bubble's SAY_IT           */
int  pk_uncall(PkGame *g);                     /* drop this bubble's CALL_OUT         */
int  pk_floor(const PkGame *g);                /* history index of the draft's floor  */

/* Rebuild `out` from `g`'s seed, seat count and starter through hist[0..k).
 * The one replay undo, the plan and the envelope's decode all share. 1, or 0
 * when the history does not replay (a tampered or truncated one). */
int  pk_replay(PkGame *out, const PkGame *g, int k);

/* Back to the draft's floor (D9): the host's cancel. */
int  pk_to_floor(PkGame *g);

/* A hash of everything that is the game - hands, deck, stack, flags, counters
 * and hist[0..hist_n) - and nothing that is not (the unused tails). Two games
 * that play the same from here hash the same. */
uint64_t pk_hash(const PkGame *g);

/* ---- small public helpers ------------------------------------------------ */

/* k seats on from `from` in the current direction. */
int  pk_next(const PkGame *g, int from, int k);

/* Is the game one where nothing can be drawn (deck empty, stack only its top)? */
int  pk_can_draw_any(const PkGame *g);

#endif
