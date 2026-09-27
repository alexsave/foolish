/* Chui Niu - the animation plan: what a run of moves did, as events.
 *
 * ONE APPLY PATH (pickemup's rule). cn.c's apply reports every thing it does
 * to an optional sink; cn_plan replays the game from its seed through that
 * one path with a sink attached, so an event list cannot disagree with the
 * state it leads to.
 *
 * NOTHING HERE IS SECRET. A bid is public; the dice an event carries are the
 * ones a call has just revealed, which every seat sees. A new round's dice
 * are not in any event: a host reads its own from cn_view. So a plan needs
 * no viewer and masks nothing. */
#ifndef CN_PLAN_H
#define CN_PLAN_H

#include "cn.h"

enum {
    CN_EV_ROUND = 1,   /* a round opens: cups shake, dice re-rolled.
                          seat = opener, round = its index              */
    CN_EV_BID,         /* seat bid (q, f)                               */
    CN_EV_CALL,        /* seat called other's bid (q, f)                */
    CN_EV_REVEAL,      /* cups lift: every die shown (dice_n, dice),
                          count = dice showing f or a 1, q f the bid     */
    CN_EV_LOSE,        /* seat loses a die; count = dice it has left     */
    CN_EV_OUT,         /* seat has no dice left                          */
    CN_EV_OVER,        /* seat wins                                      */
    CN_EV_LOBBY_JOIN,  /* the lobby's (cn_lobby.h): seat joined          */
    CN_EV_LOBBY_LEAVE, /* seat left                                      */
};

typedef struct {
    uint8_t  kind;                        /* CN_EV_*                            */
    uint8_t  seat;
    uint8_t  other;                       /* CALL: the bidder; else CN_SEAT_NONE */
    uint8_t  q, f;
    uint8_t  count;
    uint8_t  round;
    uint8_t  pad0;
    uint16_t move;                        /* 1-based move that caused it, 0 the start */
    uint8_t  dice_n[CN_MAX_SEATS];        /* REVEAL only                        */
    uint8_t  dice[CN_MAX_DICE];           /* REVEAL only: seat s die i at s*5+i */
} CnEvent;

/* The most events one move makes: CALL, REVEAL, LOSE, OUT, OVER or ROUND. */
#define CN_EVENTS_PER_MOVE 5

/* Events of moves (from, to] of `g` (0 <= from <= to <= hist_n); from = -1
 * includes the start (a ROUND for round 0). Returns the count, or -1 when
 * the range is bad, the history does not replay or `cap` is too small. */
int cn_plan(const CnGame *g, int from, int to, CnEvent *out, int cap);

/* The events one move would make from `g`, without changing `g`: what a host
 * staging that move would show. -1 for an illegal move or a small `cap`. */
int cn_plan_move(const CnGame *g, CnMove m, CnEvent *out, int cap);

#endif
