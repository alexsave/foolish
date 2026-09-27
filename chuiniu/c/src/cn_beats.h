/* Chui Niu - the plan's events laid out on a clock.
 *
 * THE TIMELINE IS THE KERNEL'S (pickemup/c/src/pk_beats.h's shape, a small
 * fraction of it). cn_plan says WHAT happened; this file says WHEN each
 * thing moves and for how long, and what the board shows at any
 * millisecond of it (cn_beats_frame). A host samples the frame every display
 * tick and draws it; it holds no duration and no ordering of its own.
 *
 * ONE BEAT PER EVENT, one after another, CN_T_GAP apart:
 *
 *   ROUND  -> SHAKE  the cups come down and shake; new dice under them
 *   BID    -> BID    the bid pops up at its seat and becomes the bid to beat
 *   CALL   -> CALL   the caller's "Call" stamp
 *   REVEAL -> LIFT   every cup lifts (the dice are cn_view's shown_*)
 *          -> COUNT  the dice showing the face or a 1 light up one by one
 *   LOSE   -> DROP   the loser's cup drops a die; its count goes down at the end
 *   OUT    -> OUT    the seat greys out
 *   OVER   -> WIN    the winner's banner
 *
 * Fixed layout, no allocation, no libc; read from Swift through structgen. */
#ifndef CN_BEATS_H
#define CN_BEATS_H

#include "cn.h"
#include "cn_plan.h"

#define CN_T_SHAKE       760
#define CN_T_BID         420
#define CN_T_CALL        360
#define CN_T_LIFT        520
#define CN_T_COUNT_STEP  140    /* one more matching die lights            */
#define CN_T_COUNT_REST  420    /* the count holds before the loser drops  */
#define CN_T_DROP        480
#define CN_T_OUT         420
#define CN_T_WIN         900
#define CN_T_GAP          80

enum {
    CN_BK_SHAKE = 1, CN_BK_BID, CN_BK_CALL, CN_BK_LIFT, CN_BK_COUNT,
    CN_BK_DROP, CN_BK_OUT, CN_BK_WIN,
};

typedef struct {
    uint32_t start_ms;
    uint32_t dur_ms;
    uint16_t ev_i;          /* the plan event it plays                   */
    uint8_t  kind;          /* CN_BK_*                                   */
    uint8_t  seat;
    uint8_t  other;
    uint8_t  q, f;
    uint8_t  count;         /* COUNT: dice that light; DROP: dice left    */
    uint8_t  round;         /* SHAKE: the round that opens                */
    uint8_t  pad0[3];
} CnBeat;

#define CN_BEATS_MAX 64

enum { CN_BS_PENDING = 0, CN_BS_ACTIVE = 1, CN_BS_DONE = 2 };
#define CN_BEAT_NEVER 0xFFFFFFFFu

/* THE BOARD AT A MOMENT: what the host draws instead of its settled view
 * while a plan plays. Once done, its bid, counts, winner and round are the
 * settled game's (cn_beats_start of it); the cups stay up after a call that
 * ended the game and are down after one that opened a round. */
typedef struct {
    uint32_t now_ms;
    uint32_t next_ms;                   /* the next moment it changes, or NEVER   */
    uint8_t  done;                      /* every beat has run                     */
    uint8_t  cups_up;                   /* every die shown (cn_view's shown_*)    */
    uint8_t  shaking;                   /* the cups are shaking                   */
    uint8_t  highlight_f;               /* the face being counted, 0 none; 1s too */
    uint8_t  highlight_n;               /* matching dice lit so far               */
    uint8_t  bid_q, bid_f;              /* the bid on the board, 0 0 for none     */
    uint8_t  bidder;
    uint8_t  winner;                    /* CN_SEAT_NONE until the WIN beat starts */
    uint8_t  round;
    uint8_t  n;                         /* beats in the plan                      */
    uint8_t  pad0;
    uint8_t  dice_n[CN_MAX_SEATS];      /* every seat's count as drawn now        */
    uint8_t  state[CN_BEATS_MAX];       /* CN_BS_* of each beat                   */
    float    p[CN_BEATS_MAX];           /* each beat's eased progress, 0..1       */
} CnBeatFrame;

typedef struct {
    uint16_t n;
    uint16_t pad0;
    uint32_t total_ms;                  /* the last beat's end                    */
    uint32_t serial;                    /* which build this is (the bridge counts) */
    CnBeatFrame start;                  /* the board before the first beat        */
    CnBeat   beat[CN_BEATS_MAX];
} CnBeats;

/* The board before a plan: `g` as it stood (cups down, nothing lit). */
void cn_beats_start(const CnGame *g, CnBeatFrame *out);

/* Lay `n` events out from `start`. The beat count, or -1 when they do not
 * fit CN_BEATS_MAX (a host then shows the settled view). */
int  cn_beats_build(const CnEvent *ev, int n, const CnBeatFrame *start, CnBeats *out);

/* The board at `now_ms` from the plan's start. */
void cn_beats_frame(const CnBeats *b, uint32_t now_ms, CnBeatFrame *out);

/* The one curve: ease in and out (cubic). */
float cn_ease(float t);

#endif
