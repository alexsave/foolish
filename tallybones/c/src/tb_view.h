/* Tallybones - the view: the game as a host draws it (T7: everyone sees
 * everything, so there is no masking, only the plain state in a fixed
 * layout structgen can read, with the card's sums done here).
 *
 * A DRAFT'S VIEW is the draft's state: a staged KEEP's rerolling dice read 0
 * and `known` has their bits clear. Nothing here can hold a value for them,
 * because the draft never derived one (tb.h). */
#ifndef TB_VIEW_H
#define TB_VIEW_H

#include "tb.h"

typedef struct {
    uint16_t filled;             /* bit c: category c is scored                 */
    uint16_t total;              /* both halves and the bonus                   */
    uint16_t upper;              /* the numbers half                            */
    uint8_t  bonus;              /* 35 or 0                                     */
    uint8_t  still_in;           /* 1: still in the game; 0: left (T5)          */
    uint8_t  winner;             /* 1 once over, for every seat that won        */
    uint8_t  pad0;
    uint8_t  score[TB_CATS];     /* points in each filled category, 0 when open */
    uint8_t  pad1;
} TbCard;

typedef struct {
    uint8_t  n;                  /* seats                                        */
    uint8_t  turn;               /* whose turn, TB_SEAT_NONE once over           */
    uint8_t  roll;               /* rolls made this turn, 1..3                   */
    uint8_t  over;
    uint8_t  draft;              /* 1: a staged move is applied, nothing derived  */
    uint8_t  pending_kind;       /* TB_M_* of the staged move, 0 for none        */
    uint8_t  pending_arg;        /* its mask or category                         */
    uint8_t  kept;               /* the newest KEEP of this turn: the held dice  */
    uint8_t  dice[TB_DICE];      /* 1..6, 0 unknown                              */
    uint8_t  known;              /* bit i: dice[i] is a value                    */
    uint8_t  rolls_left;         /* rerolls the turn seat may still take         */
    uint8_t  winners;            /* bit s, once over                             */
    uint16_t turns;              /* completed turns                              */
    uint16_t bubbles;            /* moves, the staged one included               */
    /* what each category would score for the turn seat with these dice: the
     * scorecard's preview. 0 for a taken one, and all 0 while a die is
     * unknown */
    uint8_t  would[TB_CATS];
    uint8_t  pad0;
    TbCard   seat[TB_MAX_SEATS];
} TbView;

void tb_view(const TbGame *g, TbView *out);

#endif
