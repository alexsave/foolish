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
/* FAINTER THAN THE DESIGN DOCUMENT'S, and the document is what is wrong.
 *
 * Its square is #25376b hachured at a gap of four with strokes 1.4 wide,
 * drawn twice as every rough.js line is - which at the 54 points this button
 * actually ships at closes up into a solid navy block. The pale book then
 * sits ON that block and cannot be seen at all: Chrome renders the document's
 * own version exactly the same way, so this was never a porting error.
 *
 * So the fill drops to two fifths and lets the paper back through, the
 * outline stays solid because the edge is what makes it a button, and the
 * book inverts - the dark ink rather than a pale page - because a glyph
 * reads against a light field and disappears into a dark one. "Fill in the
 * rulebook icon itself, and use a fainter colour" was the instruction; the
 * fainter colour belonged to the square. */
#define INK     0x25376b66u             /* the square's fill, 40%         */
#define EDGE    0x1b2a52ffu             /* and its outline                */
#define PAGE    0x1b2a52d8u             /* the book, dark on the light fill */

/* How finely a cubic is flattened, which is NOT a rough.js number - see
 * uttt_pen.h. MEASURED against the document, by rasterising the button at
 * twelve times its size and differencing it with the same shape drawn by
 * rough.js in a browser: mean channel error 3.47 at 6 samples, 2.73 at 10,
 * 2.65 at 14, 2.65 at 22. Fourteen is where the curve goes flat, and the
 * 2.65 that is left is antialiasing, not geometry. */
#define SEG 14

/* One shape's working buffers. MEASURED, like the display list's: a 54-point
 * rulebook is 42 strokes and 1,260 samples in its biggest shape; the Again
 * bar is the biggest shape of all, and these hold it up to the 430-point
 * width ios-smoke asserts. Past that the fill runs out of strokes and the
 * call returns -1 having drawn what fit, which is the same contract the
 * board has. */
#define MAX_SPAN  256
#define MAX_SAMP  6400

typedef struct {
    uint32_t fill, stroke;
    float    gap, angle, weight, stroke_w, roughness, bowing;
    int32_t  seed;
    /* A four-sided shape whose bottom edge is its top edge reflected, and
     * whose right edge is its left: two independent rough lines wobble by
     * different amounts, so opposite sides of a long bar read as different
     * weights (owner, 2026-09-23, of the Again door). */
    int      mirror;
} Rule;

/* One stroke, one polygon, at the flat width rough.js strokes with - not the
 * board's pen, whose velocity and lift would read at 54 points as a wobble
 * rather than as a hand, and whose overlapping quads would blend a 55% page
 * into an 80% one. */
/* `flip` 1 reflects the spans top to bottom, 2 left to right.
 *
 * THE RIBBON IS BUILT IN POINTS AND ONLY THEN DIVIDED BY THE SIZE. A door is
 * handed back in 0..1 of ITS OWN width and height, and the host stretches
 * that square to the bar (UtttInkImage, `square: false`) - x by w, y by h.
 * The ribbon used to be built in those unit coordinates with its width as a
 * fraction of w, so a stroke's thickness along y came back multiplied by h/w:
 * on a 141 x 46 door the top and bottom edges measured 2.5 pixels at 2x and
 * the left and right 6.4 (tools/uttt_look `door`), the "horizontal borders
 * look thinner" the owner reported four times. Mirroring the edges made the
 * two horizontals match each other and could not make them match the
 * verticals. Offsetting the sides in points, where the width is a width in
 * every direction, and transforming the POINTS afterwards is the one rule. */
static void emit_flip(UtttDL *d, const UtttPt *pts, const UtttSpan *sp, int n,
                      float w, float h, uint32_t rgba, float width, int flip)
{
    UtttPt u[256];
    for (int i = 0; i < n; i++) {
        int m = sp[i].n > 256 ? 256 : sp[i].n;
        for (int k = 0; k < m; k++) {
            float x = pts[sp[i].first + k].x, y = pts[sp[i].first + k].y;
            u[k].x = flip == 2 ? w - x : x;
            u[k].y = flip == 1 ? h - y : y;
        }
        int first = d->n_pt;
        uttt_ribbon(d, u, m, width, rgba);
        for (int k = first; k < d->n_pt; k++) { d->pt[k].x /= w; d->pt[k].y /= h; }
    }
}

static void emit(UtttDL *d, const UtttPt *pts, const UtttSpan *sp, int n,
                 float w, float h, uint32_t rgba, float width)
{
    emit_flip(d, pts, sp, n, w, h, rgba, width, 0);
}

/* HOW MUCH INK AN EDGE PUTS ACROSS ITSELF. rough.js draws every line twice,
 * and the two passes lie a seeded distance apart: where they overlap the
 * edge is one stroke wide, where they part it is the stroke plus the gap up
 * to two strokes. So two edges drawn with ONE pen width do not look one
 * weight - on the door the short vertical edges' passes part further than
 * the long horizontal ones' and they read heavier (owner, four times: "the
 * horizontal borders appear less thick"). This is the ink a pair of passes
 * of width `wd` lays across the edge, averaged over the middle 80% of it
 * (the corners, where edges cross, belong to neither): wd + min(gap, wd). */
static float edge_ink(const UtttPt *pts, const UtttSpan *two, float wd)
{
    const UtttSpan a = two[0], b = two[1];
    float sum = 0.f; int n = 0;
    for (int k = a.n / 10; k <= a.n - 1 - a.n / 10; k++) {
        UtttPt p = pts[a.first + k];
        float best = 1e30f;
        for (int j = 0; j + 1 < b.n; j++) {        /* nearest point of pass B */
            UtttPt q0 = pts[b.first + j], q1 = pts[b.first + j + 1];
            float dx = q1.x - q0.x, dy = q1.y - q0.y, L2 = dx * dx + dy * dy;
            float t = L2 > 0.f ? ((p.x - q0.x) * dx + (p.y - q0.y) * dy) / L2 : 0.f;
            t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
            float ex = q0.x + t * dx - p.x, ey = q0.y + t * dy - p.y;
            float d2 = ex * ex + ey * ey;
            if (d2 < best) best = d2;
        }
        float gap = sqrtf(best);
        sum += wd + (gap < wd ? gap : wd);
        n++;
    }
    return n ? sum / n : wd;
}

/* The width that makes a pair of passes lay `want` across: edge_ink rises
 * with the width (by one to two per unit), so a bisection finds it. */
static float width_for(const UtttPt *pts, const UtttSpan *two, float want)
{
    float lo = want * .25f, hi = want;
    for (int i = 0; i < 40; i++) {
        float mid = .5f * (lo + hi);
        if (edge_ink(pts, two, mid) < want) lo = mid; else hi = mid;
    }
    return .5f * (lo + hi);
}

/* THE FOUR EDGES OF A DOOR ARE ONE WEIGHT. The ink the pen would lay at its
 * own width is taken for the top pair and the left pair, their mean is the
 * weight, and each pair gets the width that lays exactly that - so what is
 * equal is the thickness the eye reads, not a constant typed twice. Measured
 * in pixels by tools/uttt_look `door`, and held from the display list by
 * uttt_test. */
static void even_edges(const UtttPt *pts, const UtttSpan *top, const UtttSpan *left,
                       float wd, float *w_top, float *w_left)
{
    float want = .5f * (edge_ink(pts, top, wd) + edge_ink(pts, left, wd));
    *w_top  = width_for(pts, top, want);
    *w_left = width_for(pts, left, want);
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
    if (r->mirror && np == 4 && n_edge == 8) {
        /* Spans 0-1 are the top edge's two passes, 6-7 the left's (the edges
         * run p0->p1->p2->p3->p0). The right and the bottom are dropped and
         * drawn as those two reflected, so each pair is one weight. */
        float wt = r->stroke_w, wl = r->stroke_w;
        even_edges(pts, sp, sp + 6, r->stroke_w, &wt, &wl);
        emit_flip(d, pts, sp,     2, w, h, r->stroke, wt, 0);
        emit_flip(d, pts, sp,     2, w, h, r->stroke, wt, 1);
        emit_flip(d, pts, sp + 6, 2, w, h, r->stroke, wl, 0);
        emit_flip(d, pts, sp + 6, 2, w, h, r->stroke, wl, 2);
    } else {
        emit(d, pts, sp, n_edge, w, h, r->stroke, r->stroke_w);
    }
    return n >= MAX_SAMP || n_edge + n_fill >= MAX_SPAN;
}

int uttt_draw_rulebook(UtttDL *d, float w, float h)
{
    if (w < 1.f || h < 1.f) return -1;
    int over = 0;

    /* EVERY NUMBER IS A FRACTION OF THE SQUARE, and the version that was not
     * is why the book vanished at the size it actually ships in the collapsed
     * strip. The design document draws this at 54 points and its coordinates
     * were copied as POINTS - so at 30 the left leaf's four corners came out
     * at x = 15, x = w/2 = 15, x = 15, x = 15. A polygon of zero width. It
     * did not draw badly; it could not draw at all.
     *
     * So the shape is fractions and the PEN is what scales with the square:
     * a hachure gap and a stroke width are lengths, and a drawing half the
     * size wants half of each or it fills in. `k` is the size this was drawn
     * at, and everything in points is multiplied by it. */
    const float k = (w < h ? w : h) / 54.f;
    const float in = 4.f * k;

    const UtttPt sq[4] = {
        { in, in }, { w - in, in }, { w - in, h - in }, { in, h - in }
    };
    const Rule square = { INK, EDGE, 4.2f * k, -41.f,
                          1.4f * k, 1.8f * k, 1.5f, 1.3f, 19, 0 };
    over |= shape(d, sq, 4, w, h, &square);

    /* The book: two leaves off one spine, each hachured the other way so the
     * fold reads without a line down the middle. Drawn in the same ink as the
     * outline, over a fill thin enough to read through. */
    const float lx = w * (15.f / 54.f), rx = w - lx, mx = w / 2.f;
    const float ty = h * (17.f / 54.f), sy = h * (20.f / 54.f);
    const float by = h * (39.f / 54.f), oy = h * (36.f / 54.f);

    const UtttPt left[4]  = { { lx, ty }, { mx, sy }, { mx, by }, { lx, oy } };
    const UtttPt right[4] = { { rx, ty }, { mx, sy }, { mx, by }, { rx, oy } };
    const Rule leaf_l = { PAGE, PAGE, 2.6f * k,  38.f, .9f * k, 1.3f * k,
                          1.3f, 1.f, 23, 0 };
    const Rule leaf_r = { PAGE, PAGE, 2.6f * k, -38.f, .9f * k, 1.3f * k,
                          1.3f, 1.f, 29, 0 };
    over |= shape(d, left,  4, w, h, &leaf_l);
    over |= shape(d, right, 4, w, h, &leaf_r);

    return over ? -1 : 0;
}

/* The other door: Again, at the foot of a finished game's expanded sheet.
 *
 * THE SAME PEN AS THE RULEBOOK SQUARE, because the two sit side by side on one
 * piece of paper. docs/UI.html draws it as a CSS slab (`.udoor go`, solid
 * #25376b with pale type), and a slab was the one printed thing left in the
 * frame; the owner asked for every button to come off the nib. So it is the
 * rulebook's square stretched to a bar - a rough outline in the dark ink over
 * a two-fifths hachure - and the label is set dark on it by the caller, for
 * the reason the book is dark: a glyph reads against a light field.
 *
 * IN POINTS, NOT FRACTIONS, unlike the rulebook. The rulebook shrinks with the
 * collapsed strip and scales its pen to stay a book; this door lives in the
 * expanded view only and only its width changes with the phone, so a width
 * change must buy more hachure lines at the SAME gap, not fatter ones. The
 * pen is therefore the rulebook's at its 54-point size (k = 1). */
int uttt_draw_door(UtttDL *d, float w, float h)
{
    if (w < 1.f || h < 1.f) return -1;
    const float in = 2.5f;              /* room for the edge's wobble */
    const UtttPt bar[4] = {
        { in, in }, { w - in, in }, { w - in, h - in }, { in, h - in }
    };
    /* The edge is heavier than the rulebook's 1.8: a long, nearly straight
     * rough line lays its two passes almost on top of each other, so at 1.8
     * the bar's top and bottom read as the last hachure rather than as the
     * box's edge (owner: "a hatched band with no edges"). */
    const Rule door = { INK, EDGE, 4.2f, -41.f, 1.4f, 2.6f, 1.5f, 1.3f, 37, 1 };
    return shape(d, bar, 4, w, h, &door) ? -1 : 0;
}
