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
} MtScore;

void mt_default_opts(MtScoreOpts *o);
/* Score one take; returns 0 when nothing moved and `whole` is off. */
int32_t mt_score(const MtRow *rows, int32_t n, const MtScoreOpts *o, MtScore out[MT_MARKS]);
/* The host spring's progress t seconds in (critically damped). */
double mt_host_progress(double t, double response);

/* ---- pace: how often the content actually changes -------------------- */
/* A screen recording keeps a frame only when the screen changed, so the
 * frames in which a box's pixels change ARE the frames our content was
 * drawn in, and the ink laid down in the box says how far a pen stroke got
 * in each of them. The benchmark for an animation's real frame rate. */
typedef struct { int32_t x, y, w, h; } MtBox;      /* pixels                 */

/* The box's ink: pixels with luma under `lum` (0..255) that are not a ruler
 * square; and a checksum of every pixel in it. */
int32_t mt_box_ink(const uint8_t *rgb, int32_t W, int32_t H, MtBox b, int32_t lum,
                   uint64_t *sum);

typedef struct {
    int32_t frames;       /* frames in which the box changed                 */
    double  t0, t1;       /* the first and the last change, s                */
    double  fps;          /* changes per second over [t0, t1]                */
    double  maxgap;       /* the longest wait between two changes, s         */
    double  maxstep;      /* the largest share of the final ink one frame laid */
    double  rough;        /* judder: sum of squared second differences of the
                             ink share as seen on a 60 Hz grid, x 1000       */
} MtPace;

/* From one row per recorded frame: its time, its box ink and checksum. */
void mt_pace(const double *t, const int32_t *ink, const uint64_t *sum, int32_t n,
             MtPace *out);

#endif
