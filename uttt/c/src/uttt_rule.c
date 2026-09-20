/* The rulebook door: a hachured square with a hachured book on it.
 *
 * WHY IT IS HERE AND NOT IN A RENDERER. It is the same sheet of paper as the
 * board and the same hand, so a flat rectangle would be the only printed
 * thing in the frame - and the moment one surface computes the squiggle, the
 * two phones looking at a bubble stop drawing the same one. Same argument as
 * uttt_pen.h, same answer: the kernel emits polygons and Swift fills them.
 *
 * IT IS BUILT IN POINTS AND HANDED BACK IN 0..1. Every literal below is a
 * point measurement out of docs/UI.html - the 4-point inset, the book's 15
 * and 17, the 4.2 and 2.6 hachure gaps - and rough.js rounds a hachure gap
 * to a whole unit, so this shape is NOT scale-free: computing it in the unit
 * square and stretching would give a different fill at every size. So the
 * caller hands over the size the button actually has, the shape is drawn at
 * that size, and the points are divided by it on the way out like every
 * other drawn thing in this kernel.
 */
#include "uttt_draw.h"
#include <math.h>

/* docs/UI.html, initRbook(). Copied, not retyped. */
#define INK     0x25376bffu             /* the square's fill              */
#define EDGE    0x1b2a52ffu             /* and its outline                */
#define PAGE    0xe2e8f48cu             /* rgba(226,232,244,.55)          */

/* How finely a cubic is flattened, which is NOT a rough.js number - see
 * uttt_pen.h. MEASURED against the document, by rasterising the button at
 * twelve times its size and differencing it with the same shape drawn by
 * rough.js in a browser: mean channel error 3.47 at 6 samples, 2.73 at 10,
 * 2.65 at 14, 2.65 at 22. Fourteen is where the curve goes flat, and the
 * 2.65 that is left is antialiasing, not geometry. */
#define SEG 14

/* One shape's working buffers. MEASURED, like the display list's: a 54-point
 * button is 42 strokes and 1,260 samples in its biggest shape, and these hold
 * a button up to about 125 points - past that the fill runs out of strokes
 * and the call returns -1 having drawn what fit, which is the same contract
 * the board has. The door is 54 points and is not a thing that resizes. */
#define MAX_SPAN  96
#define MAX_SAMP  2400

typedef struct {
    uint32_t fill, stroke;
    float    gap, angle, weight, stroke_w, roughness, bowing;
    int32_t  seed;
} Rule;

/* One stroke, one polygon, at the flat width rough.js strokes with - not the
 * board's pen, whose velocity and lift would read at 54 points as a wobble
 * rather than as a hand, and whose overlapping quads would blend a 55% page
 * into an 80% one. */
static void emit(UtttDL *d, const UtttPt *pts, const UtttSpan *sp, int n,
                 float w, float h, uint32_t rgba, float width)
{
    UtttPt u[256];
    for (int i = 0; i < n; i++) {
        int m = sp[i].n > 256 ? 256 : sp[i].n;
        for (int k = 0; k < m; k++) {
            u[k].x = pts[sp[i].first + k].x / w;
            u[k].y = pts[sp[i].first + k].y / h;
        }
        uttt_ribbon(d, u, m, width / w, rgba);
    }
}

/* One filled, outlined shape.
 *
 * rough.js BUILDS THE OUTLINE FIRST AND PAINTS IT LAST, and both halves of
 * that matter: the build order is what the seed feeds, so swapping it gives a
 * different squiggle from the document, and the paint order is what puts the
 * edge on top of its own fill. */
static int shape(UtttDL *d, const UtttPt *p, int np, float w, float h,
                 const Rule *r)
{
    UtttPt   pts[MAX_SAMP];
    UtttSpan sp[MAX_SPAN];
    int n = 0, n_edge = 0, n_fill = 0;

    UtttRough o = uttt_rough_default(r->seed);
    o.roughness = r->roughness;
    o.bowing    = r->bowing;
    o.seg_line  = SEG;

    for (int i = 0; i < np && n_edge + 2 <= MAX_SPAN; i++)
        n_edge += uttt_rough_line(&o, p[i].x, p[i].y,
                                  p[(i + 1) % np].x, p[(i + 1) % np].y,
                                  pts, MAX_SAMP, &n, sp + n_edge, 2);

    n_fill = uttt_rough_hachure(&o, p, np, r->gap, r->angle,
                                pts, MAX_SAMP, &n, sp + n_edge,
                                MAX_SPAN - n_edge);

    emit(d, pts, sp + n_edge, n_fill, w, h, r->fill,   r->weight);
    emit(d, pts, sp,          n_edge, w, h, r->stroke, r->stroke_w);
    return n >= MAX_SAMP || n_edge + n_fill >= MAX_SPAN;
}

int uttt_draw_rulebook(UtttDL *d, float w, float h)
{
    if (w < 1.f || h < 1.f) return -1;
    int over = 0;

    const UtttPt sq[4] = {
        { 4.f, 4.f }, { w - 4.f, 4.f }, { w - 4.f, h - 4.f }, { 4.f, h - 4.f }
    };
    const Rule square = { INK, EDGE, 4.2f, -41.f, 1.4f, 1.8f, 1.5f, 1.3f, 19 };
    over |= shape(d, sq, 4, w, h, &square);

    /* The book: two leaves off one spine, each hachured the other way so the
     * fold reads without a line down the middle. Faint, because it lies ON a
     * hachured square and a bright glyph would fight the fill underneath. */
    const UtttPt left[4] = {
        { 15.f, 17.f }, { w / 2.f, 20.f }, { w / 2.f, h - 15.f }, { 15.f, h - 18.f }
    };
    const UtttPt right[4] = {
        { w - 15.f, 17.f }, { w / 2.f, 20.f }, { w / 2.f, h - 15.f },
        { w - 15.f, h - 18.f }
    };
    const Rule leaf_l = { PAGE, PAGE, 2.6f,  38.f, .9f, 1.3f, 1.3f, 1.f, 23 };
    const Rule leaf_r = { PAGE, PAGE, 2.6f, -38.f, .9f, 1.3f, 1.3f, 1.f, 29 };
    over |= shape(d, left,  4, w, h, &leaf_l);
    over |= shape(d, right, 4, w, h, &leaf_r);

    return over ? -1 : 0;
}
