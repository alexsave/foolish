/* HOW A MOVE MOVES, as a pure function of a plan and a clock.
 *
 * docs/UI.html, "How it moves, before how it looks", is a grid of channels:
 * one move arrives on screen through several doors and each door has its own
 * motion. What animates, in what order and for how long is a decision about
 * the game, so it lives here beside the pen; a renderer asks "what does the
 * board look like at t" once a display frame and draws the answer. It never
 * schedules anything and never types a duration.
 *
 * Three things move, all on the board's own 0..1 square:
 *   - the new mark, drawn in (the same draw stopped early, uttt_draw.c);
 *   - the highlighter, ONE rect travelling from the block the move was
 *     played in to the block it sends the other player to (or growing to
 *     the whole sheet when they are freed) - after the ink lands, never with
 *     it;
 *   - the destination pulse, a ring that opens out from the destination
 *     block twice, 300 ms after the ink lands.
 */
#ifndef UTTT_ANIM_H
#define UTTT_ANIM_H

#include "uttt.h"

/* The channels of UI.html's grid that draw something. B (at Send) and Undo
 * are not here: B is "usually nothing" and there is no undo (owner). */
enum {
    UTTT_CH_STILL   = 0,   /* nothing moves: the resting board              */
    UTTT_CH_STAGE   = 1,   /* A: I tapped a square                          */
    UTTT_CH_REPLAY  = 2,   /* C: I reopened my own bubble - no pulse        */
    UTTT_CH_THEIRS  = 3,   /* D: I opened a bubble of theirs                */
    UTTT_CH_ARRIVAL = 4,   /* E: their move landed while I was looking      */
};

/* THE TIMINGS, in milliseconds. From UI.html's grid ("the mark draws
 * (260/340ms), then the destination board pulses, at +300ms") and its pulse
 * keyframes (.62s ease-out, twice); the highlighter's travel is the Play
 * nib's .34 of its one-second move, and on their move the Motion tab's
 * freed-board .42, because "the wash takes longer to move, because you did
 * not choose it". */
#define UTTT_MS_INK_X        260
#define UTTT_MS_INK_O        340
#define UTTT_MS_WASH_MINE    340
#define UTTT_MS_WASH_THEIRS  420
#define UTTT_MS_PULSE_AT     300     /* after the ink lands                 */
#define UTTT_MS_PULSE        620     /* one ring                            */
#define UTTT_PULSES          2

typedef struct {
    int32_t ch;
    int32_t mv;            /* block*9+cell, -1 when nothing is drawn in      */
    int32_t mark;          /* UTTT_X / UTTT_O                                */
    int32_t from, to;      /* wash blocks: 0..8, 9 anywhere, -1 none         */
    int32_t ink_ms;        /* the mark is drawn over [0, ink_ms]             */
    int32_t wash_at, wash_ms;
    int32_t pulse_at;      /* -1 for no pulse                                */
    int32_t end_ms;        /* nothing moves at or after this                 */
} UtttMotion;

typedef struct {
    float   mark_t;        /* 0..1 how far the new mark is drawn             */
    float   wash[4];       /* x, y, w, h; w == 0 for no wash                 */
    uint32_t wash_rgba;    /* the highlighter, alpha included (0xRRGGBBAA)   */
    float   pulse[4];      /* the destination's rect; w == 0 for no ring     */
    float   pulse_spread;  /* how far the ring stands out, board units       */
    uint32_t pulse_rgba;   /* the ring, alpha included                       */
    int32_t landed;        /* 1 once the ink is down - the drawer may move   */
    int32_t running;       /* 0 once nothing will change again               */
} UtttFrame;

/* The plan for the LAST move of `g` arriving through `ch`. A game with no
 * moves, or UTTT_CH_STILL, gives a plan that is already at rest. */
UtttMotion uttt_motion(const UtttGame *g, int ch);

/* The board at `now_ms` since the plan started. Pure. */
void uttt_motion_at(const UtttMotion *m, int32_t now_ms, UtttFrame *f);

/* Where the highlighter sits on `block` (0..8, 9 the whole sheet), and how
 * strong it is. The one owner of the wash's geometry: uttt_draw_board and
 * the travelling rect both read it. Returns 0 for no wash (-1). */
int uttt_wash_rect(int block, float r[4], float *alpha);

/* The highlighter's and the ring's colours, with alpha. */
uint32_t uttt_wash_rgba(float alpha);
#define UTTT_PULSE_RGB   0xa8321f00u     /* UI.html cellpulse, rgba(168,50,31) */
#define UTTT_PULSE_ALPHA .5f
#define UTTT_PULSE_REACH (13.f / 390.f)  /* 13 points on a 390-point board   */

#endif
