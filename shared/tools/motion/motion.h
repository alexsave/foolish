/* motion.h - read the debug ruler back off filmed frames, and score it.
 *
 * The ruler (shared/c/motion_ruler/motion_ruler.h, painted by
 * shared/swift/MotionRuler.swift) puts a red bar on a moving container's top
 * edge, a green bar on its bottom, a millisecond clock under the red bar and
 * a 12pt saturated square on every element that moves. This finds them in a
 * frame (one pass: every pixel classified by hue once, one labelling of the
 * whole ink map), and scores a take: does every mark ride its anchor.
 *
 * Product-free. No JSON anywhere: a take's marks are a fixed-layout text
 * table (motion_write_table), one row per composited frame. */
#ifndef MOTION_H
#define MOTION_H

#include <stdint.h>
#include "motion_ruler.h"

/* Every mark a frame can report: each square ink on its own, and split by
 * quadrant around the magenta square when an ink appears more than once. */
enum { MT_Q_ONE = 0, MT_Q_TL, MT_Q_TR, MT_Q_BL, MT_Q_BR, MT_QUADS };
enum { MT_MARKS = MR_SQUARE_INKS * MT_QUADS };
#define MT_NONE (-1e9)                 /* a mark or bar not in this frame   */

typedef struct {
    double t;                          /* the frame's presentation time, s  */
    int32_t clock;                     /* the ruler's clock, -1 unreadable  */
    double red, green;                 /* the bars' y, points, or MT_NONE   */
    double x[MT_MARKS], y[MT_MARKS];   /* each mark's centre, points        */
} MtRow;

/* The mark index for square ink i (0..MR_SQUARE_INKS-1) in quadrant q, and
 * its name ("magenta", "cyan_tl"), written into buf. */
static inline int32_t mt_mark(int32_t i, int32_t q) { return i * MT_QUADS + q; }
const char *mt_mark_name(int32_t mark, char *buf, int32_t n);
int32_t mt_mark_by_name(const char *name);

/* Find everything in one frame of `w` x `h` packed RGB. `scale` is pixels per
 * point (3 on a 1320-wide 6.9" phone, 2 below 2000px tall). The squares are
 * searched at half resolution, as a 12pt square is still 18px across at 3x. */
void mt_find(const uint8_t *rgb, int32_t w, int32_t h, double scale, MtRow *row);

/* The fixed-layout table: a header line naming every column, then one line
 * per row; a missing value is "-". */
void mt_write_header(void *file);
void mt_write_row(void *file, const MtRow *r);
/* Read a table back; returns the number of rows (at most cap), or -1. */
int32_t mt_read_table(const char *path, MtRow *rows, int32_t cap);

/* ---- scoring ---------------------------------------------------------- */

enum { MT_ANCHOR_RED = 0, MT_ANCHOR_GREEN, MT_ANCHOR_MID, MT_ANCHOR_NONE };

typedef struct {
    double span;          /* seconds after the first move (ride.py --span)  */
    double snap;          /* a snap is a change of the offset above this, pt */
    int32_t whole;        /* score the whole take (the drawer need not move) */
    double response;      /* the host spring for the jerk floor, s (0.338)   */
    int32_t anchor[MT_MARKS];
    /* A board mark off the centre scales with the board: its expected offset
     * from the anchor is its offset at the take's shorter end times the ratio
     * of `side(h)` at the two heights, h the bars' distance plus a constant
     * fitted 0..24pt. NULL side: every offset is rigid. */
    const double *side;   /* side[h - side_h0] for h in side_h0..side_h1   */
    int32_t side_h0, side_h1;
    int32_t scaled[MT_MARKS];
} MtScoreOpts;

typedef struct {
    int32_t seen;         /* the mark was scored in this take               */
    /* ride.py, against the mark's own anchor */
    int32_t snaps, late, miss;
    double maxsnap, rough;
    double maxstep;       /* the largest change of the offset, snap or not   */
    /* bars.py, on the mark's y */
    double jerk, floor, stray, travel;
    /* frames the mark sits outside the visible drawer (above the red bar or
     * below the green, by more than MT_OFF_TOL): it has left the drawer */
    int32_t off;
} MtScore;
#define MT_OFF_TOL 2.0

/* THE BOARD'S SIZE, from the four corner squares (cyan_tl/tr/bl/br): its
 * width (the top pair's and the bottom pair's x distance, averaged over the
 * pairs seen) and its height (the left and right pairs' y distance), per
 * frame, over the same window as mt_score. A size that follows the drawer
 * changes direction only where the drawer does, so a reversal the drawer
 * does not make is a wobble; a step is the largest one-frame change. */
#define MT_REV_TOL 1.0      /* pt: a turn smaller than this is not a reversal */
typedef struct {
    int32_t seen;           /* frames with a width or a height                 */
    double w_maxstep, h_maxstep;
    int32_t w_rev, h_rev;   /* reversals of the width and of the height        */
    int32_t drawer_rev;     /* reversals of the drawer's height (green - red)  */
    double w_rough, h_rough;   /* sum of squared second differences         */
    double w_first, w_last, h_first, h_last;
    double maxskew;         /* the largest |width - height| seen               */
    /* with a side table (MtScoreOpts.side): the size against the product's
     * side for the drawer's height at that frame, the two agreeing at the
     * take's first frame (the corner squares sit a constant inside the
     * board, so the size is the side less a constant) - the largest one-frame step of the residual
     * (what the size did that the drawer did not ask for), and its largest
     * absolute value */
    double w_res_step, h_res_step, w_res_max, h_res_max;
} MtBoard;

/* The board's size in one frame (MT_NONE when no pair is seen). */
double mt_board_w(const MtRow *r);
double mt_board_h(const MtRow *r);
/* Score the size over a take; returns 0 when the window is empty. */
int32_t mt_board(const MtRow *rows, int32_t n, const MtScoreOpts *o, MtBoard *out);
/* Reversals of a series with hysteresis `tol` (MT_NONE entries skipped). */
int32_t mt_reversals(const double *v, int32_t n, double tol);

void mt_default_opts(MtScoreOpts *o);
/* THE DRAWER'S BOTTOM IS THE SCREEN'S: a Messages drawer's lower edge never
 * moves (only its top does), but the green bar is painted by the content,
 * so content that jumps takes the bar with it and then scores as riding it.
 * This puts a row's green at `y` points (where the drawer's bottom is at
 * rest) wherever the red bar is seen and the painted bar is further than
 * MT_CARD_SHIFT from it. Within that the painted bar is the drawer's: the
 * host moves its whole card that far when the drawer changes style (compact
 * 913 against expanded 919.67 on a 956-point phone, the card's top keeping
 * its place over the content), and the content moves with it.
 * BELOW THE COMPACT HEIGHT the host slides a rigid card down (a drag past
 * compact, a flick to the minimised drawer): the bottom is then the top plus
 * the compact drawer's bars' distance `hc` (0: no such zone). */
void mt_fix_bottom(MtRow *rows, int32_t n, double y, double hc);
#define MT_CARD_SHIFT 8.0
/* Score one take; returns 0 when nothing moved and `whole` is off. */
int32_t mt_score(const MtRow *rows, int32_t n, const MtScoreOpts *o, MtScore out[MT_MARKS]);
/* The host spring's progress t seconds in (critically damped). */
double mt_host_progress(double t, double response);

/* ---- grid: a take with no ruler (a device recording) --------------------
 * The drawer's top edge and the board's four heavy grid lines, found in a
 * frame of a real screen recording, so a take filmed without the ruler (the
 * owner's phone) still says where the board is and how big. Paper is light
 * and neutral; the heavy grid lines are dark and neutral (the marks, the win
 * line and the doors are coloured ink, which this skips); anything above the
 * drawer's top (a dark chat wallpaper) is not looked at. */
typedef struct {
    double t;
    double top;              /* the drawer's top edge, pt, or MT_NONE       */
    double h1, h2, v1, v2;   /* the heavy lines: two rows, two columns, pt  */
} MtGrid;
#define MT_GRID_PAPER_MIN   225   /* every channel at least this: paper      */
#define MT_GRID_PAPER_SPREAD 20   /* and this neutral                        */
#define MT_GRID_INK_MAX     110   /* every channel under this: grid ink      */
#define MT_GRID_INK_SPREAD   45   /* and this neutral                        */
void mt_grid(const uint8_t *rgb, int32_t w, int32_t h, double scale, MtGrid *g);
/* The board's centre and side from a frame's lines (the heavy lines cut the
 * board in thirds): 0 when the lines were not all found. */
int32_t mt_grid_board(const MtGrid *g, double *cx, double *cy, double *side);

/* ---- pace: how often the content actually changes -------------------- */
/* A screen recording keeps a frame only when the screen changed, so the
 * frames in which a box's pixels change ARE the frames our content was
 * drawn in, and the ink laid down in the box says how far a pen stroke got
 * in each of them. The benchmark for an animation's real frame rate. */
typedef struct { int32_t x, y, w, h; } MtBox;      /* pixels                 */

/* The box's ink: pixels with luma under `lum` (0..255) that are not a ruler
 * square. */
int32_t mt_box_ink(const uint8_t *rgb, int32_t W, int32_t H, MtBox b, int32_t lum);

/* How many of the box's pixels differ between two frames by more than `tol`
 * in some channel. A recording is h264: an unchanged screen still shimmers
 * a level or two, so a change is a count of real differences, never "any
 * byte moved". */
#define MT_PACE_TOL   40      /* levels: well above the codec's shimmer        */
#define MT_PACE_MIN    6      /* pixels: a frame changed when this many did    */
int32_t mt_box_diff(const uint8_t *a, const uint8_t *b, int32_t W, int32_t H, MtBox box,
                    int32_t tol);

typedef struct {
    int32_t frames;       /* frames in which the box changed                 */
    double  t0, t1;       /* the first and the last change, s                */
    double  fps;          /* changes per second over [t0, t1]                */
    double  maxgap;       /* the longest wait between two changes, s         */
    double  maxstep;      /* the largest share of the final ink one frame laid */
    double  rough;        /* judder: sum of squared second differences of the
                             ink share as seen on a 60 Hz grid, x 1000       */
} MtPace;

/* From one row per recorded frame: its time, its box ink, and whether the
 * box changed from the frame before (mt_box_diff >= MT_PACE_MIN). */
void mt_pace(const double *t, const int32_t *ink, const int32_t *changed, int32_t n,
             MtPace *out);

#endif
