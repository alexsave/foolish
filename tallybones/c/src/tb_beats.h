/* Tallybones - the plan's events laid out on a clock (DECISIONS.md T9).
 *
 * THE TIMELINE IS THE KERNEL'S (pickemup/c/src/pk_beats.h's shape, much
 * smaller): tb_plan says WHAT happened, in the order the kernel applied it;
 * this file says WHEN each thing moves, for how long, on which curve, and
 * what the board shows at any moment of it. A host samples it every frame
 * (tb_beats_frame, tb_beat_sample) and holds no duration, no easing and no
 * order of its own.
 *
 * T9's three motions and nothing more:
 *   - a ROLL is one SETTLE beat: the rolled dice tumble and land, one after
 *     another STEP apart, the kept ones never move (a KEEP bubble rolls only
 *     its rerolled positions; a turn's roll 1 rolls all five);
 *   - a SCORE (and its BONUS) is a STAMP into the seat's category row;
 *   - a TURN is the turn bar passing to the next seat.
 * A LEAVE fades its seat, and the end fades the results in and holds.
 *
 * Fixed-layout structs, no allocation, no libc: the file builds for wasm32
 * and is read from Swift through structgen's readers. */
#ifndef TB_BEATS_H
#define TB_BEATS_H

#include "tb.h"
#include "tb_plan.h"

/* ---- the vocabulary ---------------------------------------------------------- */
#define TB_T_LEAD_OPEN   100   /* a sequence starts after a bubble was opened   */
#define TB_T_LEAD_LIVE    16   /* ... or one frame after it landed, or was sent */
#define TB_T_GAP          25   /* between two beats                             */
#define TB_T_SETTLE      620   /* one die: tumble, land, rest                   */
#define TB_T_SETTLE_STEP  70   /* start to start of two dice                    */
#define TB_T_TUMBLE       60   /* a tumbling die shows a new face this often    */
#define TB_T_STAMP       340   /* a score stamps into its row                   */
#define TB_T_TURN        340   /* the turn bar moves on                         */
#define TB_T_FADE        220
#define TB_T_OVER       1000   /* the results rest before anything else         */

enum { TB_BK_NONE = 0, TB_BK_SETTLE, TB_BK_STAMP, TB_BK_TURN, TB_BK_FADE, TB_BK_HOLD, TB_BK_COUNT };
/* STAMP subs, and FADE's: what fades */
enum { TB_STAMP_SCORE = 1, TB_STAMP_BONUS = 2 };
enum { TB_FADE_SEAT_OUT = 1, TB_FADE_RESULTS_IN = 2 };
enum { TB_EASE_LINEAR = 0, TB_EASE_OUT, TB_EASE_SETTLE, TB_EASE_STAMP, TB_EASE_COUNT };

typedef struct {
    uint32_t start_ms;   /* from the plan's start                               */
    uint16_t dur_ms;     /* the whole envelope, every part included             */
    uint16_t part_ms;    /* one die's own run (dur_ms with a single part)       */
    uint16_t ev_i;       /* the plan event it plays                             */
    uint8_t  kind;       /* TB_BK_*                                             */
    uint8_t  ease;       /* TB_EASE_*                                           */
    uint8_t  seat;       /* the seat it is about, or TB_SEAT_NONE               */
    uint8_t  other;      /* TURN: the seat it leaves                            */
    uint8_t  cat;        /* STAMP: the category (BONUS: TB_CATS)                */
    uint8_t  mask;       /* SETTLE: the dice rolled                             */
    uint8_t  parts;      /* SETTLE: how many dice                               */
    uint8_t  stagger_ms; /* start to start of two parts                         */
    uint8_t  sub;        /* STAMP / FADE sub; SETTLE: the roll index            */
    uint8_t  ev_kind;    /* the TB_EV_* it plays                                */
    uint16_t value;      /* STAMP: the points                                   */
    uint16_t total;      /* STAMP: the seat's total once it lands               */
    uint8_t  dice[TB_DICE];   /* SETTLE: where the dice land                    */
    uint8_t  pad0[3];
} TbBeat;                /* 32 bytes */

#define TB_BEATS_MAX 96

/* ---- the board as of a moment ------------------------------------------------ */
typedef struct {
    uint8_t  dice[TB_DICE];      /* the face each die shows: 0 blank (in the cup)  */
    uint8_t  rolling;            /* bit i: die i is tumbling now                    */
    uint8_t  kept;               /* the held dice                                   */
    uint8_t  turn;               /* the turn bar's seat, or TB_SEAT_NONE            */
    uint8_t  roll;               /* the roll the tray shows, 1..3                   */
    uint8_t  in;                 /* bit s: seat s is still shown in the game        */
    uint8_t  results;            /* 1 once the results have begun to fade in        */
    uint8_t  winners;
    uint8_t  done;               /* every beat has run                              */
    uint8_t  pad0[3];
    uint16_t filled[TB_MAX_SEATS];   /* a stamp still in the air is not filled     */
    uint16_t total[TB_MAX_SEATS];
    uint32_t now_ms;
    uint32_t next_ms;            /* the next moment the frame changes, or TB_BEAT_NEVER */
} TbBeatFrame;

#define TB_BEAT_NEVER 0xFFFFFFFFu

enum { TB_BS_PENDING = 0, TB_BS_ACTIVE = 1, TB_BS_DONE = 2 };
typedef struct {
    float   p;          /* eased progress 0..1 (may overshoot)                  */
    float   scale;
    float   rot;        /* degrees                                              */
    float   dx, dy;     /* points                                               */
    float   opacity;
    uint8_t state;      /* TB_BS_*                                              */
    uint8_t apply;      /* 1: the host applies this transform now               */
    uint8_t pad0[2];
} TbBeatSample;

enum {
    TB_BEATS_OPEN = 0,   /* a bubble opened: lead 100ms                          */
    TB_BEATS_ARRIVAL,    /* a bubble landed while I look: lead 16ms              */
    TB_BEATS_SEND,       /* my own bubble, sent: lead 16ms                       */
};

typedef struct {
    uint16_t n;
    uint16_t pad0;
    uint32_t total_ms;   /* the last beat's end                                  */
    uint32_t serial;     /* which build this is (the bridge counts; 0 here)      */
    uint8_t  mode, n_seats, pad1[2];
    TbBeatFrame start;   /* the board before the first beat                      */
    TbBeat   beat[TB_BEATS_MAX];
} TbBeats;

/* The longest event list a build takes. */
#define TB_BEATS_EVENTS 512

/* The board at the end of bubble `from` (-1: before the start), replayed from
 * the seed. 1, or 0 when it does not replay. */
int tb_beats_pre(const TbGame *g, int from, TbBeatFrame *out);

/* Lay `n` plan events out from `start`. The beat count, or -1 when they do
 * not fit TB_BEATS_MAX (a host then shows the settled view). */
int tb_beats_build(const TbEvent *ev, int n, const TbBeatFrame *start, int n_seats, int mode, TbBeats *out);

/* The board at `now_ms` from the plan's start. */
void tb_beats_frame(const TbBeats *b, uint32_t now_ms, TbBeatFrame *out);

/* One beat at `now_ms`; `part` is a die 0..4 for a SETTLE, else 0. */
void tb_beat_sample(const TbBeat *b, uint32_t now_ms, int part, TbBeatSample *out);

float tb_ease(int ease, float t);

#endif
