#include "uttt_draw.h"
#include <math.h>
#include <string.h>

#define S   1.0f                 /* the board is a unit square */
#define BL  (S / 3.f)            /* a block */
#define CE  (S / 9.f)            /* a cell  */

#define INK_X   0x25376bffu
#define INK_O   0xa8321fffu
#define INK_GR  0x2f2b26ffu
#define WASH    0xd6a836ffu

/* One pen at two scales. A mark is drawn inside a CELL and the grid across the
 * whole BOARD, which is nine cells - so the same ball that is 2.7 units wide
 * on a mark is 2.7/9 here. Derived, never typed: getting this wrong by hand is
 * what made the first grid a thicket. */
#define REF   (8.9f / 100.f * CE * 9.f)   /* a normal mark, in board units */

static float rough_for(float L) { return 1.5f * powf(REF / (L > 1e-4f ? L : 1e-4f), .75f); }
static float bow_for  (float L) { return 1.0f * powf(REF / (L > 1e-4f ? L : 1e-4f), .85f); }
/* rough.js's maxRandomnessOffset of 2 is in the drawing's OWN units, and the
 * board here is a unit square rather than a hundred of them - so it converts
 * once, here, instead of at four call sites that would each forget. */
static float mro_for  (float L) { return .02f * powf(REF / (L > 1e-4f ? L : 1e-4f), .75f); }

UtttDrawOpts uttt_draw_opts(int32_t seed)
{
    UtttDrawOpts o;
    o.seed = seed ? seed : 1;
    o.active = -1; o.last = -1; o.mark_t = 1.f; o.meta_t = 1.f;
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
    uttt_ink(d, pts, m, p);
}

/* A MARK IS DRAWN IN ITS OWN HUNDRED-UNIT SQUARE and then transformed into
 * the board, which is exactly what the document does with ctx.scale. Doing it
 * the other way round means every rough.js constant - its maxRandomnessOffset
 * of 2, its ellipse offsets of 1 and 1.5 - has to be converted at the call
 * site, and the ellipse ones were not, so every O wandered across the whole
 * sheet. Convert the POINTS once instead of the constants everywhere. */
static void mark_in(UtttDL *d, int kind, float x, float y, float s,
                    int32_t seed, float t, const UtttPen *base)
{
    UtttPt pts[1400]; int np = 0;
    UtttSpan sp[4];
    const float s100 = s * 100.f;          /* the mark's size, board-100 */
    UtttRough r = uttt_rough_default(seed);
    r.roughness = 1.5f * powf(8.9f / s100, .75f) * (s100 / 8.9f);
    r.bowing    = 1.0f * powf(8.9f / s100, .85f);

    UtttPen p = *base;
    p.ink = kind == UTTT_O ? INK_O : INK_X;
    p.w   = base->w * s / 100.f;           /* stroke lives in board units */

    int k = 0, k2 = 0;
    if (kind == UTTT_X) {
        k  = uttt_rough_line(&r, 10, 11, 94, 92, pts, 1400, &np, sp, 2);
        k2 = uttt_rough_line(&r, 92, 12, 13, 90, pts, 1400, &np, sp + k, 2);
    } else {
        k  = uttt_rough_ellipse(&r, 50, 50, 78, 76, pts, 1400, &np, sp, 2);
    }
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

int uttt_draw_board(UtttDL *d, const UtttGame *g, const UtttDrawOpts *o)
{
    UtttPen base = uttt_pen_92();

    /* the block you are sent to, in highlighter - ink cannot say "here"
     * without also saying something it can never take back */
    if (o->active == 9) {
        rect(d, .01f, .01f, S - .02f, S - .02f, (WASH & 0xffffff00u) | 43u);
    } else if (o->active >= 0 && o->active < 9) {
        rect(d, (o->active % 3) * BL + .012f,
                (o->active / 3) * BL + .012f, BL - .024f, BL - .024f,
             (WASH & 0xffffff00u) | 77u);
    }

    for (int b = 0; b < 9; b++)
        hash_in(d, (b % 3) * BL, (b / 3) * BL, BL,
                o->seed * 131 + b * 17,
                base.w / 9.f / 100.f * .62f, BL * .03f, .5f, 1.5f);
    hash_in(d, 0, 0, S, o->seed * 7 + 3,
            base.w / 9.f / 100.f * 1.7f, S * .05f, .9f, 3.4f);
    hash_in(d, 0, 0, S, o->seed * 19 + 5,
            base.w / 9.f / 100.f * 1.5f, S * .045f, .72f, 3.4f);

    for (int b = 0; b < 9; b++) {
        int won = g->block[b] == UTTT_X || g->block[b] == UTTT_O;
        for (int c = 0; c < 9; c++) {
            int v = g->cell[b * 9 + c];
            if (!v) continue;
            float x = (b % 3) * BL + (c % 3) * CE;
            float y = (b / 3) * BL + (c / 3) * CE;
            int is_last = (b * 9 + c) == o->last;
            UtttPen p = base;
            if (won && !is_last) p.a = base.a * .34f;
            if (is_last) { p.w = base.w * 2.2f; p.a = 1.f; p.grain = .22f; }
            mark_in(d, v, x + CE * .1f, y + CE * .1f, CE * .8f,
                    o->seed * 1000 + b * 9 + c,
                    is_last ? o->mark_t : 1.f, &p);
            if (is_last) {
                p.w = base.w * 1.9f; p.a = .85f;
                mark_in(d, v, x + CE * .1f, y + CE * .1f, CE * .8f,
                        o->seed * 1000 + b * 9 + c + 613, o->mark_t, &p);
            }
        }
        if (won) {
            UtttPen p = base; p.a = .62f; p.w = 2.2f;
            mark_in(d, g->block[b], (b % 3) * BL + BL * .08f,
                    (b / 3) * BL + BL * .08f, BL * .84f,
                    o->seed * 77 + b, 1.f, &p);
        }
    }

    /* the win line: the only mark that crosses a thick line, in the winner's
     * own ink, and drawn twice because a ball has one width */
    if (g->over == UTTT_X || g->over == UTTT_O) {
        static const uint8_t L[8][3] = {
            {0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6} };
        for (int i = 0; i < 8; i++) {
            if (g->block[L[i][0]] != g->over || g->block[L[i][1]] != g->over
             || g->block[L[i][2]] != g->over) continue;
            int a = L[i][0], z = L[i][2];
            float ax = ((a % 3) + .5f) * BL, ay = ((a / 3) + .5f) * BL;
            float zx = ((z % 3) + .5f) * BL, zy = ((z / 3) + .5f) * BL;
            float len = sqrtf((zx-ax)*(zx-ax) + (zy-ay)*(zy-ay));
            const float W[2] = { 2.7f, 2.3f };
            const float A[2] = { .92f, .74f };
            const int32_t SD[2] = { 313, 977 };
            for (int q = 0; q < 2; q++) {
                UtttRough r = uttt_rough_default(o->seed * SD[q]);
                r.roughness = rough_for(len) * 2.2f;
                r.bowing    = bow_for(len) * 2.2f;
                r.max_offset = mro_for(len) * 2.2f;
                UtttPt pts[1024]; int np = 0; UtttSpan sp[2];
                int n = uttt_rough_line(&r, ax, ay, zx, zy, pts, 1024, &np, sp, 2);
                UtttPen p = uttt_pen_92();
                p.ink = g->over == UTTT_O ? INK_O : INK_X;
                p.w = base.w / 9.f / 100.f * W[q]; p.a = A[q];
                p.vel = 0; p.lift = .2f; p.grain = .25f; p.agrain = .2f;
                for (int s2 = 0; s2 < n; s2++)
                    stroke(d, pts + sp[s2].first, sp[s2].n, &p, o->meta_t);
            }
            break;
        }
    }
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
            seed * 1000 + b * 9 + c, t, &p);
    return 0;
}

int uttt_draw_mark(UtttDL *d, int mark, int32_t seed, float calm)
{
    UtttPen p = uttt_pen_92();
    p.w = uttt_pen_92().w * 1.15f;
    mark_in(d, mark, .06f, .06f, .88f, seed, 1.f, &p);
    (void)calm;
    return 0;
}

/* ------------------------------------------------------------ the bubble */
/* Every number below is measured off the design document rather than chosen
 * here: 10 points of side padding, a 12-point gutter, and the board takes the
 * height because the height is what runs out first. 195 - 2*7 = 181, and
 * 300 - 10 - 181 - 12 - 10 = 87 for the text. Eighty-seven points is why the
 * place line is allowed to wrap and the headline is not. */
#define BUB_W    300.f
#define BUB_H    195.f
#define BUB_PAD   10.f
#define BUB_GUT   12.f

UtttBubble uttt_bubble(void)
{
    UtttBubble b;
    b.w = BUB_W; b.h = BUB_H;

    float side = BUB_H - 2.f * 7.f;          /* 181 */
    b.board.x = BUB_PAD;
    b.board.y = (BUB_H - side) * .5f;
    b.board.w = side;
    b.board.h = side;

    b.text.x = b.board.x + side + BUB_GUT;
    b.text.y = b.board.y;
    b.text.w = BUB_W - b.text.x - BUB_PAD;
    b.text.h = side;

    b.headline_pt = 16.f;
    b.place_pt    = 16.f;
    b.lead        = 1.f;
    b.headline_rgba = 0x1d1b16ffu;
    b.place_rgba    = INK_X;                 /* the same blue an X is drawn in */
    return b;
}

static const char *const PLACE[10] = {
    "top left",    "top middle",    "top right",
    "middle left", "centre",        "middle right",
    "bottom left", "bottom middle", "bottom right",
    "anywhere"
};
static const char *const PLACE_SPOKEN[10] = {
    "top-left",    "top-middle",    "top-right",
    "middle-left", "centre",        "middle-right",
    "bottom-left", "bottom-middle", "bottom-right",
    "anywhere"
};

const char *uttt_place_name(int block, int spoken)
{
    if (block < 0 || block > 9) return "";
    return spoken ? PLACE_SPOKEN[block] : PLACE[block];
}
