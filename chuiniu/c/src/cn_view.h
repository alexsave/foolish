/* Chui Niu - the masked per-seat view: what one phone may draw.
 *
 * MY OWN DICE, EVERYBODY'S COUNTS. While a round is open a seat sees its own
 * dice and only the number of dice every other seat holds. Once a call is
 * made, the dice that stood at the call are everybody's (shown_*), until the
 * next call replaces them. A spectator sees counts only.
 *
 * THE MENU is the seat on turn's: whether it may raise, the lowest legal
 * raise, the highest quantity a bid may name (R7), and whether it may call
 * (never on a round's opening bid, R3 / R5).
 *
 * Every die list is sorted ascending, so a host never sorts. Plain integers,
 * one-dimensional arrays only, so shared/tools/structgen can read it. */
#ifndef CN_VIEW_H
#define CN_VIEW_H

#include "cn.h"

#define CN_VIEW_SPECTATOR (-1)
#define CN_VIEW_ALL       (-2)     /* tests only: every seat's current dice in all[] */

typedef struct {
    int8_t   viewer;                    /* a seat, or CN_VIEW_*                     */
    uint8_t  n;
    uint8_t  phase;                     /* CN_PH_*                                  */
    uint8_t  round;
    uint8_t  turn;                      /* CN_SEAT_NONE when over                   */
    uint8_t  my_turn;                   /* the viewer is on turn                    */
    uint8_t  bid_q, bid_f;              /* the standing bid, 0 0 for none           */
    uint8_t  bidder;
    uint8_t  total;                     /* dice on the table                        */
    uint8_t  winner;
    uint8_t  dice_n[CN_MAX_SEATS];      /* every seat's count: public               */

    uint8_t  my_n;                      /* the viewer's own dice, sorted            */
    uint8_t  my_dice[CN_START_DICE];

    /* the menu, all zero unless my_turn */
    uint8_t  can_raise;
    uint8_t  can_call;
    uint8_t  min_q, min_f;              /* the lowest legal raise                   */
    uint8_t  max_q;                     /* the highest quantity a bid may name      */

    /* the newest call, public; revealed while its reveal is the table's news
     * (phase REVEALED or OVER) */
    uint8_t  revealed;
    uint8_t  call_seat;                 /* CN_SEAT_NONE before the first call       */
    uint8_t  call_bidder;
    uint8_t  call_q, call_f;
    uint8_t  call_count;
    uint8_t  call_loser;
    uint8_t  call_true;                 /* the bid stood: the caller lost           */
    uint8_t  shown_n[CN_MAX_SEATS];
    uint8_t  shown[CN_MAX_DICE];        /* seat s at s*5, sorted within the seat    */

    uint8_t  all[CN_MAX_DICE];          /* CN_VIEW_ALL only, seat s at s*5, sorted  */
} CnView;

/* The view of `g` for `viewer`. */
void cn_view(const CnGame *g, int viewer, CnView *out);

#endif
