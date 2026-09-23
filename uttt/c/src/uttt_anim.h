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
    int32_t settled;       /* 1 once the wash has arrived too - the host may
                              insert its bubble without stalling a travel   */
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

/* THE HEIGHT THE SHEET IS LAID OUT AT, which is not the height Messages
 * last handed it.
 *
 * Messages resizes the extension sparsely: an auto-collapse is ONE new
 * height, handed about 20 ms before the drawer starts to slide, and after a
 * manual drag is released the drawer settles on its own spring while the
 * extension hears a new height only about every 200 ms (516, 334, 289).
 * Laid out at the handed height, the board shrank in one frame and then in
 * 50-90 point steps while the drawer moved smoothly beside it (measured with
 * the ruler, uttt/docs/TESTFLIGHT_PLAN.md "Drawer motion").
 *
 * So the layout height is a critically damped spring on the host's own
 * response (0.338 s, fitted to the Messages drawer in docs/COLLAPSE_MSE.md)
 * from wherever the layout is toward the last handed height. A small change
 * - a finger dragging the handle hands one every frame - is followed at
 * once, because a spring would put the layout 100 points behind the finger;
 * a large one, or any change while the spring is still running, re-aims the
 * spring from its current position AND velocity, so there is never a step.
 * A spring that starts from rest waits UTTT_DRAWER_LEAD_MS, the lead the
 * height has over the slide. */
#define UTTT_DRAWER_RESPONSE_MS 338
#define UTTT_DRAWER_FOLLOW_PT   32.f
#define UTTT_DRAWER_LEAD_MS     20

typedef struct {
    float   target;      /* the last height handed                        */
    float   from;        /* layout minus target at t0                     */
    float   vel;         /* layout velocity at t0, points per ms          */
    int32_t t0;          /* when the running spring started               */
    int32_t moving;      /* 1 while a spring runs                         */
    int32_t seen;        /* 0 until the first height                      */
} UtttDrawer;

/* A new height from the host at `now_ms`. */
void  uttt_drawer_report(UtttDrawer *d, float h, int32_t now_ms);
/* The layout height at `now_ms`; *moving 0 once it will not change again
 * until the next report. Pure. */
float uttt_drawer_at(const UtttDrawer *d, int32_t now_ms, int32_t *moving);

/* Put the layout at rest at `h` now, no spring: the auto-collapse's slide
 * lays out at the compact height from its first frame (below). */
void  uttt_drawer_rest(UtttDrawer *d, float h);

/* THE AUTO-COLLAPSE IS A SLIDE ON THE COMPOSITOR, not a layout per frame.
 *
 * foolish's finding (ios/FoolishKit/Messages/CollapseLayer.swift, and
 * docs/COLLAPSE_MSE.md): the host moves the extension's view at the
 * composite rate (~90 Hz on the simulator) while the extension renders at
 * ~60, so any layout that follows the drawer per frame is a render behind on
 * a third of the frames - at 3.6 pt/ms that is ~30 points. So once the
 * collapse flips, the sheet is laid out at the COMPACT height for the whole
 * slide and a Core Animation keyframe animation pushes it down by the
 * drawer's remaining travel, so its bottom edge never moves; each element
 * takes its share of the push back on a layer of its own - the header all
 * of it, the board half of it while it scales about its centre, the doors
 * none - evaluated by the render server on the same frame.
 *
 * The push is the host's own spring (UTTT_DRAWER_RESPONSE_MS, critically
 * damped) from the whole travel down to nothing over UTTT_COLLAPSE_MS, in
 * UTTT_COLLAPSE_STEPS linear keyframes. A drop of more than
 * UTTT_COLLAPSE_FLIP points while an auto-collapse is armed is the flip;
 * anything else - a finger on the handle - is followed by the layout. */
#define UTTT_COLLAPSE_MS     600
#define UTTT_COLLAPSE_STEPS  120
#define UTTT_COLLAPSE_FLIP   60.f

/* How far the compact sheet is pushed down `t_ms` into a slide of `travel`
 * points: travel at 0, falling on the host's curve, 0 at UTTT_COLLAPSE_MS. */
float uttt_collapse_push(float travel, int32_t t_ms);

/* ONE LAYOUT FOR EVERY SCREEN, a pure function of the drawer's height.
 *
 * docs/UI.html, "What holds which edge": the header line holds the top, the
 * turn strip and the doors hold the bottom, and THE BOARD HOLDS THE CENTRE -
 * "348 to 214 is a scale, not a slide. It is the only element that
 * resizes." So the board's centre is the sheet's centre at every height,
 * compact, expanded and every frame of a drag between, on every screen
 * (waiting, play, end, spectator), and its side is a continuous function of
 * the height: min and max of lerps on the openness, never a branch on it.
 *
 * THE BOARD IS AS LARGE AS THE SHEET ALLOWS (owner, 2026-09-23): its side is
 * limited by the drawer's height less the grab handle's margin, and by its
 * width less the side columns, and by nothing else. The words never cost it
 * a point. On the strip they go BESIDE the board, in the room its square
 * leaves at one side, wrapped onto as many lines as that takes (`words`,
 * `words_side`); opening, they move into the header band, which the height
 * the expanded sheet has to spare pays for - it is the width that limits the
 * board there. The host sets the words inside the box it is given. */
enum {
    UTTT_SHEET_PLAY  = 0,  /* a seat: "you are" column, doors, headline     */
    UTTT_SHEET_WATCH = 1,  /* a spectator: the rulebook column, doors       */
    UTTT_SHEET_WAIT  = 2,  /* the waiting (first-open) screen: words only   */
};

typedef struct {
    float w, h;            /* the sheet, laid out at the drawer's height     */
    int32_t kind;          /* UTTT_SHEET_*                                   */
    int32_t words;         /* the strip carries words: 0 only for a live
                              seat, whose headline waits for the band       */
    int32_t hint;          /* a bubble waits in the field: the send hint may
                              stand in the top right corner                 */
} UtttSheetIn;

typedef struct {
    float t;               /* how far open, 0 compact .. 1 expanded          */
    float board[3];        /* x, y (top left) and side, sheet points         */
    float hpad, vpad;      /* the sheet's margins                            */
    float col;             /* the side columns ("you are", the rulebook)     */
    float bar;             /* the header band's height                       */
    float foot;            /* the door row's height                          */
    float door;            /* the rulebook door's side                       */
    float icon;            /* the "you are" mark's side                      */
    float icon_lead;       /* the gap between its label and the mark         */
    float icon_top;        /* how far the indicator sits below the margin    */
    float words_alpha;     /* the column copy of the words (`words`)         */
    float door_alpha;      /* the Again door: expanded only                  */
    float words[4];        /* x, y, w, h: the COLUMN beside the ink the
                              words are set in, wrapped to its width - the
                              play screen's on the right, the others' left  */
    int32_t words_side;    /* always 1: `words` is a column                 */
    float band[4];         /* x, y, w, h: the header band's copy of them    */
    float band_alpha;      /* its alpha. A copy shows only where it fits, so
                              a drag crossfades the two, never squeezes one */
} UtttSheet;

/* The drawer heights the openness runs between: 360, above the tallest
 * compact drawer (340, 323 with the keyboard), and 530, below the shortest
 * expanded one (541, an SE). */
#define UTTT_SHEET_LO 360.f
#define UTTT_SHEET_HI 530.f

void uttt_sheet(const UtttSheetIn *in, UtttSheet *out);

#endif
