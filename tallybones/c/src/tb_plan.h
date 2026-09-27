/* Tallybones - the animation plan: what each bubble does, as events (T9).
 *
 * THE EVENTS ARE THE KERNEL'S OWN APPLY, OBSERVED (pk_plan's rule): tb_plan
 * replays the game from its seed through the one apply path, and that path
 * reports what it does as it does it, so an event list cannot disagree with
 * the state it leads to.
 *
 * Bubble 0 is the start (START, TURN, ROLL of all five); bubble k >= 1 is
 * hist[k - 1]. A KEEP bubble is KEEP then ROLL of the rerolled positions; a
 * SCORE bubble is SCORE (BONUS when it makes the numbers half), then TURN
 * and the next seat's ROLL, or OVER. A LEAVE is LEAVE, then TURN and ROLL
 * when the turn seat left, or OVER.
 *
 * A DRAFT'S PLAN (tb_plan_draft) is its pending move applied with nothing
 * derived: its ROLL events carry 0 for every rerolled die. */
#ifndef TB_PLAN_H
#define TB_PLAN_H

#include "tb.h"

enum {
    TB_EV_NONE = 0,
    TB_EV_LOBBY_JOIN,      /* seat = the joiner                                  */
    TB_EV_LOBBY_LEAVE,     /* seat = the leaver; later rows move down            */
    TB_EV_START,           /* seat = the starter, value = seats                  */
    TB_EV_TURN,            /* seat = the new turn seat, other = the previous one */
    TB_EV_ROLL,            /* seat, roll = 1..3, mask = positions rolled, dice   */
    TB_EV_KEEP,            /* seat, mask = positions kept, dice = before the roll */
    TB_EV_SCORE,           /* seat, cat, value = points                          */
    TB_EV_BONUS,           /* seat, value = 35: the numbers half reached 63      */
    TB_EV_LEAVE,           /* seat = the leaver                                  */
    TB_EV_OVER,            /* mask = the winners, value = the winning total      */
    TB_EV_COUNT
};

typedef struct {
    uint8_t  kind;         /* TB_EV_*                                        */
    uint8_t  seat;
    uint8_t  other;
    uint8_t  cat;
    uint8_t  mask;
    uint8_t  roll;
    uint16_t value;
    uint8_t  dice[TB_DICE];/* ROLL: after it (0 unknown); KEEP: as kept    */
    uint8_t  pad0;
    uint16_t bubble;       /* 0 the start, else the bubble it belongs to     */
} TbEvent;                 /* 16 bytes, fixed layout, structgen'd to Swift   */

/* Every event of bubbles `from` (exclusive) to `to` (inclusive); from = -1
 * includes the start. The count, or -1 for a range off the game, a `cap` too
 * small, or a history that does not replay. `g` must not be a draft. */
int tb_plan(const TbGame *g, int from, int to, TbEvent *out, int cap);

/* The draft's own events (its pending move, nothing derived). 0 with no
 * draft, -1 for a `cap` too small. */
int tb_plan_draft(const TbGame *g, TbEvent *out, int cap);

typedef void (*TbEventFn)(const TbEvent *e, void *ctx);
int tb_plan_each(const TbGame *g, int from, int to, TbEventFn fn, void *ctx);

#endif
