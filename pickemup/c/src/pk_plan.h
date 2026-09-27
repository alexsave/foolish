/* Pick 'Em Up - the animation plan (RULES_AND_KERNEL.md section 5).
 *
 * THE EVENTS ARE THE KERNEL'S OWN APPLY, OBSERVED. pk_plan does not describe
 * a bubble after the fact from a diff of two states; it replays the game from
 * its seed through the one apply path, and that path reports each thing it
 * does as it does it. So an event list cannot disagree with the state it
 * leads to: there is no second derivation to drift (foolish's evwire rule).
 *
 * The order is the order the kernel applied things (5.3.1); a host never
 * reorders. Every event carries its `step`: one step per kernel action (a
 * deal card, a draw, a play, a pass, a say, a catch declaration, a seal), and
 * an action's automatic consequences share its step. Within a step everything
 * moves at once; between steps, one after another - except PENALTY_DRAW and
 * REVEAL, which a host staggers because the player is being told a quantity.
 *
 * `half` is the cut (5.3.6): ACTION events play at stage (channel A), SETTLE
 * events are held until Send (channel B). The catch outcome is SETTLE and is
 * never in a draft's plan at all, because it is decided at seal (D5d). */
#ifndef PK_PLAN_H
#define PK_PLAN_H

#include "pk.h"

enum {
    PK_HALF_ACTION = 0,    /* what the sender did: plays at stage (channel A) */
    PK_HALF_SETTLE = 1,    /* what it caused: held until Send (channel B)     */
};

enum {
    PK_EV_NONE = 0,
    PK_EV_LOBBY_JOIN,      /* seat = the joiner                               */
    PK_EV_LOBBY_LEAVE,     /* seat = the leaver; later rows move down         */
    PK_EV_LOBBY_START,     /* seat = the starter, n = seats                   */
    PK_EV_SHUFFLE,         /* n = 104, once, before the deal                  */
    PK_EV_DEAL,            /* seat = receiver, card masked, n = round 1..7, i = 1..7n */
    PK_EV_FLIP,            /* card = the card turned over                     */
    PK_EV_BURY,            /* card = a flipped non-number, to the deck bottom */
    PK_EV_START_CARD,      /* card = the flip that stuck; suit = live suit    */
    PK_EV_TURN_TO,         /* SETTLE; seat = new turn seat, other = previous  */
    PK_EV_SAY_IT,          /* ACTION; seat = the sayer                        */
    PK_EV_CALL_OUT,        /* ACTION; seat = the target, other = the catcher  */
    PK_EV_DRAW,            /* ACTION; seat = drawer, card masked, i = draws this turn so far */
    PK_EV_RESHUFFLE_GATHER,/* same half as the draw it serves; n = cards      */
    PK_EV_RESHUFFLE_SHUFFLE,/* n = cards, i = reshuffle number r (low 8 bits) */
    PK_EV_RESHUFFLE_DONE,  /* n = new deck count                              */
    PK_EV_PLAY,            /* ACTION; seat = player, card, i = hand position  */
    PK_EV_WILD_SUIT,       /* ACTION; seat = player, card = the wild, suit = chosen */
    PK_EV_SKIP,            /* SETTLE; seat = the skipped, other = the player  */
    PK_EV_REVERSE,         /* SETTLE; other = the player, dir = new direction */
    PK_EV_REVERSE_AS_SKIP, /* SETTLE; 2 players (D13); seat = skipped, other = player */
    PK_EV_PENALTY,         /* SETTLE; seat = victim, other = cause's seat, card = cause card or NONE,
                              n = cards owed, i = PK_PEN_*                    */
    PK_EV_PENALTY_DRAW,    /* SETTLE; seat = victim, card masked, n = owed, i = 1..n */
    PK_EV_PENALTY_SHORT,   /* SETTLE; seat = victim, n = cards forgiven (D15) */
    PK_EV_PASS,            /* ACTION; seat = passer, n = cards drawn this turn */
    PK_EV_CALL_HIT,        /* SETTLE; seat = the caught, other = the catcher  */
    PK_EV_CALL_MISS,       /* SETTLE; seat = the catcher, other = the target  */
    PK_EV_WIN,             /* SETTLE; seat = winner, card = last card played or NONE, i = PK_OVER_* */
    PK_EV_REVEAL,          /* SETTLE; seat, card (never masked), n = hand size, i = 1..n */
    PK_EV_BUBBLE_BEGIN,    /* seat = sender                                   */
    PK_EV_BUBBLE_END,      /* seat = sender                                   */
    PK_EV_COUNT
};

/* What a PENALTY is for (its i). */
enum { PK_PEN_PLUS2 = 1, PK_PEN_WILD4, PK_PEN_CAUGHT, PK_PEN_WRONG };

typedef struct {
    uint8_t  kind;         /* PK_EV_*                                          */
    uint8_t  half;         /* PK_HALF_*                                        */
    uint8_t  seat;         /* the seat it happens to, or PK_SEAT_NONE          */
    uint8_t  other;        /* the other seat involved, or PK_SEAT_NONE         */
    uint8_t  card;         /* id, PK_CARD_HIDDEN when masked, PK_CARD_NONE     */
    uint8_t  suit;         /* live suit after the event                        */
    uint8_t  n;            /* how many (penalty size, reshuffle size, round)   */
    uint8_t  i;            /* which of n (1-based), or the hand position       */
    uint16_t bubble;       /* 0 for the deal, else the bubble it belongs to    */
    uint16_t step;         /* the kernel step it belongs to                    */
    uint8_t  deck_n;       /* deck count after this event                      */
    uint8_t  dir;          /* PK_DIR_* after this event                        */
} PkEvent;                 /* 14 bytes, fixed layout, structgen'd to Swift     */

/* Every event of bubbles `from` (exclusive) to `to` (inclusive), masked for
 * `viewer` (a seat, PK_VIEW_SPECTATOR or PK_VIEW_ALL). The deal is bubble 0,
 * so from = -1 includes it; the open draft, if any, is bubble `bubbles + 1`.
 * Returns the count written, or -1 if `cap` is too small or the history does
 * not replay. */
int pk_plan(const PkGame *g, int viewer, int from, int to, PkEvent *out, int cap);

/* The draft's own events (the open bubble), for channel A. 0 with no draft. */
int pk_plan_draft(const PkGame *g, int viewer, PkEvent *out, int cap);

/* The same events, one at a time, to `fn`; for callers that summarise rather
 * than keep (pk_since, the captions). Returns the count, -1 on a bad replay. */
typedef void (*PkEventFn)(const PkEvent *e, void *ctx);
int pk_plan_each(const PkGame *g, int viewer, int from, int to, PkEventFn fn, void *ctx);

/* ---- since last seen (5.4) ----------------------------------------------
 *
 * Counts of cards that MOVED over a bubble range, which are public; they
 * never become a hand count on screen. The per-seat counts are 16 bits
 * (DECISION D36): one seat can draw more than 255 cards in a whole game. */
typedef struct {
    uint16_t from, to;                 /* the bubble range summarised           */
    uint16_t drawn[PK_MAX_SEATS];      /* own draws per seat                    */
    uint16_t penalty[PK_MAX_SEATS];    /* penalty cards per seat                */
    uint16_t plays[PK_MAX_SEATS];
    uint16_t reshuffles;               /* how many happened in the range        */
    uint8_t  caught, caught_by;        /* last hit in the range, or NONE        */
    uint8_t  wrong, wrong_on;          /* last miss in the range, or NONE       */
    uint8_t  said;                     /* seats that said "Last card!"          */
    uint8_t  skipped;                  /* seats skipped (Skip, 2p Reverse)      */
    uint8_t  reversed;                 /* direction flips                       */
    uint8_t  pad0;
} PkSince;

/* 1, or 0 if the history does not replay. */
int pk_since(const PkGame *g, int from, int to, PkSince *out);

#endif
