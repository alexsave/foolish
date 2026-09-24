/* HOW A MOVE MOVES, as a pure function of a plan and a clock.
 *
 * docs/UI.html, "How it moves, before how it looks", is a grid of channels:
 * one move arrives on screen through several doors and each door has its own
 * motion. What animates, in what order and for how long is a decision about
 * the game, so it lives here beside the pen; a renderer asks "what does the
 * board look like at t" once a display frame and draws the answer. It never
 * schedules anything and never types a duration.
 *
 * Two things move, all on the board's own 0..1 square:
 *   - the new mark, drawn in (the same draw stopped early, uttt_draw.c);
 *   - the highlighter, ONE rect travelling from the block the move was
 *     played in to the block it sends the other player to (or growing to
 *     the whole sheet when they are freed) - after the ink lands, never with
 *     it.
 * No ring pulses round the destination (owner, 2026-09-23: "I don't like
 * how it looks"); the highlighter's travel is what says where you go.
 */
#ifndef UTTT_ANIM_H
#define UTTT_ANIM_H

#include "uttt.h"

/* THE TWO HALVES OF A MOVE (owner, 2026-09-23, after foolish's pre- and
 * post-settlement):
 *
 *   PRE, at stage (A) - everything the move DID, in order: the small mark
 *   draws; if it won its block, that block's big mark draws right after; if
 *   it won the game, the win line after that. The highlighter DOES NOT MOVE:
 *   it stays on the block the move was played in, so it is plain which block
 *   a re-tap can change the move within. What the move will DO to the other
 *   player is drawn as a promise instead - a pen outline, in the
 *   highlighter's own rect and colour, round the block they will be sent to
 *   (the whole sheet when they are freed; none when the game is over).
 *
 *   POST, at Send (B) - the ONLY thing that moves is the highlighter: it
 *   travels from the block the move was played in to the outlined one, and
 *   the outline fades as the tint arrives to replace it.
 *
 * The receiver (D, E, and my own bubble reopened, C) sees the whole move in
 * that order: small mark, big mark, win line, then the highlighter - and no
 * outline, because for them the move is not a promise. */
enum {
    UTTT_CH_STILL   = 0,   /* nothing moves: the resting board              */
    UTTT_CH_STAGE   = 1,   /* A: I tapped a square - the pre-settlement     */
    UTTT_CH_REPLAY  = 2,   /* C: I reopened my own bubble - my wash's pace  */
    UTTT_CH_THEIRS  = 3,   /* D: I opened a bubble of theirs                */
    UTTT_CH_ARRIVAL = 4,   /* E: their move landed while I was looking     */
    UTTT_CH_SETTLE  = 6,   /* B: I tapped Send - the post-settlement only  */
    UTTT_CH_DRAFT   = 7,   /* at rest, my last move staged and unsent: the
                              stage's last frame (outline and all)         */
};

/* THE TIMINGS, in milliseconds. From UI.html's grid ("the mark draws
 * (260/340ms)"); the grid's destination pulse is dropped (owner decision:
 * no pulse). The highlighter's travel is the Play
 * nib's .34 of its one-second move, and on their move the Motion tab's
 * freed-board .42, because "the wash takes longer to move, because you did
 * not choose it". */
#define UTTT_MS_INK_X        260
#define UTTT_MS_INK_O        340
#define UTTT_MS_WASH_MINE    340
#define UTTT_MS_WASH_THEIRS  420
/* THE SETTLEMENT (UI.html 04 "A board falls", 05 "The line"): third in a
 * line, then the big mark over the top of the block, and at the end of the
 * game the line across three blocks. The big mark draws over .46 to .92 of
 * the page's 1.7 s demo, the line over .1 to .85 of 1.3 s. */
#define UTTT_MS_FALL         780
#define UTTT_MS_LINE         500     /* strikein .5s                         */
/* THE PROMISE: the outline round the destination block, drawn round once by
 * the pen after everything the move did has landed. */
#define UTTT_MS_OUTLINE      420
/* THE REST before the drawer moves (owner, 2026-09-23: "let it breathe"):
 * from a move whose whole plan has run this long with nothing moving, so
 * the result reads, and only then the auto-collapse. foolish's `stage`
 * rests the same 500 ms after its board settles. */
#define UTTT_MS_REST         500

typedef struct {
    int32_t ch;
    int32_t mv;            /* block*9+cell, -1 when nothing is drawn in      */
    int32_t mark;          /* UTTT_X / UTTT_O                                */
    int32_t from, to;      /* wash blocks: 0..8, 9 anywhere, -1 none         */
    int32_t ink_ms;        /* the mark is drawn over [0, ink_ms]             */
    int32_t wash_at, wash_ms;
    int32_t end_ms;        /* nothing moves at or after this                 */
    int32_t fall_at;       /* the big mark of the block the move won; -1 none */
    int32_t line_at;       /* the win line; -1 none                          */
    int32_t outline;       /* the promised block (0..8, 9 the sheet), -1 none */
    int32_t outline_at;    /* it is drawn round over [at, at+UTTT_MS_OUTLINE];
                              -1: already drawn                             */
    int32_t outline_fade;  /* 1: it fades as the wash travels (B)            */
} UtttMotion;

typedef struct {
    float   mark_t;        /* 0..1 how far the new mark is drawn             */
    float   wash[4];       /* x, y, w, h; w == 0 for no wash                 */
    uint32_t wash_rgba;    /* the highlighter, alpha included (0xRRGGBBAA)   */
    int32_t landed;        /* 1 once the ink is down                         */
    int32_t settled;       /* 1 once nothing will move again - the host may
                              insert its bubble without stalling a stroke  */
    int32_t running;       /* 0 once nothing will change again               */
    float   fall_t;        /* 0..1 the big mark of the block the move won    */
    float   line_t;        /* 0..1 the win line                              */
    int32_t outline;       /* the promised block, -1 none                    */
    float   outline_t;     /* 0..1 how far round the pen has gone            */
    float   outline_a;     /* 0..1 its opacity (it fades at Send)            */
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

/* The highlighter's colour, with alpha. The outline is the same colour at
 * full strength (uttt_wash_rgba(1)) - one constant, so they cannot differ. */
uint32_t uttt_wash_rgba(float alpha);

/* THE SHEET IS LAID OUT AT THE HEIGHT MESSAGES HANDS IT, at once, always.
 *
 * A finger on the handle hands a height for every touch the host takes, and
 * the drawer on screen is exactly there. A release, a tap to expand and a
 * bubble opened hand the final height once and the host animates the
 * extension's view there itself, so content laid out at the final height at
 * once rides it. Measured with the ruler (TESTFLIGHT_PLAN.md 11): a spring
 * on the layout toward the handed height (it stood here until 2026-09-23)
 * put the board 35pt off the drawer's centre at a drag's release and the
 * door 17pt off its bottom on a tap to expand; at once, 0.7 and 1.1pt. The
 * one motion the host does not carry is the auto-collapse, which is ours
 * (below). The simulator's drag injection moves the drawer in 40-50 point
 * steps every ~140 ms; the extension's main thread is idle through them
 * (sampled), and the layout follows each step in the frame it lands. */
#define UTTT_DRAWER_RESPONSE_MS 338   /* the host's spring, docs/COLLAPSE_MSE.md */


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

/* THE EXPAND IS THE HOST'S, AND THE SHEET RIDES IT ON THE COMPOSITOR TOO.
 *
 * A tap to expand (or a drag's release upward) hands the tall height once,
 * and the host animates the extension's bounds from the height it had to the
 * new one on a spring of its own - read off the layer on the SE simulator
 * (TESTFLIGHT_PLAN 14): CASpringAnimation, mass 1, stiffness 333.3, damping
 * 36.5 (critically damped), no initial velocity, additive, 0.506 s. Content
 * laid out at the tall height from the first frame sat anchored to the
 * drawer's top while the drawer was still short: the board's corners 11.4
 * points off the drawer's scale. So the sheet is laid out at the final height
 * and every rider is carried, on the render server, by where the layout would
 * have put it for the drawer's height at each moment of the host's spring.
 *
 * How much of `travel` the host's spring still has to go `t_ms` in: travel at
 * 0, falling to (nearly) 0; any damping - under, critical or over - and an
 * initial velocity `v0` in the host's units (progress per second). The host
 * passes its own animation's numbers, so the curve is the one on screen. */
float uttt_spring_left(float travel, float mass, float stiffness, float damping,
                       float v0, int32_t t_ms);

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
    float rulebook[4];     /* x, y, w, h: the rulebook door, bottom right    */
    float again[4];        /* x, y, w, h: the Again door beside it, from the
                              left margin to SHEET_AGAIN_GAP short of the
                              rulebook - never flush with the sheet's edge,
                              which a phone's rounded corner cuts           */
} UtttSheet;

/* The drawer heights the openness runs between: 360, above the tallest
 * compact drawer (340, 323 with the keyboard), and 530, below the shortest
 * expanded one (541, an SE). */
#define UTTT_SHEET_LO 360.f
#define UTTT_SHEET_HI 530.f

void uttt_sheet(const UtttSheetIn *in, UtttSheet *out);

/* WHERE THE SEND HINT'S CONTAINER STARTS, down from the drawer's top. The
 * hint (shared/swift/MessagesKit/SendHintMetrics) rests its arrow's ink 11.6
 * points above its container's top - lifted 9 into the margin, 1.6 of ring
 * and 1 of air - and its bob lifts it 14 more at the crest. At 14 the crest
 * ran 11.6 points above the drawer, which Messages clips: the arrowhead was
 * cut flat at every bob (the release pass, 2026-09-23). 28 puts the crest's
 * ink 2.4 points under the edge - where the rest used to be - so the arrow's
 * highest point is the old rest and nothing of it is ever cut. */
#define UTTT_SHEET_HINT_TOP   28.f
#define UTTT_HINT_LIFT        11.6f   /* the arrow's ink above its container  */
#define UTTT_HINT_BOB         14.f    /* SendHintMetrics.bobTravel             */

#endif
