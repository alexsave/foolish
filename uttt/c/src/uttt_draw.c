#include "uttt_draw.h"
#include "uttt_anim.h"
#include "uttt_lang.h"
#include <math.h>
#include <string.h>

#define S   1.0f                 /* the board is a unit square */
#define BL  (S / 3.f)            /* a block */
#define CE  (S / 9.f)            /* a cell  */

#define INK_X   0x25376bffu
#define INK_O   0xa8321fffu
#define INK_GR  0x2f2b26ffu

/* One pen at two scales. A mark is drawn inside a CELL and the grid across the
 * whole BOARD, which is nine cells - so the same ball that is 2.7 units wide
 * on a mark is 2.7/9 here. Derived, never typed: getting this wrong by hand is
 * what made the first grid a thicket. */
/* The O, in its hundred-unit square: lopsided by the same .3 as the X */
#define O_CX (50.f + .3f * 4.f)
#define O_CY (50.f - .3f * 3.f)
#define O_W  (78.f - .3f * 10.f)
#define O_H  (76.f + .3f * 8.f)

/* A seed derived from another: s x k + a, wrapped as the hardware wraps it.
 * Signed overflow is undefined in C (the asan lane's UBSan flags it), so the
 * sum is taken unsigned and converted back - the same bits as before. */
static int32_t sd(int32_t s, uint32_t k, int32_t a) { return (int32_t)((uint32_t)s * k + (uint32_t)a); }

#define REF   (8.9f / 100.f * CE * 9.f)   /* a normal mark, in board units */

static float rough_for(float L) { return 1.5f * powf(REF / (L > 1e-4f ? L : 1e-4f), .75f); }
static float bow_for  (float L) { return 1.0f * powf(REF / (L > 1e-4f ? L : 1e-4f), .85f); }
/* rough.js's maxRandomnessOffset of 2 is in the drawing's OWN units, and the
 * board here is a unit square rather than a hundred of them - so it converts
 * once, here, instead of at four call sites that would each forget. */
static float mro_for  (float L) { return .02f * powf(REF / (L > 1e-4f ? L : 1e-4f), .75f); }

uint32_t uttt_mark_ink(int mark) { return mark == UTTT_O ? INK_O : INK_X; }

UtttDrawOpts uttt_draw_opts(int32_t seed)
{
    UtttDrawOpts o;
    o.seed = seed ? seed : 1;
    o.active = -1; o.last = -1; o.mark_t = 1.f; o.meta_t = 1.f; o.fall_t = 1.f;
    o.reach = UTTT_REACH;
    return o;
}

static void rect(UtttDL *d, float x, float y, float w, float h, uint32_t rgba)
{
    if (d->n_poly >= d->cap_poly || d->n_pt + 4 > d->cap_pt) return;
    UtttPoly *p = &d->poly[d->n_poly++];
    p->first = d->n_pt; p->n = 4; p->rgba = rgba;
    d->pt[d->n_pt++] = (UtttPt){ x,     y     };
    d->pt[d->n_pt++] = (UtttPt){ x + w, y     };
    d->pt[d->n_pt++] = (UtttPt){ x + w, y + h };
    d->pt[d->n_pt++] = (UtttPt){ x,     y + h };
}

/* Draw a stroke, optionally only part of it - which is how an animation works
 * here: it is THE SAME DRAW, stopped early, not a finished stroke revealed. */
static void stroke(UtttDL *d, const UtttPt *pts, int n, const UtttPen *p, float t)
{
    if (t <= 0.f) return;
    int m = (int)ceilf((n - 1) * (t > 1.f ? 1.f : t)) + 1;
    if (m > n) m = n;
    if (m < 2) return;
    uttt_ink_part(d, pts, n, m, p);
}

/* A MARK IS DRAWN IN ITS OWN HUNDRED-UNIT SQUARE and then transformed into
 * the board, which is exactly what the document does with ctx.scale. Doing it
 * the other way round means every rough.js constant - its maxRandomnessOffset
 * of 2, its ellipse offsets of 1 and 1.5 - has to be converted at the call
 * site, and the ellipse ones were not, so every O wandered across the whole
 * sheet. Convert the POINTS once instead of the constants everywhere. */
/* The mark's strokes in its own hundred-unit square, before placing: `k`
 * spans for the first (or only) stroke and `k2` for an X's second. */
static int mark_geom(int kind, float s, int32_t seed, UtttPt *pts, int cap,
                     UtttSpan *sp, int *k2)
{
    int np = 0;
    const float s100 = s * 100.f;          /* the mark's size, board-100 */
    UtttRough r = uttt_rough_default(sd(seed, 97, 3));
    r.roughness = 1.5f * powf(8.9f / s100, .75f) * (s100 / 8.9f);
    r.bowing    = 1.0f * powf(8.9f / s100, .85f);

    /* LOPSIDED. Nobody draws an X whose two strokes cross in the middle with
     * equal arms; the hand starts the second one a bit off and overshoots.
     * rough.js cannot give you that - it wobbles a line, it does not move the
     * line - so the skew is in the endpoints, and it is the single thing that
     * makes a small mark read as drawn rather than stamped. It was dropped in
     * the port and the board's X's came out ruled. */
    const float L = .3f;
    int k = 0;
    *k2 = 0;
    if (kind == UTTT_X) {
        k   = uttt_rough_line(&r, 10 - L*4, 11, 94 + L*3, 92 - L*6, pts, cap, &np, sp, 2);
        *k2 = uttt_rough_line(&r, 92 + L*4, 12, 13 - L*5, 90 + L*5, pts, cap, &np, sp + k, 2);
    } else {
        k   = uttt_rough_ellipse(&r, O_CX, O_CY, O_W, O_H, pts, cap, &np, sp, 2);
    }
    return k;
}

static void mark_in(UtttDL *d, int kind, float x, float y, float s,
                    int32_t seed, float t, const UtttPen *base)
{
    UtttPt pts[1400];
    UtttSpan sp[4];
    UtttPen p = *base;
    p.ink = kind == UTTT_O ? INK_O : INK_X;
    p.w   = base->w * s / 100.f;           /* stroke lives in board units */

    int k2, k = mark_geom(kind, s, seed, pts, 1400, sp, &k2);
    int np = sp[k + k2 - 1].first + sp[k + k2 - 1].n;
    for (int i = 0; i < np; i++) {
        pts[i].x = x + pts[i].x * s / 100.f;
        pts[i].y = y + pts[i].y * s / 100.f;
    }
    if (kind == UTTT_X) {
        /* two strokes, the second starting before the first has finished */
        float a = t / .52f;      if (a > 1.f) a = 1.f;
        float b = (t - .44f) / .56f;
        for (int i = 0; i < k; i++)
            stroke(d, pts + sp[i].first, sp[i].n, &p, a);
        for (int i = k; i < k + k2; i++)
            stroke(d, pts + sp[i].first, sp[i].n, &p, b);
    } else {
        for (int i = 0; i < k; i++)
            stroke(d, pts + sp[i].first, sp[i].n, &p, t / .98f);
    }
}

/* One # - four lines, no outer border, every line overshooting. Nobody draws
 * a lattice; a tic-tac-toe board is a hash. */
static void hash_in(UtttDL *d, float x, float y, float sz, int32_t seed,
                    float w, float over, float alpha, float rmul)
{
    UtttPt pts[2048]; int np = 0;
    UtttSpan sp[2];
    UtttPen p = uttt_pen_92();
    p.ink = INK_GR; p.w = w; p.a = alpha; p.vel = 0; p.lift = .3f;
    p.grain = .35f; p.agrain = .25f;

    float k = sz / 3.f;
    for (int i = 1; i <= 2; i++) {
        float v = x + k * i, hh = y + k * i;
        UtttRough r = uttt_rough_default(seed + i * 31);
        r.roughness = rough_for(sz) * rmul;
        r.bowing    = bow_for(sz) * rmul;
        r.max_offset = mro_for(sz) * rmul;
        r.seg_line = 18;
        np = 0;
        int n = uttt_rough_line(&r, v, y - over, v, y + sz + over,
                                pts, 2048, &np, sp, 2);
        for (int q = 0; q < n; q++) stroke(d, pts + sp[q].first, sp[q].n, &p, 1.f);
        np = 0;
        n = uttt_rough_line(&r, x - over, hh, x + sz + over, hh,
                            pts, 2048, &np, sp, 2);
        for (int q = 0; q < n; q++) stroke(d, pts + sp[q].first, sp[q].n, &p, 1.f);
    }
}

/* THE LAST MARK IS HEAVIER: gone over twice rather than boxed, so it says
 * "this one" without adding a shape the game does not otherwise have. The
 * headline's mark ("Waiting on O") is drawn with the same two passes. */
static void heavy_mark(UtttDL *d, int v, float x, float y, float s,
                       int32_t seed1, int32_t seed2, float t, float k)
{
    UtttPen p = uttt_pen_92();
    const float w = p.w * k;
    p.w = w * 2.2f; p.a = 1.f; p.grain = .22f;
    mark_in(d, v, x, y, s, seed1, t, &p);
    p.w = w * 1.9f; p.a = .85f;
    mark_in(d, v, x, y, s, seed2, t, &p);
}

static void last_mark(UtttDL *d, int v, int mv, int32_t seed, float t)
{
    int b = mv / 9, c = mv % 9;
    float x = (b % 3) * BL + (c % 3) * CE + CE * .1f;
    float y = (b / 3) * BL + (c / 3) * CE + CE * .1f;
    heavy_mark(d, v, x, y, CE * .8f, sd(seed, 1000, mv), sd(seed, 1000, mv + 613), t, 1.f);
}

/* THE BIG MARK OVER A WON BLOCK, drawn to `t`: pen is purely additive, so
 * the nine marks under it stay and only lose the glance (UI.html 04). */
static void big_mark(UtttDL *d, const UtttGame *g, int b, int32_t seed, float t)
{
    UtttPen p = uttt_pen_92(); p.a = .62f; p.w = 2.2f;
    mark_in(d, uttt_block(g, b), (b % 3) * BL + BL * .08f,
            (b / 3) * BL + BL * .08f, BL * .84f, sd(seed, 77, b), t, &p);
}

/* The four major grid lines' first (heavier) pass, in hundredths of a ninth
 * of the pen: the unit the win line's weight is stated in. */
#define GRID_MAJOR_W 1.7f

#ifndef WIN_ROUGH
#define WIN_ROUGH 1.5f
#endif
#ifndef WIN_MO
#define WIN_MO 1.5f
#endif
#ifndef WIN_BOW
#define WIN_BOW 2.f
#endif

static void win_stroke(UtttDL *d, float ax, float ay, float zx, float zy,
                       uint32_t ink, int32_t seed, float t, float wk);

static void win_line(UtttDL *d, const UtttGame *g, int32_t seed, float t)
{
    /* the win line: the only mark that crosses a thick line, in the winner's
     * own ink, and drawn twice because a ball has one width */
    if (g->over == UTTT_X || g->over == UTTT_O) {
        static const uint8_t L[8][3] = {
            {0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6} };
        for (int i = 0; i < 8; i++) {
            if (uttt_block(g, L[i][0]) != g->over
             || uttt_block(g, L[i][1]) != g->over
             || uttt_block(g, L[i][2]) != g->over) continue;
            int a = L[i][0], z = L[i][2];
            float ax = ((a % 3) + .5f) * BL, ay = ((a / 3) + .5f) * BL;
            float zx = ((z % 3) + .5f) * BL, zy = ((z / 3) + .5f) * BL;
            /* IT RUNS PAST BOTH BLOCKS IT ENDS ON. A line drawn centre to
             * centre stops inside the two end blocks and reads as a
             * measurement; the one somebody actually draws goes through them
             * and out the far side. A sixth of the run at each end - a third
             * longer overall - which is also what the four main grid lines
             * do, and for the same reason. */
            {
                float ex = (zx - ax) / 6.f, ey = (zy - ay) / 6.f;
                ax -= ex; ay -= ey; zx += ex; zy += ey;
            }
            /* TWICE THE MAJOR LINE AND MORE (owner, 2026-09-23: "winning
             * diagonal needs to be thicker"). It was 2.7 and 2.3, half again
             * the major lines' 1.7 (UI.html 08), and read as one more line
             * among the ones it crosses. Now 3x and 2.5x the major pen (measured
             * on the ribbons: twice the major line's ink width), so
             * the stroke that ends the game is the heaviest ink on the sheet
             * by a clear margin - a width only, the same points. */
            win_stroke(d, ax, ay, zx, zy, g->over == UTTT_O ? INK_O : INK_X, seed, t, 1.f);
            break;
        }
    }
}

/* The win line's two passes from (ax, ay) to (zx, zy), in board units, in
 * `ink`, its widths times `wk` (1 on the board). */
static void win_stroke(UtttDL *d, float ax, float ay, float zx, float zy,
                       uint32_t ink, int32_t seed, float t, float wk)
{
    {
        {
            const float W[2] = { GRID_MAJOR_W * 3.0f, GRID_MAJOR_W * 2.5f };
            const float A[2] = { .92f, .74f };
            const int32_t SD[2] = { 313, 977 };
            for (int q = 0; q < 2; q++) {
                /* DRAWN IN A HUNDRED-UNIT BOARD, like a mark in its own
                 * hundred-unit square, and the points scaled back once.
                 * rough.js's bow is bowing x maxRandomnessOffset x length /
                 * 200 - an absolute offset times a length - so with both
                 * converted to the unit board it came out squared-small: at
                 * rough_for(len) x 2.2 the bow was a millionth of the board
                 * and the line was ruled (owner, TestFlight 1.0(6): "the
                 * winning line still appears quite straight"). Here rough.js
                 * gets its own units and its own offset of 2, and the line
                 * wanders and bows by a percent or two of the board, as one
                 * drawn in one stroke across a sheet does. */
                UtttRough r = uttt_rough_default(sd(seed, (uint32_t)SD[q], 0));
                r.roughness = WIN_ROUGH;
                r.bowing    = WIN_BOW;
                r.max_offset = WIN_MO;
                r.seg_line = 24;
                UtttPt pts[1024]; int np = 0; UtttSpan sp[2];
                int n = uttt_rough_line(&r, ax * 100.f, ay * 100.f, zx * 100.f, zy * 100.f,
                                        pts, 1024, &np, sp, 2);
                for (int i = 0; i < np; i++) { pts[i].x /= 100.f; pts[i].y /= 100.f; }
                UtttPen p = uttt_pen_92();
                p.ink = ink;
                p.w = uttt_pen_92().w / 9.f / 100.f * W[q] * wk; p.a = A[q];
                p.vel = 0; p.lift = .2f; p.grain = .25f; p.agrain = .2f;
                for (int s2 = 0; s2 < n; s2++)
                    stroke(d, pts + sp[s2].first, sp[s2].n, &p, t);
            }
        }
    }
}

int uttt_draw_board(UtttDL *d, const UtttGame *g, const UtttDrawOpts *o)
{
    UtttPen base = uttt_pen_92();

    /* the block you are sent to, in highlighter - ink cannot say "here"
     * without also saying something it can never take back */
    {
        float r[4], a;
        if (uttt_wash_rect(o->active, r, &a))
            rect(d, r[0], r[1], r[2], r[3], uttt_wash_rgba(a));
    }

    for (int b = 0; b < 9; b++)
        hash_in(d, (b % 3) * BL, (b / 3) * BL, BL,
                sd(o->seed, 131, b * 17),
                base.w / 9.f / 100.f * .62f, BL * .03f, .5f, 1.5f);
    /* THE FOUR MAIN LINES RUN LONG. Nobody ruling a board stops the pen
     * neatly at the last cell - the line goes where the arm goes, past the
     * corner and sometimes off the paper. A short overshoot reads as a
     * cautious drawing; a long one reads as somebody who drew it in four
     * strokes without looking. The renderer's own frame clips whatever runs
     * past the edge, which is the right answer: a line that leaves the board
     * should leave the board. */
    hash_in(d, 0, 0, S, sd(o->seed, 7, 3),
            base.w / 9.f / 100.f * GRID_MAJOR_W, S * .135f * o->reach, .9f, 3.4f);
    hash_in(d, 0, 0, S, sd(o->seed, 19, 5),
            base.w / 9.f / 100.f * 1.5f, S * .118f * o->reach, .72f, 3.4f);

    for (int b = 0; b < 9; b++) {
        int won = uttt_block(g, b) == UTTT_X || uttt_block(g, b) == UTTT_O;
        for (int c = 0; c < 9; c++) {
            int v = uttt_cell(g, b * 9 + c);
            if (!v) continue;
            float x = (b % 3) * BL + (c % 3) * CE;
            float y = (b / 3) * BL + (c / 3) * CE;
            int is_last = (b * 9 + c) == o->last;
            if (is_last) { last_mark(d, v, b * 9 + c, o->seed, o->mark_t); continue; }
            UtttPen p = base;
            if (won) p.a = base.a * .34f;
            mark_in(d, v, x + CE * .1f, y + CE * .1f, CE * .8f,
                    sd(o->seed, 1000, b * 9 + c), 1.f, &p);
        }
        if (won) big_mark(d, g, b, o->seed, (o->last >= 0 && o->last / 9 == b) ? o->fall_t : 1.f);
    }

    win_line(d, g, o->seed, o->meta_t);
    return (d->n_poly < d->cap_poly && d->n_pt < d->cap_pt) ? 0 : -1;
}

int uttt_draw_last(UtttDL *d, const UtttGame *g, int32_t seed, float t)
{
    if (g->n_plies == 0) return -1;
    int mv = g->move[g->n_plies - 1];
    last_mark(d, uttt_cell(g, mv), mv, seed ? seed : 1, t);
    return (d->n_poly < d->cap_poly && d->n_pt < d->cap_pt) ? 0 : -1;
}

int uttt_draw_settle(UtttDL *d, const UtttGame *g, int32_t seed, float fall_t, float line_t)
{
    if (g->n_plies == 0) return -1;
    if (!seed) seed = 1;
    int b = g->move[g->n_plies - 1] / 9;
    if (uttt_block(g, b) == UTTT_X || uttt_block(g, b) == UTTT_O) {
        big_mark(d, g, b, seed, fall_t);
        win_line(d, g, seed, line_t);
    }
    return (d->n_poly < d->cap_poly && d->n_pt < d->cap_pt) ? 0 : -1;
}

/* THE PROMISE (uttt_anim.h): the highlighter's own rect, in its own colour
 * at full strength, drawn round by the pen - top, right, bottom, left, one
 * after the other, as a hand goes round a box - to `t`. The rect and the
 * colour are uttt_wash_rect and uttt_wash_rgba, the tint's, so the outline
 * and the tint that replaces it at Send cannot disagree by a point. The seed
 * is the sheet's and the block's, so both phones draw the same wobble.
 *
 * A HAND-DRAWN ROUGH.JS RECTANGLE, CALMED (owner, 2026-09-23: "the gold
 * outline is not rough enough"; then, TestFlight 1.0(7): "still hand drawn
 * but not so crazy"). Each side is its own seeded rough.js line on the
 * marks' pen, at HALF what a mark's stroke gets - roughness, bowing and the
 * end offset - so a block-sized box still wobbles by hand, but by half as
 * much. Every side runs a small fixed overshoot past both its corners, so
 * the four sides cross there as a quick pen box's do, rather than meeting
 * exactly or stopping short. */
#define OUTLINE_CALM      .5f                 /* of a mark's roughness */
#define OUTLINE_OVERSHOOT (BL * .025f)        /* past each corner */

/* The promise's pen box round r (x, y, w, h), its stroke `w` wide, sides
 * seeded from `seed` and `salt`, drawn round to `t`. */
static void promise_box(UtttDL *d, const float r[4], float o, float w,
                        int32_t seed, int salt, float t)
{
    const float x0 = r[0], y0 = r[1], x1 = r[0] + r[2], y1 = r[1] + r[3];
    const float side[4][4] = {
        { x0 - o, y0, x1 + o, y0 }, { x1, y0 - o, x1, y1 + o },
        { x1 + o, y1, x0 - o, y1 }, { x0, y1 + o, x0, y0 - o } };
    const float len[4] = { r[2] + 2 * o, r[3] + 2 * o, r[2] + 2 * o, r[3] + 2 * o };
    const float per = len[0] + len[1] + len[2] + len[3];
    UtttPen p = uttt_pen_92();
    p.ink = uttt_wash_rgba(1.f);
    p.w = w;
    p.a = 1.f;
    float done = 0.f, want = (t > 1.f ? 1.f : t) * per;
    for (int k = 0; k < 4 && done < want; k++) {
        UtttRough rg = uttt_rough_default(sd(seed, 577, salt + k * 7));
        rg.roughness  = rough_for(REF) * OUTLINE_CALM;
        rg.bowing     = bow_for(REF) * OUTLINE_CALM;
        rg.max_offset = mro_for(REF) * OUTLINE_CALM;
        rg.seg_line = 18;
        UtttPt pts[1024]; int np = 0; UtttSpan sp[2];
        int n = uttt_rough_line(&rg, side[k][0], side[k][1], side[k][2], side[k][3],
                                pts, 1024, &np, sp, 2);
        float part = (want - done) / len[k];
        for (int q = 0; q < n; q++) stroke(d, pts + sp[q].first, sp[q].n, &p, part);
        done += len[k];
    }
}

int uttt_draw_outline(UtttDL *d, int block, int32_t seed, float t)
{
    float r[4];
    if (t <= 0.f || !uttt_wash_rect(block, r, NULL)) return 0;
    if (!seed) seed = 1;
    promise_box(d, r, OUTLINE_OVERSHOOT, uttt_pen_92().w / 9.f / 100.f * GRID_MAJOR_W * 1.3f,
                seed, block * 31, t);
    return (d->n_poly < d->cap_poly && d->n_pt < d->cap_pt) ? 0 : -1;
}

int uttt_draw_cell(UtttDL *d, int mark, int mv, int32_t seed, float t)
{
    if (mv < 0 || mv > 80) return -1;
    int b = mv / 9, c = mv % 9;
    float x = (b % 3) * BL + (c % 3) * CE;
    float y = (b / 3) * BL + (c / 3) * CE;
    UtttPen p = uttt_pen_92();
    mark_in(d, mark, x + CE * .1f, y + CE * .1f, CE * .8f,
            sd(seed, 1000, b * 9 + c), t, &p);
    return 0;
}

/* The inverse of the placement above, from the same BL and CE: a point in the
 * board's unit square to the move under it. The far edge (exactly 1) belongs
 * to the last cell rather than to nothing; anything off the board is -1. */
int uttt_hit(float u, float v)
{
    if (!(u >= 0.f && u <= S && v >= 0.f && v <= S)) return -1;
    int bx = (int)(u / BL), by = (int)(v / BL);
    if (bx > 2) bx = 2;
    if (by > 2) by = 2;
    int cx = (int)((u - bx * BL) / CE), cy = (int)((v - by * BL) / CE);
    if (cx > 2) cx = 2;
    if (cy > 2) cy = 2;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    return (by * 3 + bx) * 9 + (cy * 3 + cx);
}

int uttt_cell_rect(int mv, float r[4])
{
    if (mv < 0 || mv > 80) return 0;
    int b = mv / 9, c = mv % 9;
    r[0] = (b % 3) * BL + (c % 3) * CE;
    r[1] = (b / 3) * BL + (c / 3) * CE;
    r[2] = r[3] = CE;
    return 1;
}

/* THE O STAYS A RING. rough.js closes an ellipse by running past its start
 * by a seeded overlap and curving in to 98% and then 90% of the radius, and
 * at the "you are" mark's size its roughness is 2.66 - so for some seeds the
 * tail sweeps up to two thirds of a turn further on a handful of points, and
 * the curve through them cuts a CHORD across the inside of the circle (owner,
 * TestFlight 1.0(6): "the drawn O sometimes draws lines right through the
 * circle"). This asks whether every sample of both passes lies in the ring
 * [1 - UTTT_O_RING, 1 + UTTT_O_RING] of the O's own size - its median
 * distance from the centre, in the ellipse's own proportions, so an O that
 * came out small or large is judged against itself. */
int uttt_o_in_ring(int32_t seed, float s)
{
    UtttPt pts[1400];
    UtttSpan sp[4];
    int k2, k = mark_geom(UTTT_O, s, seed, pts, 1400, sp, &k2);
    int np = sp[k - 1].first + sp[k - 1].n;
    if (np < 8) return 0;
    float rho[1400], sorted[1400];
    for (int i = 0; i < np; i++) {
        float dx = (pts[i].x - O_CX) / (O_W * .5f), dy = (pts[i].y - O_CY) / (O_H * .5f);
        rho[i] = sorted[i] = sqrtf(dx * dx + dy * dy);
    }
    for (int i = 1; i < np; i++)                        /* median: insertion */
        for (int j = i; j > 0 && sorted[j - 1] > sorted[j]; j--) {
            float t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t;
        }
    float med = sorted[np / 2];
    for (int i = 0; i < np; i++)
        if (fabsf(rho[i] / med - 1.f) > UTTT_O_RING) return 0;
    return 1;
}

/* THE SEED THE "YOU ARE" MARK IS DRAWN WITH: the game's own for an X, and
 * for an O the first of a fixed walk from it whose O stays a ring - a pure
 * function of the seed, so both phones pick the same one. Around half of all
 * seeds pass, so the walk is a try or two; 64 without one (never seen in a
 * million) keeps the game's own. */
int32_t uttt_mark_seed(int mark, int32_t seed)
{
    if (mark != UTTT_O) return seed;
    for (int i = 0; i < 64; i++) {
        int32_t s = (int32_t)((uint32_t)seed + (uint32_t)i * 7919u);
        if (s && uttt_o_in_ring(s, UTTT_MARK_SIDE)) return s;
    }
    return seed;
}

int uttt_draw_mark(UtttDL *d, int mark, int32_t seed, float board)
{
    int32_t s1 = uttt_mark_seed(mark, seed);
    if (board > 0.f) {
        /* a stroke's width is a share of its mark's side, so the board's
         * CE .8 mark at `board` times this one's frame lays down the same
         * points of ink when its pen is scaled by this */
        float k = CE * .8f * board / UTTT_MARK_SIDE;
        heavy_mark(d, mark, .06f, .06f, UTTT_MARK_SIDE, s1,
                   uttt_mark_seed(mark, (int32_t)((uint32_t)seed + 613u)), 1.f, k);
        return 0;
    }
    UtttPen p = uttt_pen_92();
    p.w = uttt_pen_92().w * 1.15f;
    mark_in(d, mark, .06f, .06f, UTTT_MARK_SIDE, s1, 1.f, &p);
    return 0;
}

/* ------------------------------------------------------------ the bubble */
/* docs/UI.html "Bubble 300x195", option 02: the board takes the height, two
 * words and a place take what is left, 10 points of padding and a 12-point
 * gutter between them.
 *
 * THE BOARD IS ON THE RIGHT, mirrored against option 02, and that is not a
 * taste. Messages stamps the app's own logo into the TOP-LEFT corner of every
 * bubble it draws, over whatever is underneath: measured in the simulator
 * transcript it is a pill about 31 by 24 points, 6 in from the corner. A
 * board in that corner has the badge sitting on its top-left cell - one of
 * the 81 squares - for the whole game. The text column carries it instead:
 * the two lines are centred well below it.
 *
 * AND THE LINES STOP ON THE PAPER. On the drawer the four main lines run 13.5%
 * past the board and the sheet clips them, which reads as the pen leaving the
 * paper. In a 195-point frame that overshoot is 24 points and every line ran
 * into the bubble's rounded edge, which read as the board being cut off
 * (the WP1 audit's "clipped at the right edge"). Here they run 5% - the
 * design document's own figure - and the board is sized so the longest line
 * is asked to end 5 points inside the frame on the three sides it faces, and
 * with its jitter still ends at least 3 inside. */
#define BUB_W     300.f
#define BUB_H     195.f
#define BUB_PAD    10.f
#define BUB_GUT    12.f
#define BUB_REACH  UTTT_REACH       /* the design's 5% over the pen's 13.5% */
#define BUB_EDGE    5.f             /* where the longest line is asked to stop; its jitter takes it ~1.5pt further since its far end draws */

UtttBubble uttt_bubble(const UtttGame *g)
{
    UtttBubble b = { 0 };
    b.w = BUB_W; b.h = BUB_H;
    b.words = g && g->over;

    /* side + 2 * .05 * side + 2 * edge = height */
    float side = (BUB_H - 2.f * BUB_EDGE) / (1.f + 2.f * .135f * BUB_REACH);
    side = (float)(int)side;                     /* whole points: 168 */
    float m = (BUB_H - side) * .5f;
    /* THE BOARD ALONE IS CENTRED: the frame is wider than tall, so the side
     * is the height's and the badge's corner is 66 points left of it. */
    b.board.x = b.words ? BUB_W - m - side : (BUB_W - side) * .5f;
    b.board.y = m;
    b.board.w = side;
    b.board.h = side;

    if (b.words) {
        b.text.x = BUB_PAD;
        b.text.y = b.board.y;
        b.text.w = b.board.x - BUB_GUT - BUB_PAD;
        b.text.h = side;
    }

    /* 18, measured off option 02: "Your move" there is 82 points wide in
     * bold, which is 18-point type. At 16 the two lines read as a label
     * beside the board rather than the thing a glance lands on. */
    b.headline_pt = 18.f;
    b.place_pt    = 18.f;
    b.lead        = 1.f;
    b.reach       = BUB_REACH;
    b.headline_rgba = UTTT_INK;
    /* the winner's own ink, as the win line is drawn in it (owner,
     * 2026-09-26: O's "50 moves" was X blue); a draw keeps X blue */
    b.place_rgba    = uttt_mark_ink(g && g->over == UTTT_O ? UTTT_O : UTTT_X);
    return b;
}

float uttt_bubble_scale(float display)
{
    if (!(display >= 2.f)) return 2.f;           /* NaN lands here too */
    return display > 3.f ? 3.f : display;
}

/* THE NINE BLOCKS BY NAME, and "anywhere": the table's (uttt/c/i18n),
 * in the kernel's language. */
const char *uttt_place_name(int block)
{
    if (block < 0 || block > 9) return "";
    return uttt_text(UT_K_PLACE_TOP_LEFT + block);
}

/* ---------------------------------------------------------------- the icon */
/* THE APP'S OWN FACE, drawn with the app's own pen.
 *
 * It is a static asset - a build-time tool rasterises this into the PNGs the
 * asset catalogues carry - but it is still the pen, and the pen has one
 * implementation. An icon traced by hand in a drawing program is a second pen
 * that drifts from the first the next time the first one changes.
 *
 * NO BOARD IN IT. The real one is 9x9 and the smallest icon iOS asks for is
 * 27x20 POINTS, where nine hashes are a grey smudge. */
int uttt_draw_icon(UtttDL *d, float w, float h)
{
    if (w <= 0 || h <= 0) return -1;
    const int first = d->n_pt;

    /* THE LOGO IS A MONOGRAM, not a board. An X and an O the same size, on
     * top of each other, drawn in three movements:
     *
     *     1. the X's north-west to south-east stroke
     *     2. the O
     *     3. the X's north-east to south-west stroke
     *
     * so the circle passes OVER the first stroke and UNDER the second, and
     * the two marks are threaded through one another rather than stacked.
     * That is the whole idea and it only exists because this pen is a ribbon
     * of strokes laid down in order - a renderer that stacked two finished
     * images could not do it, and neither could an icon drawn by hand once.
     *
     * Drawn in a UNIT SQUARE and mapped into the frame afterwards: Apple's
     * messages icons are landscape (27x20, 60x45) and a display list is 0..1
     * of the FRAME, so a square asked for by one number comes out stretched.
     * Transform the POINTS once, at the end.
     */
    UtttPt pts[3000]; int np = 0;
    UtttSpan sp[6];

    UtttPen p = uttt_pen_92();
    p.w = p.w * 1.55f / 100.f;       /* a logo is one mark filling the frame,
                                      * where a board's mark is a ninth of it */
    /* OPAQUE, which no other mark is. At .8 the two inks blend where they
     * cross and the threading reads as a muddy overlap instead of as one
     * stroke passing under another. The whole point of the logo is that
     * crossing, so it is the one place the paper does not show through. */
    p.a = 1.f; p.agrain = .12f; p.grain = .3f;

    UtttRough rx = uttt_rough_default(31 * 97 + 3);
    rx.roughness = 1.5f * powf(8.9f / 100.f, .75f) * (100.f / 8.9f);
    rx.bowing    = 1.0f * powf(8.9f / 100.f, .85f);
    UtttRough ro = rx;
    ro.seed = 52 * 97 + 3;

    const float L = .3f;             /* the same lopsidedness every mark has */
    int n1 = uttt_rough_line(&rx, 10 - L*4, 11, 94 + L*3, 92 - L*6,
                             pts, 3000, &np, sp, 2);
    int no = uttt_rough_ellipse(&ro, 50 + L*4, 50 - L*3, 78 - L*10, 76 + L*8,
                                pts, 3000, &np, sp + n1, 2);
    int n2 = uttt_rough_line(&rx, 92 + L*4, 12, 13 - L*5, 90 + L*5,
                             pts, 3000, &np, sp + n1 + no, 2);
    for (int i = 0; i < np; i++) { pts[i].x /= 100.f; pts[i].y /= 100.f; }

    int k = 0;
    p.ink = INK_X;
    for (int i = 0; i < n1; i++, k++) stroke(d, pts + sp[k].first, sp[k].n, &p, 1.f);
    p.ink = INK_O;
    for (int i = 0; i < no; i++, k++) stroke(d, pts + sp[k].first, sp[k].n, &p, 1.f);
    p.ink = INK_X;
    for (int i = 0; i < n2; i++, k++) stroke(d, pts + sp[k].first, sp[k].n, &p, 1.f);

    /* 70% of the short side, centred - a monogram needs air around it in a
     * way a board does not, and the strokes overshoot their own box. */
    const float sq = (w < h ? w : h) * .70f;
    const float dx = (w - sq) / 2.f, dy = (h - sq) / 2.f;
    for (int i = first; i < d->n_pt; i++) {
        d->pt[i].x = (dx + d->pt[i].x * sq) / w;
        d->pt[i].y = (dy + d->pt[i].y * sq) / h;
    }
    return (d->n_poly < d->cap_poly && d->n_pt < d->cap_pt) ? 0 : -1;
}

/* ---------------------------------------------------------------- the sheet */
/* THE NAPKIN, and it belongs here for the same reason the marks do: both
 * phones have to be looking at the same piece of paper.
 *
 * It had drifted into three copies - the Swift bridge, the PPM harness, and
 * the design document's canvas - which is the shape of every bug this pen has
 * had. Crossed cellulose over a warm near-white, one shade darker down the
 * page than up it.
 *
 * RGBA, 4 bytes a pixel, fully opaque: the sheet is the bottom layer and
 * nothing is ever behind it. */
void uttt_paper(uint8_t *rgba, int w, int h)
{
    if (!rgba || w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float t = (float)y / (float)h;
            float base = .976f - t * .028f;
            float fib = uttt_grain((float)x, (float)y, .82f, .012f, 3) * .5f
                      + uttt_grain((float)x, (float)y, .012f, .82f, 8) * .5f;
            float v = base + (fib - .5f) * .035f;
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            p[0] = (uint8_t)(v * 255.f);
            p[1] = (uint8_t)(v * .998f * 255.f);
            p[2] = (uint8_t)(v * .982f * 255.f);
            p[3] = 255;
        }
}


/* ---------------------------------------------------------------- the rules */
/* THE EIGHT DRAWINGS OF THE RULES SHEET (docs/RULES.html, owner-approved),
 * out of this pen: the board's hashes, marks, big marks, win line, wash and
 * promise, each drawn by the function the board draws it with.
 *
 * THE MOCKUP'S OWN UNITS. RULES.html draws a 90-unit board in a 98-unit
 * square (viewBox -4 -4 98 98): a block is 30 units, its hash inset 3, a
 * mark's arm `r` from its square's middle, every width in those units. The
 * numbers below are its numbers, and a frame puts them in the unit square
 * the display list is in: v -> o + v k. */
typedef struct { float o, k; } RFrame;
static const RFrame R_BOARD = { 4.f / 98.f, 1.f / 98.f };
static const RFrame R_WHOLE = { 0.f, 1.f / 90.f };      /* rule 8's lone X */

#define R_BLOCK 30.f
/* A big mark's strength: .62 on the paper, as the mockup's group opacity,
 * through the pen's .8 and the ink gain (UTTT_INK_GAIN). */
#define R_BIG_A (.62f / (.8f * UTTT_INK_GAIN))
#define R_CELL  (24.f / 3.f)             /* a square: the hash is 24 of 30  */

static float rf(RFrame f, float v) { return f.o + v * f.k; }

/* The middle of square `c` of block `b`, in the mockup's units. */
static void r_cell(int b, int c, float *x, float *y)
{
    *x = (b % 3) * R_BLOCK + 3.f + R_CELL * ((c % 3) + .5f);
    *y = (b / 3) * R_BLOCK + 3.f + R_CELL * ((c / 3) + .5f);
}

/* THE BOARD: nine small hashes inset in their blocks, and the four main
 * lines two units past it in the board's own two passes. */
static void r_grid(UtttDL *d, int32_t seed)
{
    const RFrame f = R_BOARD;
    for (int b = 0; b < 9; b++)
        hash_in(d, rf(f, (b % 3) * R_BLOCK + 3.f), rf(f, (b / 3) * R_BLOCK + 3.f), 24.f * f.k,
                sd(seed, 131, b * 17), .75f * f.k, .4f * f.k, .62f, 1.5f);
    hash_in(d, rf(f, 0), rf(f, 0), 90.f * f.k, sd(seed, 7, 3), 1.35f * f.k, 2.f * f.k, .9f, 3.4f);
    hash_in(d, rf(f, 0), rf(f, 0), 90.f * f.k, sd(seed, 19, 5), 1.1f * f.k, 1.6f * f.k, .72f, 3.4f);
}

/* A MARK where the mockup puts it - an X's arms `r` from (cx, cy), an O
 * 2.1 r across - drawn by mark_in, its stroke `w` wide, at `alpha` of the
 * board's ink. mark_in draws an X from 10 to 94 of its side and an O O_W
 * across about (O_CX, O_CY), so this finds the side that spans it. */
static void r_mark(UtttDL *d, RFrame f, int kind, float cx, float cy, float r,
                   float w, float alpha, int32_t seed)
{
    float s, x, y;
    if (kind == UTTT_X) { s = 2.f * r / .84f; x = cx - s * .52f; y = cy - s * .515f; }
    else {
        s = 2.1f * r / (O_W / 100.f);
        x = cx - s * O_CX / 100.f; y = cy - s * O_CY / 100.f;
    }
    UtttPen p = uttt_pen_92();
    p.a *= alpha;
    p.w = w * 100.f / s;                 /* mark_in: stroke = p.w * side / 100 */
    mark_in(d, kind, rf(f, x), rf(f, y), s * f.k, seed, 1.f, &p);
}

/* A BLOCK WON: the winner's big mark over it at the board's big-mark
 * strength (.62), the mockup's size - an X's arms 12.6 from the block's
 * middle, an O 25.2 across - `w` wide. */
static void r_won(UtttDL *d, RFrame f, int b, int kind, float w, int32_t seed)
{
    r_mark(d, f, kind, (b % 3) * R_BLOCK + 15.f, (b / 3) * R_BLOCK + 15.f,
           kind == UTTT_O ? 12.f : 12.6f, w, R_BIG_A, sd(seed, 77, b));
}

/* The wash over block `b` (9: the sheet), the board's own rect and alpha. */
static void r_wash(UtttDL *d, int b)
{
    float r[4], a;
    if (!uttt_wash_rect(b, r, &a)) return;
    rect(d, rf(R_BOARD, r[0] * 90.f), rf(R_BOARD, r[1] * 90.f), r[2] * 90.f * R_BOARD.k,
         r[3] * 90.f * R_BOARD.k, uttt_wash_rgba(a));
}

/* A smooth pen line through `n` points in the mockup's units: the arrow. */
static void r_line(UtttDL *d, const UtttPt *pts, int n, float w, uint32_t ink)
{
    UtttPt q[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) q[i] = (UtttPt){ rf(R_BOARD, pts[i].x), rf(R_BOARD, pts[i].y) };
    UtttPen p = uttt_pen_92();
    p.ink = ink; p.w = w * R_BOARD.k; p.a = 1.f; p.vel = 0; p.lift = .2f;
    p.grain = .2f; p.agrain = .15f;
    stroke(d, q, n, &p, 1.f);
}

int uttt_draw_rule(UtttDL *d, int i)
{
    if (i < 0 || i > 7) return -1;
    const RFrame B = R_BOARD;
    const int32_t seed = 3 + i * 10;     /* the mockup's: 3, 11 .. 71 */
    float cx, cy;
    switch (i) {
    case 0:                              /* the board */
        r_grid(d, seed);
        break;
    case 1: {                            /* three won on a diagonal, and the line */
        r_grid(d, seed);
        r_cell(1, 4, &cx, &cy); r_mark(d, B, UTTT_O, cx, cy, 2.4f, 1.1f, 1.f, sd(seed, 1000, 13));
        r_cell(3, 0, &cx, &cy); r_mark(d, B, UTTT_X, cx, cy, 2.4f, 1.1f, 1.f, sd(seed, 1000, 27));
        r_cell(8, 2, &cx, &cy); r_mark(d, B, UTTT_O, cx, cy, 2.4f, 1.1f, 1.f, sd(seed, 1000, 74));
        r_won(d, B, 0, UTTT_O, 2.f, seed);
        r_won(d, B, 2, UTTT_X, 2.f, seed);
        r_won(d, B, 4, UTTT_X, 2.f, seed);
        r_won(d, B, 6, UTTT_X, 2.f, seed);
        /* the board's win line through blocks 2, 4 and 6, a sixth of the
         * run past both ends as win_line runs it, in the mockup's width */
        const float ax = 5.f / 6.f + 1.f / 9.f, ay = 1.f / 6.f - 1.f / 9.f;
        const float zx = 1.f / 6.f - 1.f / 9.f, zy = 5.f / 6.f + 1.f / 9.f;
        win_stroke(d, rf(B, ax * 90.f), rf(B, ay * 90.f), rf(B, zx * 90.f), rf(B, zy * 90.f),
                   INK_X, seed, 1.f, 2.8f * B.k / (uttt_pen_92().w / 9.f / 100.f * GRID_MAJOR_W * 3.f));
        break;
    }
    case 2: {                            /* one subgrid won: its marks fade, a big O over it */
        const RFrame W = R_BOARD;
        /* one hash across the whole drawing, its lines stopping 4 short */
        hash_in(d, rf(W, 0), rf(W, 0), 90.f * W.k, sd(seed, 7, 3), 1.4f * W.k, -4.f * W.k, .9f, 3.4f);
        static const int8_t C[5][2] = { { 0, UTTT_O }, { 4, UTTT_O }, { 8, UTTT_O },
                                        { 1, UTTT_X }, { 5, UTTT_X } };
        for (int k = 0; k < 5; k++)
            r_mark(d, W, C[k][1], (C[k][0] % 3) * 30.f + 15.f, (C[k][0] / 3) * 30.f + 15.f,
                   8.f, 2.2f, .34f, sd(seed, 1000, C[k][0]));
        /* the big O over it, 75.6 across, at the big mark's strength */
        r_mark(d, W, UTTT_O, 45.f, 45.f, 36.f, 2.4f, R_BIG_A, sd(seed, 77, 0));
        break;
    }
    case 3: {                            /* the square you mark sends them */
        r_grid(d, seed);
        r_cell(4, 2, &cx, &cy);
        r_mark(d, B, UTTT_X, cx, cy, 2.4f, 1.1f, 1.f, sd(seed, 1000, 38));
        /* a red arrow from that square to the block it names: the
         * mockup's quadratic, sampled, and its two-stroke head */
        const float x2 = 2 * R_BLOCK + 15.f, y2 = 15.f;
        const float x1 = cx + 3.f, y1 = cy - 3.f, ex = x2 - 3.f, ey = y2 + 5.f;
        const float mx = (cx + x2) / 2.f + 10.f, my = (cy + y2) / 2.f + 4.f;
        UtttPt a[24];
        for (int k = 0; k < 24; k++) {
            float t = k / 23.f, u = 1.f - t;
            a[k] = (UtttPt){ u * u * x1 + 2 * u * t * mx + t * t * ex,
                             u * u * y1 + 2 * u * t * my + t * t * ey };
        }
        r_line(d, a, 24, 1.2f, INK_O);
        UtttPt h1[2] = { { ex, ey }, { ex - 1.f, ey + 5.f } };
        UtttPt h2[2] = { { ex, ey }, { ex - 5.f, ey + 1.f } };
        r_line(d, h1, 2, 1.2f, INK_O);
        r_line(d, h2, 2, 1.2f, INK_O);
        break;
    }
    case 4:                              /* sent to a won block: anywhere */
        r_wash(d, 9);
        r_grid(d, seed);
        r_cell(4, 2, &cx, &cy);
        r_mark(d, B, UTTT_X, cx, cy, 2.4f, 1.1f, 1.f, sd(seed, 1000, 38));
        r_won(d, B, 2, UTTT_O, 2.f, seed);
        break;
    case 5: {                            /* the promise round where they go */
        r_wash(d, 4);
        r_grid(d, seed);
        r_cell(4, 2, &cx, &cy);
        r_mark(d, B, UTTT_X, cx, cy, 2.4f, 1.1f, 1.f, sd(seed, 1000, 38));
        float r[4];
        uttt_wash_rect(2, r, NULL);
        for (int k = 0; k < 4; k++) r[k] = (k < 2 ? rf(B, r[k] * 90.f) : r[k] * 90.f * B.k);
        promise_box(d, r, OUTLINE_OVERSHOOT * 90.f * B.k, 1.8f * B.k, seed, 2 * 31, 1.f);
        break;
    }
    case 6: {                            /* the draft faded, a new tap in the tint */
        r_wash(d, 4);
        r_grid(d, seed);
        r_cell(4, 2, &cx, &cy);
        r_mark(d, B, UTTT_X, cx, cy, 2.4f, 1.1f, .35f, sd(seed, 1000, 38));
        /* a dashed ring where the finger lands: 11 across, dashes of two */
        r_cell(4, 6, &cx, &cy);
        const float rr = 5.5f, dash = 2.f;
        const int n = (int)(2.f * 3.14159265f * rr / (2.f * dash));
        for (int k = 0; k < n; k++) {
            UtttPt q[6];
            float a0 = (float)k / n * 6.2831853f;
            for (int j = 0; j < 6; j++) {
                float a = a0 + dash / rr * j / 5.f;
                q[j] = (UtttPt){ cx + rr * cosf(a), cy + rr * sinf(a) };
            }
            r_line(d, q, 6, .9f, INK_X);
        }
        break;
    }
    case 7:                              /* X moves first: one large X */
        r_mark(d, R_WHOLE, UTTT_X, 45.f, 45.f, 24.f, 5.f, 1.f / .8f, sd(seed, 1000, 0));
        break;
    }
    return (d->n_poly < d->cap_poly && d->n_pt < d->cap_pt) ? 0 : -1;
}

/* The drawer's board side the promise's pen is sized for: the phrase's box
 * is drawn as the promise is drawn round a block at this size. */
#define RULES_BOARD_PT 370.f

UtttRulesLook uttt_rules_look(void)
{
    UtttRulesLook L;
    L.margin_x = 16.f; L.top = 28.f; L.bottom = 26.f;
    L.title_pt = 21.f; L.title_gap = 14.f;
    L.art = 62.f; L.art_gap = 12.f; L.row_gap = 14.f;
    L.body_pt = 15.f; L.body_lead = 1.42f;
    L.box_pad = 2.f; L.word_room = 6.f;
    L.tint_pad_x = 4.f; L.tint_pad_y = 1.f;
    L.ink = UTTT_INK;
    L.tint = uttt_wash_rgba(.38f);
    return L;
}

int uttt_draw_rule_box(UtttDL *d, float w, float h)
{
    if (!(w > 0.f && h > 0.f)) return -1;
    const int first = d->n_pt;
    const float k = 1.f / RULES_BOARD_PT;              /* points -> board units */
    const float p = uttt_rules_look().box_pad;
    const float r[4] = { p * k, p * k, (w - 2.f * p) * k, (h - 2.f * p) * k };
    promise_box(d, r, 1.5f * k, 2.f * k, 7, 0, 1.f);
    for (int i = first; i < d->n_pt; i++) {
        d->pt[i].x = d->pt[i].x / k / w;
        d->pt[i].y = d->pt[i].y / k / h;
    }
    return (d->n_poly < d->cap_poly && d->n_pt < d->cap_pt) ? 0 : -1;
}
