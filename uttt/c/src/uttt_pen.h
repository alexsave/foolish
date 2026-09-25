/* The pen, in C, because both surfaces have to draw the same board.
 *
 * WHY THIS IS NOT IN SWIFT. Every mark is lopsided in its own way and every
 * grid line wanders on its own, all of it derived from one seed - and the two
 * phones looking at a bubble must produce the SAME sheet, stroke for stroke.
 * The moment the geometry lives in a renderer, "the bubble is the game"
 * becomes "the bubble is a picture of the game", and the reason for putting
 * the whole history in every message goes with it.
 *
 * So the kernel emits a DISPLAY LIST - filled polygons with a colour - and a
 * renderer's only job is to fill them. Core Graphics fills them on iOS and a
 * canvas fills them in the design document; neither decides anything.
 *
 * The sketchy geometry is a faithful port of rough.js (MIT, Preet Shihn),
 * down to its Lehmer generator, so a seed means the same thing here as it
 * does in the document that was drawn with the original.
 */
#ifndef UTTT_PEN_H
#define UTTT_PEN_H

#include <stdint.h>
#include <stddef.h>

typedef struct { float x, y; } UtttPt;

/* ---------------------------------------------------------- display list */
typedef struct {
    int      first;      /* index into pt[] */
    int      n;          /* points in this polygon */
    uint32_t rgba;       /* 0xRRGGBBAA */
} UtttPoly;

typedef struct {
    UtttPt   *pt;    int n_pt,   cap_pt;
    UtttPoly *poly;  int n_poly, cap_poly;
} UtttDL;

void uttt_dl_init(UtttDL *d, UtttPt *pt, int cap_pt,
                  UtttPoly *poly, int cap_poly);
void uttt_dl_reset(UtttDL *d);

/* ---------------------------------------------------------- rough.js port */
typedef struct {
    float roughness;        /* 1.0 is rough.js's default          */
    float bowing;           /* 1.0                                */
    float max_offset;       /* maxRandomnessOffset, 2.0           */
    float curve_tightness;  /* 0                                  */
    float curve_fitting;    /* 0.95                               */
    int   curve_step_count; /* 9                                  */
    int   single;           /* disableMultiStroke                 */
    int32_t seed;
    /* How finely a cubic is flattened. NOT a rough.js number - rough.js hands
     * a renderer cubics and stops. But this pen sets a width per SAMPLE, so the
     * sample count is what the grain and the width modulation are carried on,
     * and the design document picked one per shape: 22 for a mark's lines, 18
     * for a grid line, 24 for the winning line, 14 around an ellipse. Get it
     * wrong and the geometry still matches while the texture does not. */
    int   seg_line;         /* 22 */
    int   seg_curve;        /* 14 */
} UtttRough;

UtttRough uttt_rough_default(int32_t seed);

/* Flattened polylines. Each call appends 1..2 strokes (rough.js draws twice)
 * and returns how many it wrote; `out` receives {first,n} spans into `pts`. */
typedef struct { int first, n; } UtttSpan;

int uttt_rough_line(UtttRough *o, float x1, float y1, float x2, float y2,
                    UtttPt *pts, int cap, int *n_pts,
                    UtttSpan *out, int out_cap);

int uttt_rough_ellipse(UtttRough *o, float cx, float cy, float w, float h,
                       UtttPt *pts, int cap, int *n_pts,
                       UtttSpan *out, int out_cap);

/* rough.js `hachureFill`: a shape filled with parallel rough lines, `gap`
 * apart, at `angle` degrees. The polygon is NOT closed by the caller - the
 * last point joins the first, as it does for a rough.js polygon.
 *
 * The gap and the angle are in the polygon's own units and degrees, which is
 * why this takes both rather than a pre-rotated shape: rough.js rounds the
 * gap to a whole unit, so a fill is not scale-free and a caller that wants
 * the document's fill has to work in the document's units. */
enum { UTTT_HACHURE_POLY = 16,   /* points in a fillable polygon */
       UTTT_HACHURE_MAX  = 128 };/* scan lines before it gives up */

int uttt_rough_hachure(UtttRough *o, const UtttPt *poly, int np,
                       float gap, float angle,
                       UtttPt *pts, int cap, int *n_pts,
                       UtttSpan *out, int out_cap);

/* --------------------------------------------------------------- the pen */
typedef struct {
    float w, a;             /* base width and alpha                     */
    float vel, lift, liftp; /* velocity, landing arc and its sharpness   */
    float press, grain, dir, chat;
    float agrain;           /* alpha driven by the same grain            */
    float gfx, gfy;         /* grain anisotropy - the sheet's own numbers*/
    uint32_t ink;
} UtttPen;

UtttPen uttt_pen_92(void);      /* the locked one: 9.2 */

/* THE INK'S STRENGTH, raised once for every stroke (owner, TestFlight 1.0(6),
 * option e at .90: TESTFLIGHT_PLAN.md 20). The pen's own alpha is .8 and every
 * stroke's alpha - a mark's .8, a faded mark's .27, a minor grid line's .5, a
 * big mark's .62, a door's hachure .4 - is multiplied by .90/.80 and capped at
 * one, so their weights keep their proportions and only what would pass one
 * (the major lines' .9, the win line's .92, the last mark's 1) stays opaque.
 * Applied where a stroke gets its colour - uttt_ink and uttt_ribbon - and
 * nowhere else: the highlighter's wash is not ink. */
#define UTTT_INK_GAIN (.90f / .80f)

/* `rgba` with its alpha raised by UTTT_INK_GAIN, capped at 255. */
uint32_t uttt_ink_rgba(uint32_t rgba);

/* Lay a stroke down as ONE polygon - its outline, width per sample, round
 * ends, alpha the stroke's mean x UTTT_INK_GAIN - so nothing in a stroke
 * overlaps itself (uttt_pen.c says why). */
void uttt_ink(UtttDL *d, const UtttPt *pts, int n, const UtttPen *p);

/* The same stroke drawn in to its first `m` of `n` points: the outline of
 * what has been drawn, in the colour of the whole (so a stroke does not
 * change colour as it draws). m == n is uttt_ink. */
void uttt_ink_part(UtttDL *d, const UtttPt *pts, int n, int m, const UtttPen *p);

/* What the pen gives a sample: its width (t is 0..1 along the stroke) and
 * its alpha before the gain. For tests that rebuild a stroke's pieces. */
float uttt_pen_width(const UtttPen *p, float x, float y, float t);
float uttt_pen_alpha(const UtttPen *p, float x, float y);

/* Lay a stroke down as ONE polygon of constant width - what a canvas does
 * when it strokes a path. Its alpha is raised by UTTT_INK_GAIN too. */
void uttt_ribbon(UtttDL *d, const UtttPt *pts, int n, float w, uint32_t rgba);

/* The sheet's grain, shared by the pen and by anything else that wants it. */
float uttt_grain(float x, float y, float fx, float fy, int seed);

#endif
