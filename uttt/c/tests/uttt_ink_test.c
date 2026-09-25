/* THE INK: one polygon per stroke, the pen's own shape, no holes, and the
 * alpha table (TESTFLIGHT_PLAN.md 20).
 *
 * A stroke used to be a quad per segment and a disc per sample, overlapping at
 * every sample, so translucent ink came out as a bead there. uttt_ink now
 * outlines the stroke and the host fills that once. What can go wrong with an
 * outline is geometry: a join that twists the band at a sharp turn sums to a
 * winding of zero and leaves a HOLE in the ink, and an outline that is not the
 * pen's shape grows or loses ink. Both are checked against the pen's own
 * pieces - each segment's band at the width the pen gives its two ends -
 * rebuilt here from uttt_pen_width, over every kind of stroke the board draws
 * (marks at their three sizes, grid lines, the win line), at many seeds, and
 * drawn in part the way the animation draws them. */
#include "../src/uttt.h"
#include "../src/uttt_draw.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)

static UtttPt   PT[400000];
static UtttPoly PO[4000];

/* the winding number of `p` round the polygon, as a nonzero fill counts it */
static int winding(const UtttPt *q, int n, float x, float y)
{
    int w = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float x0 = q[j].x, y0 = q[j].y, x1 = q[i].x, y1 = q[i].y;
        float cr = (x1 - x0) * (y - y0) - (x - x0) * (y1 - y0);
        if (y0 <= y) { if (y1 > y && cr > 0) w++; }
        else         { if (y1 <= y && cr < 0) w--; }
    }
    return w;
}

static float seg_dist(float x, float y, UtttPt a, UtttPt b, float *f)
{
    float dx = b.x - a.x, dy = b.y - a.y, L2 = dx * dx + dy * dy;
    float t = L2 > 0 ? ((x - a.x) * dx + (y - a.y) * dy) / L2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    *f = t;
    float ex = a.x + dx * t - x, ey = a.y + dy * t - y;
    return sqrtf(ex * ex + ey * ey);
}

/* One stroke, `m` of its `n` points drawn: inked, and checked against its
 * pieces. Adds to the tallies; returns 0 when the ink made no polygon. */
static long holes, spills, deep, filled, strokes, multi;
static int check_stroke(const UtttPt *pts, int n, int m, const UtttPen *p)
{
    UtttDL d; uttt_dl_init(&d, PT, 400000, PO, 4000);
    uttt_ink_part(&d, pts, n, m, p);
    if (d.n_poly == 0) return 0;
    strokes++;
    if (d.n_poly != 1) multi++;
    const UtttPt *q = &d.pt[d.poly[0].first];
    int nq = d.poly[0].n;

    /* the pieces: the drawn samples, repeats dropped, each with its width */
    static UtttPt c[600]; static float hw[600];
    int k = 0;
    for (int i = 0; i < m && k < 600; i++) {
        if (k && hypotf(pts[i].x - c[k - 1].x, pts[i].y - c[k - 1].y) < 1e-5f) continue;
        c[k] = pts[i];
        hw[k++] = uttt_pen_width(p, pts[i].x, pts[i].y, (float)i / (float)(n - 1)) * .5f;
    }

    /* NO HOLE: every point well inside a segment's band - within .8 of the
     * narrower end's half-width of the segment, and not in the last tenth of
     * its ends, where a bisector join thins the band by up to 3.4% - is ink */
    for (int j = 0; j + 1 < k; j++) {
        float h = fminf(hw[j], hw[j + 1]) * .8f;
        float ux = c[j + 1].x - c[j].x, uy = c[j + 1].y - c[j].y, L = hypotf(ux, uy);
        ux /= L; uy /= L;
        for (int a = 1; a < 10; a++)
            for (int b = -4; b <= 4; b++) {
                float f = a / 10.f, o = h * b / 4.f;
                float x = c[j].x + (c[j + 1].x - c[j].x) * f - uy * o;
                float y = c[j].y + (c[j + 1].y - c[j].y) * f + ux * o;
                deep++;
                if (!winding(q, nq, x, y)) holes++;
            }
    }

    /* NO SPILL: every point of the fill is within the pen's reach of the
     * path - its half-width where it is, and a hair for rounding */
    float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f }, hmax = 0;
    for (int i = 0; i < nq; i++) {
        lo[0] = fminf(lo[0], q[i].x); hi[0] = fmaxf(hi[0], q[i].x);
        lo[1] = fminf(lo[1], q[i].y); hi[1] = fmaxf(hi[1], q[i].y);
    }
    for (int i = 0; i < k; i++) hmax = fmaxf(hmax, hw[i]);
    const int G = 48;
    for (int gy = 0; gy < G; gy++)
        for (int gx = 0; gx < G; gx++) {
            float x = lo[0] + (hi[0] - lo[0]) * (gx + .5f) / G;
            float y = lo[1] + (hi[1] - lo[1]) * (gy + .5f) / G;
            if (!winding(q, nq, x, y)) continue;
            filled++;
            int near = 0;
            for (int j = 0; j + 1 < k && !near; j++) {
                float f, dd = seg_dist(x, y, c[j], c[j + 1], &f);
                float h = hw[j] + (hw[j + 1] - hw[j]) * f;
                if (dd <= fmaxf(h, fmaxf(hw[j], hw[j + 1])) * 1.001f + hmax * 1e-3f) near = 1;
            }
            if (!near) spills++;
        }
    return 1;
}

/* a mark's strokes in its hundred-unit square, as uttt_draw.c's mark_geom
 * makes them: `s100` is the mark's side in hundredths of the board */
static int mark_pts(int kind, float s100, int32_t seed, UtttPt *pts, int cap, UtttSpan *sp)
{
    int np = 0;
    UtttRough r = uttt_rough_default((int32_t)((uint32_t)seed * 97u + 3u));
    r.roughness = 1.5f * powf(8.9f / s100, .75f) * (s100 / 8.9f);
    r.bowing    = 1.0f * powf(8.9f / s100, .85f);
    const float L = .3f;
    if (kind == UTTT_X) {
        int k = uttt_rough_line(&r, 10 - L*4, 11, 94 + L*3, 92 - L*6, pts, cap, &np, sp, 2);
        return k + uttt_rough_line(&r, 92 + L*4, 12, 13 - L*5, 90 + L*5, pts, cap, &np, sp + k, 2);
    }
    return uttt_rough_ellipse(&r, 50 + L*4, 50 - L*3, 78 - L*10, 76 + L*8, pts, cap, &np, sp, 2);
}

/* the number of strokes uttt_draw_board lays for `g` with `last` doubled:
 * the grid, the marks, the big marks and the win line */
static int strokes_for(const UtttGame *g, int last)
{
    int n = 9 * 4 * 2 + 2 * 4 * 2;                  /* nine hashes, the main one twice */
    for (int mv = 0; mv < 81; mv++) {
        int v = uttt_cell(g, mv);
        if (!v) continue;
        int k = v == UTTT_X ? 4 : 2;
        n += mv == last ? 2 * k : k;
    }
    for (int b = 0; b < 9; b++) {
        int v = uttt_block(g, b);
        if (v == UTTT_X) n += 4;
        else if (v == UTTT_O) n += 2;
    }
    if (g->over == UTTT_X || g->over == UTTT_O) n += 4;
    return n;
}

int main(void)
{
    /* ---- the alpha table: every stroke's alpha x .90/.80, capped at one */
    {
        static const struct { uint32_t in, out; const char *what; } T[] = {
            { 0x25376bccu, 0x25376be6u, "a mark: .80 -> .90" },
            { 0x25376b45u, 0x25376b4eu, "a mark in a won block: .27 -> .31" },
            { 0x2f2b2680u, 0x2f2b2690u, "a minor grid line: .50 -> .56" },
            { 0x2f2b26b8u, 0x2f2b26cfu, "the main lines' second pass: .72 -> .81" },
            { 0x2f2b26e6u, 0x2f2b26ffu, "the main lines' first pass: .90 -> opaque" },
            { 0x25376b9eu, 0x25376bb2u, "a big mark: .62 -> .70" },
            { 0x25376b66u, 0x25376b73u, "a door's hachure: .40 -> .45" },
            { 0x1b2a52d8u, 0x1b2a52f3u, "the rulebook's page: .85 -> .95" },
            { 0x1b2a52ffu, 0x1b2a52ffu, "an opaque edge stays opaque" },
            { 0x25376b00u, 0x25376b00u, "nothing stays nothing" },
        };
        for (size_t i = 0; i < sizeof T / sizeof *T; i++) {
            if (uttt_ink_rgba(T[i].in) != T[i].out)
                printf("  %s: got %08x\n", T[i].what, uttt_ink_rgba(T[i].in));
            OK(uttt_ink_rgba(T[i].in) == T[i].out, T[i].what);
        }

        /* and uttt_ink lays a stroke at its pen's alpha times the gain, one
         * number for the whole stroke, drawn in or not */
        UtttPt line[23];
        for (int i = 0; i < 23; i++) { line[i].x = 10.f + i * 3.f; line[i].y = 20.f + sinf(i * .3f); }
        UtttPen p = uttt_pen_92(); p.agrain = 0;
        const float want[4] = { .8f, .5f, .27f, .95f };
        const uint32_t got_want[4] = { 230, 143, 77, 255 };
        for (int i = 0; i < 4; i++) {
            UtttDL d; uttt_dl_init(&d, PT, 400000, PO, 4000);
            p.a = want[i];
            uttt_ink(&d, line, 23, &p);
            uttt_ink_part(&d, line, 23, 9, &p);
            OK(d.n_poly == 2 && (d.poly[0].rgba & 0xffu) == got_want[i]
               && d.poly[1].rgba == d.poly[0].rgba,
               "a stroke's alpha is its pen's x .90/.80, the same drawn in part");
        }
        /* with the sheet's grain on, the pen's alpha changes along the line,
         * and a stroke drawn in part is still the whole stroke's colour */
        UtttDL d;
        p = uttt_pen_92();
        int same = 1, varies = 0;
        for (int m = 2; m < 23; m++) {
            uttt_dl_init(&d, PT, 400000, PO, 4000);
            uttt_ink(&d, line, 23, &p);
            uttt_ink_part(&d, line, 23, m, &p);
            if (d.n_poly != 2 || d.poly[1].rgba != d.poly[0].rgba) same = 0;
            float a0 = uttt_pen_alpha(&p, line[0].x, line[0].y);
            if (fabsf(uttt_pen_alpha(&p, line[m].x, line[m].y) - a0) > .01f) varies = 1;
        }
        OK(varies && same, "a stroke drawn in part is the whole stroke's colour at every length");
        uttt_dl_init(&d, PT, 400000, PO, 4000);
        uttt_ribbon(&d, line, 23, 1.f, 0x25376b66u);
        OK(d.n_poly == 1 && d.poly[0].rgba == 0x25376b73u, "a ribbon's alpha is raised the same");
    }

    /* ---- every stroke is one polygon, the pen's shape, with no hole */
    {
        UtttPt pts[4000]; UtttSpan sp[8];
        UtttPen base = uttt_pen_92();
        int32_t seed = 1;
        /* marks: a cell's (8.9), a big mark's (28), the you-are mark's (88),
         * and the last mark's heavy pen */
        const float size[3] = { 8.9f, 28.f, 88.f };
        for (int it = 0; it < 600; it++) {
            seed = (int32_t)((uint32_t)seed * 1103515245u + 12345u) & 0x7fffffff;
            int kind = it % 2 ? UTTT_O : UTTT_X;
            float s100 = size[it % 3];
            int ns = mark_pts(kind, s100, seed, pts, 4000, sp);
            UtttPen p = base;
            if (it % 5 == 0) { p.w *= 2.2f; p.grain = .22f; }
            if (it % 7 == 0) p.w *= 1.15f;
            for (int s = 0; s < ns; s++) {
                check_stroke(pts + sp[s].first, sp[s].n, sp[s].n, &p);
                /* drawn in, as the animation draws it */
                int m = 2 + (int)((uint32_t)seed % (uint32_t)(sp[s].n - 1));
                check_stroke(pts + sp[s].first, sp[s].n, m, &p);
            }
        }
        /* long lines: the grid's and the win line's shapes, in a hundred-unit
         * board, at their widths */
        for (int it = 0; it < 600; it++) {
            seed = (int32_t)((uint32_t)seed * 1103515245u + 12345u) & 0x7fffffff;
            UtttRough r = uttt_rough_default(seed);
            r.roughness = it % 2 ? 1.5f : 3.4f; r.bowing = it % 2 ? 2.f : 1.f;
            r.seg_line = it % 3 ? 18 : 24;
            int np = 0;
            int ns = uttt_rough_line(&r, 16.f, 50.f + it % 7, 84.f, 50.f - it % 5, pts, 4000, &np, sp, 2);
            UtttPen p = base;
            p.w = base.w / 9.f * (it % 2 ? 5.1f : 1.7f); p.vel = 0; p.lift = .3f;
            for (int s = 0; s < ns; s++) check_stroke(pts + sp[s].first, sp[s].n, sp[s].n, &p);
        }
        printf("  ink: %ld strokes, %ld deep samples, %ld holes; %ld filled samples, %ld spills\n",
               strokes, deep, holes, filled, spills);
        OK(multi == 0, "every stroke is exactly one polygon");
        OK(holes == 0, "no stroke has a hole where the pen turned");
        OK(spills == 0, "no stroke's fill reaches past its pen");
    }

    /* ---- a finished board is one polygon per stroke: a few hundred, where
     * a quad and a disc per sample made it tens of thousands */
    {
        uint64_t r = 88172645463325252ull;
        int exact = 1, games = 0, lo = 1 << 30, hi = 0;
        long sum = 0;
        while (games < 400) {
            UtttGame g; uttt_init(&g);
            uint8_t mv[81];
            while (!g.over) {
                int n = uttt_legal(&g, mv);
                r ^= r << 13; r ^= r >> 7; r ^= r << 17;
                uttt_play(&g, mv[r % (uint64_t)n]);
            }
            UtttDL d; uttt_dl_init(&d, PT, 400000, PO, 4000);
            UtttDrawOpts o = uttt_draw_opts((int32_t)(r & 0x7fffffff) | 1);
            o.last = g.move[g.n_plies - 1];
            if (uttt_draw_board(&d, &g, &o) != 0) { exact = 0; break; }
            if (d.n_poly != strokes_for(&g, o.last)) exact = 0;
            lo = d.n_poly < lo ? d.n_poly : lo; hi = d.n_poly > hi ? d.n_poly : hi;
            sum += d.n_poly; games++;
        }
        printf("  finished boards: %d to %d polygons, mean %.0f\n", lo, hi, (double)sum / games);
        OK(exact, "a finished board's polygons are its strokes, one each");
        OK(hi < 600, "a finished board is a few hundred polygons");
    }

    printf("uttt_ink: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
