/* LOOK AT THE PEN, AND MEASURE IT, without Xcode.
 *
 * `make render` draws one board to eyeball. This draws the pieces the owner
 * reviews on TestFlight at a phone's own scale, the way Core Graphics fills
 * them (UtttInkImage: each polygon filled in order, source-over, nonzero,
 * antialiased), and prints numbers where a number settles the question:
 *
 *   look door W H SCALE OUT.ppm        the door, stretched to W x H points as
 *                                      UtttInkImage stretches it, and the ink
 *                                      thickness of each of its four edges in
 *                                      pixels, across the edge, at several
 *                                      places along it
 *   look marks FIRST COUNT SCALE OUT   the "you are" O for COUNT game seeds
 *   look board CODE SIDE SCALE MODE OUT
 *                                      a replay code's finished board, SIDE
 *                                      points square at SCALE, compositing
 *                                      the ink by MODE (see `composite`)
 *
 * A measuring tool, so it is C (feedback: measuring tools are C too). Sheets
 * of several images are laid out by the caller.
 */
#include "../src/uttt_draw.h"
#include "../src/uttt_code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------ the image */
typedef struct { int w, h; float *px; } Img;      /* RGB, 0..1 */

static Img img_new(int w, int h)
{
    Img m = { w, h, calloc((size_t)w * h * 3, sizeof(float)) };
    return m;
}

static void img_paper(Img *m)
{
    uint8_t *p = malloc((size_t)m->w * m->h * 4);
    uttt_paper(p, m->w, m->h);
    for (int i = 0; i < m->w * m->h; i++)
        for (int c = 0; c < 3; c++) m->px[i * 3 + c] = p[i * 4 + c] / 255.f;
    free(p);
}

static void img_write(const Img *m, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", m->w, m->h);
    for (int i = 0; i < m->w * m->h * 3; i++) {
        float v = m->px[i];
        fputc((int)(v < 0 ? 0 : v > 1 ? 255 : v * 255.f + .5f), f);
    }
    fclose(f);
}

/* --------------------------------------------------------- coverage */
/* One polygon's coverage of each pixel, 0..1, 4x4 samples, NONZERO winding
 * (what CGContextFillPath does). `cov` is the whole image; only the bbox is
 * written, and returned so the caller can walk just that. */
typedef struct { int x0, y0, x1, y1; } Box;

#define SS 4
static Box coverage(const UtttPt *p, int n, float *cov, int W, int H)
{
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        minx = fminf(minx, p[i].x); maxx = fmaxf(maxx, p[i].x);
        miny = fminf(miny, p[i].y); maxy = fmaxf(maxy, p[i].y);
    }
    Box b = { (int)floorf(minx), (int)floorf(miny), (int)ceilf(maxx) + 1, (int)ceilf(maxy) + 1 };
    if (b.x0 < 0) b.x0 = 0;
    if (b.y0 < 0) b.y0 = 0;
    if (b.x1 > W) b.x1 = W;
    if (b.y1 > H) b.y1 = H;
    for (int y = b.y0; y < b.y1; y++)
        for (int x = b.x0; x < b.x1; x++) cov[y * W + x] = 0.f;
    for (int y = b.y0; y < b.y1; y++)
        for (int sy = 0; sy < SS; sy++) {
            float py = y + (sy + .5f) / SS;
            /* crossings of this scan line, with their winding */
            float xs[512]; int ws[512], nx = 0;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                if ((p[i].y > py) == (p[j].y > py)) continue;
                if (nx >= 512) break;
                xs[nx] = (p[j].x - p[i].x) * (py - p[i].y) / (p[j].y - p[i].y) + p[i].x;
                ws[nx] = p[j].y > p[i].y ? 1 : -1;
                nx++;
            }
            if (!nx) continue;
            for (int x = b.x0; x < b.x1; x++)
                for (int sx = 0; sx < SS; sx++) {
                    float px = x + (sx + .5f) / SS;
                    int wind = 0;
                    for (int k = 0; k < nx; k++) if (px < xs[k]) wind += ws[k];
                    if (wind) cov[y * W + x] += 1.f / (SS * SS);
                }
        }
    return b;
}

static float chan(uint32_t rgba, int c) { return ((rgba >> (24 - 8 * c)) & 0xff) / 255.f; }

/* A display list's polygon, mapped to pixels: x by sx, y by sy, then moved. */
static int map_poly(const UtttDL *d, int i, UtttPt *out, int cap,
                    float ox, float oy, float sx, float sy)
{
    int n = d->poly[i].n > cap ? cap : d->poly[i].n;
    for (int k = 0; k < n; k++) {
        out[k].x = ox + d->pt[d->poly[i].first + k].x * sx;
        out[k].y = oy + d->pt[d->poly[i].first + k].y * sy;
    }
    return n;
}

/* Core Graphics: each polygon in order, source-over at its own alpha. */
static void fill_cg(Img *m, const UtttDL *d, float ox, float oy, float sx, float sy,
                    uint32_t only)
{
    float *cov = malloc((size_t)m->w * m->h * sizeof(float));
    UtttPt q[1024];
    for (int i = 0; i < d->n_poly; i++) {
        if (only && d->poly[i].rgba != only) continue;
        int n = map_poly(d, i, q, 1024, ox, oy, sx, sy);
        Box b = coverage(q, n, cov, m->w, m->h);
        float a = chan(d->poly[i].rgba, 3);
        for (int y = b.y0; y < b.y1; y++)
            for (int x = b.x0; x < b.x1; x++) {
                float k = cov[y * m->w + x] * a;
                if (k <= 0.f) continue;
                float *p = &m->px[(y * m->w + x) * 3];
                for (int c = 0; c < 3; c++) p[c] += (chan(d->poly[i].rgba, c) - p[c]) * k;
            }
    }
    free(cov);
}

static UtttPt   pool[600000];
static UtttPoly polys[160000];

/* --------------------------------------------------------------- door */
/* Ink thickness across an edge: the paper's luminance less the pixel's, over
 * the paper's less the ink's, summed across the edge - the width in pixels a
 * fully inked line of the same total ink would have. Summing rather than
 * thresholding makes a half-covered antialiased pixel count as half, so a
 * line that is thinner reads thinner even when both touch the same pixels. */
static float lum(const float *p) { return .2126f * p[0] + .7152f * p[1] + .0722f * p[2]; }

static float across(const Img *ink, const Img *bare, int x0, int y0, int dx, int dy, int len,
                    float ink_l)
{
    float sum = 0.f;
    for (int k = 0; k < len; k++) {
        int x = x0 + dx * k, y = y0 + dy * k;
        if (x < 0 || y < 0 || x >= ink->w || y >= ink->h) continue;
        float pl = lum(&bare->px[(y * ink->w + x) * 3]);
        float v = (pl - lum(&ink->px[(y * ink->w + x) * 3])) / (pl - ink_l);
        sum += v < 0 ? 0 : v > 1 ? 1 : v;
    }
    return sum;
}

static int door(int argc, char **argv)
{
    if (argc < 6) return 2;
    float w = (float)atof(argv[2]), h = (float)atof(argv[3]), sc = (float)atof(argv[4]);
    const int pad = 12;
    int W = (int)ceilf(w * sc) + 2 * pad, H = (int)ceilf(h * sc) + 2 * pad;

    UtttDL d; uttt_dl_init(&d, pool, 600000, polys, 160000);
    int rc = uttt_draw_door(&d, w, h);

    Img full = img_new(W, H), edge = img_new(W, H), bare = img_new(W, H);
    img_paper(&full); img_paper(&edge); img_paper(&bare);
    fill_cg(&full, &d, pad, pad, w * sc, h * sc, 0);
    /* THE EDGE ALONE, for the measurement: the hachure crosses every section
     * taken near the edge and would be counted as edge. 0x1b2a52ff is
     * uttt_rule.c's EDGE. */
    fill_cg(&edge, &d, pad, pad, w * sc, h * sc, 0x1b2a52ffu);
    img_write(&full, argv[5]);

    const float ink_l = .2126f * chan(0x1b2a52ffu, 0) + .7152f * chan(0x1b2a52ffu, 1)
                      + .0722f * chan(0x1b2a52ffu, 2);
    /* sections: 9 along each edge, 10% to 90%, each reaching 9 points in */
    int reach = (int)(9.f * sc);
    float t[4] = { 0 }, lo[4], hi[4];
    for (int e = 0; e < 4; e++) { lo[e] = 1e9f; hi[e] = 0.f; }
    for (int e = 0; e < 4; e++)
        for (int k = 1; k <= 9; k++) {
            float f = k / 10.f;
            float v;
            if (e == 0)      v = across(&edge, &bare, pad + (int)(f * w * sc), 0, 0, 1, pad + reach, ink_l);
            else if (e == 1) v = across(&edge, &bare, pad + (int)(f * w * sc), H - 1, 0, -1, pad + reach, ink_l);
            else if (e == 2) v = across(&edge, &bare, 0, pad + (int)(f * h * sc), 1, 0, pad + reach, ink_l);
            else             v = across(&edge, &bare, W - 1, pad + (int)(f * h * sc), -1, 0, pad + reach, ink_l);
            t[e] += v / 9.f;
            lo[e] = fminf(lo[e], v); hi[e] = fmaxf(hi[e], v);
        }
    static const char *nm[4] = { "top", "bottom", "left", "right" };
    printf("door %gx%g @%gx%s:", w, h, sc, rc ? " OVERFLOW" : "");
    for (int e = 0; e < 4; e++) printf("  %s %.2fpx (%.2f-%.2f)", nm[e], t[e], lo[e], hi[e]);
    printf("\n");
    return 0;
}

/* -------------------------------------------------------------- marks */
static int marks(int argc, char **argv)
{
    if (argc < 6) return 2;
    int first = atoi(argv[2]), count = atoi(argv[3]);
    float sc = (float)atof(argv[4]);
    const float side = 46.f;                        /* the header's mark */
    int cols = count < 10 ? count : 10, rows = (count + cols - 1) / cols;
    int cell = (int)(side * sc) + 8;
    Img m = img_new(cols * cell, rows * cell);
    img_paper(&m);
    for (int i = 0; i < count; i++) {
        UtttDL d; uttt_dl_init(&d, pool, 600000, polys, 160000);
        uttt_draw_mark(&d, UTTT_O, (first + i) + 4, 0.f);   /* model.seed &+ 4 */
        fill_cg(&m, &d, (i % cols) * cell + 4, (i / cols) * cell + 4, side * sc, side * sc, 0);
    }
    img_write(&m, argv[5]);
    return 0;
}

/* ------------------------------------------------ ink compositing options */
/* THE BEADS (owner, TestFlight 1.0(6): "little circles in the middle of the
 * lines"). uttt_ink lays each stroke as a quad per segment plus a round disc
 * at every sample, all at the stroke's alpha (.8 for a mark, .5 for a minor
 * grid line), so at every sample three translucent shapes overlap and the
 * ink there is 1-(1-a)^3 instead of a: a dot. These are the ways out,
 * rendered from the SAME display list so only the compositing differs.
 *
 * A stroke is recovered from the list, not asked of the kernel (nothing
 * shipped changes for this): uttt_ink's quads run A,B at the segment's start
 * and C,E at its end, so a quad whose start is the previous quad's end
 * continues the stroke, and each 9-point disc belongs to the quad before it. */
typedef struct { int first, n; uint32_t rgb; float a; } Stroke;

static int strokes(const UtttDL *d, Stroke *out, int cap)
{
    int ns = 0;
    float ex = 1e9f, ey = 1e9f;
    for (int i = 0; i < d->n_poly; i++) {
        const UtttPoly *p = &d->poly[i];
        const UtttPt *q = &d->pt[p->first];
        if (p->n == 4) {
            float sx = (q[0].x + q[1].x) * .5f, sy = (q[0].y + q[1].y) * .5f;
            int cont = ns && fabsf(sx - ex) < 1e-5f && fabsf(sy - ey) < 1e-5f
                       && (out[ns - 1].rgb == (p->rgba & 0xffffff00u));
            if (!cont && ns < cap) {
                out[ns].first = i; out[ns].n = 0;
                out[ns].rgb = p->rgba & 0xffffff00u; out[ns].a = 0.f; ns++;
            }
            ex = (q[2].x + q[3].x) * .5f; ey = (q[2].y + q[3].y) * .5f;
            out[ns - 1].a += chan(p->rgba, 3);
        }
        if (ns) out[ns - 1].n = i + 1 - out[ns - 1].first;
    }
    for (int k = 0; k < ns; k++) {                     /* mean quad alpha */
        int nq = 0;
        for (int i = out[k].first; i < out[k].first + out[k].n; i++) nq += d->poly[i].n == 4;
        out[k].a = nq ? out[k].a / nq : 1.f;
    }
    return ns;
}

/* e) ONE POLYGON PER STROKE: the quads' centreline and half-widths, outlined
 * down one side with bisector normals, a round cap, back up the other and a
 * round cap - filled once, nonzero, so nothing in a stroke overlaps. */
static int outline(const UtttDL *d, const Stroke *s, UtttPt *o, int cap)
{
    UtttPt c[512]; float hw[512]; int n = 0;
    for (int i = s->first; i < s->first + s->n && n + 2 < 512; i++) {
        if (d->poly[i].n != 4) continue;
        const UtttPt *q = &d->pt[d->poly[i].first];
        if (!n) {
            c[n].x = (q[0].x + q[1].x) * .5f; c[n].y = (q[0].y + q[1].y) * .5f;
            hw[n++] = hypotf(q[0].x - q[1].x, q[0].y - q[1].y) * .5f;
        }
        c[n].x = (q[2].x + q[3].x) * .5f; c[n].y = (q[2].y + q[3].y) * .5f;
        hw[n++] = hypotf(q[2].x - q[3].x, q[2].y - q[3].y) * .5f;
    }
    if (n < 2) return 0;
    float nx[512], ny[512];
    for (int i = 0; i < n; i++) {
        float ax = 0, ay = 0, bx = 0, by = 0, L;
        if (i > 0) { ax = c[i].x - c[i-1].x; ay = c[i].y - c[i-1].y; L = hypotf(ax, ay); if (L > 0) { ax /= L; ay /= L; } }
        if (i < n - 1) { bx = c[i+1].x - c[i].x; by = c[i+1].y - c[i].y; L = hypotf(bx, by); if (L > 0) { bx /= L; by /= L; } }
        float dx = ax + bx, dy = ay + by; L = hypotf(dx, dy);
        if (L < 1e-3f) { dx = bx ? bx : ax; dy = bx ? by : ay; L = hypotf(dx, dy); }
        if (L < 1e-9f) { dx = 1; dy = 0; L = 1; }
        nx[i] = -dy / L; ny[i] = dx / L;
    }
    int k = 0;
    enum { CAP = 6 };
    for (int i = 0; i < n && k < cap; i++) { o[k].x = c[i].x + nx[i] * hw[i]; o[k].y = c[i].y + ny[i] * hw[i]; k++; }
    for (int j = 1; j < CAP && k < cap; j++) {           /* the far cap */
        float t = (float)M_PI * j / CAP, cs = cosf(t), sn = sinf(t);
        float ux = nx[n-1] * cs + ny[n-1] * sn, uy = ny[n-1] * cs - nx[n-1] * sn;
        o[k].x = c[n-1].x + ux * hw[n-1]; o[k].y = c[n-1].y + uy * hw[n-1]; k++;
    }
    for (int i = n - 1; i >= 0 && k < cap; i--) { o[k].x = c[i].x - nx[i] * hw[i]; o[k].y = c[i].y - ny[i] * hw[i]; k++; }
    for (int j = 1; j < CAP && k < cap; j++) {           /* the near cap */
        float t = (float)M_PI * j / CAP, cs = cosf(t), sn = sinf(t);
        float ux = -nx[0] * cs - ny[0] * sn, uy = -ny[0] * cs + nx[0] * sn;
        o[k].x = c[0].x + ux * hw[0]; o[k].y = c[0].y + uy * hw[0]; k++;
    }
    return k;
}

/* Returns 0, or 1 for a mode it does not know.
 *   a  every polygon opaque, same colours
 *   b  each stroke opaque into its own layer, the layer at the stroke's alpha
 *   c  darken: each polygon opaque in its colour pre-mixed over the paper at
 *      its alpha, combined by min - ink never goes past one layer of itself
 *   d  each stroke's coverage x alpha into a mask by MAX, tinted once
 *   e  one outline polygon per stroke, at the stroke's alpha
 *   f  one layer for the whole sheet, alpha by MAX across strokes: nothing
 *      darkens, not even two strokes crossing */
static int composite(Img *m, const UtttDL *d, float ox, float oy, float sz, const char *mode)
{
    const int W = m->w, H = m->h;
    char k = mode[0];
    if (!strchr("abcdef", k) || mode[1]) return 1;
    static Stroke st[20000];
    int ns = strokes(d, st, 20000);
    fprintf(stderr, "strokes %d\n", ns);
    float *cov = malloc((size_t)W * H * sizeof(float));
    float *lay = calloc((size_t)W * H, sizeof(float));
    float *lrgb = k == 'f' ? calloc((size_t)W * H * 3, sizeof(float)) : NULL;
    float *sheet_a = k == 'f' ? calloc((size_t)W * H, sizeof(float)) : NULL;
    UtttPt q[2048];

    float pm[3] = { 0 };                       /* the paper's mean, for c */
    for (int i = 0; i < W * H; i++) for (int c = 0; c < 3; c++) pm[c] += m->px[i * 3 + c];
    for (int c = 0; c < 3; c++) pm[c] /= (float)(W * H);

    /* anything outside a stroke (a highlighter rect) is drawn as it ships */
    for (int i = 0, s = 0; i < d->n_poly; i++) {
        while (s < ns && st[s].first + st[s].n <= i) s++;
        if (s < ns && i >= st[s].first) continue;
        UtttDL one = *d; one.poly = (UtttPoly *)&d->poly[i]; one.n_poly = 1;
        fill_cg(m, &one, ox, oy, sz, sz, 0);
    }

    for (int s = 0; s < ns; s++) {
        float col[3] = { chan(st[s].rgb, 0), chan(st[s].rgb, 1), chan(st[s].rgb, 2) };
        Box bb = { W, H, 0, 0 };
        if (k == 'e') {
            int n = outline(d, &st[s], q, 2048);
            for (int j = 0; j < n; j++) { q[j].x = ox + q[j].x * sz; q[j].y = oy + q[j].y * sz; }
            Box b = coverage(q, n, cov, W, H);
            for (int y = b.y0; y < b.y1; y++)
                for (int x = b.x0; x < b.x1; x++) {
                    float a = cov[y * W + x] * st[s].a;
                    float *p = &m->px[(y * W + x) * 3];
                    for (int c = 0; c < 3; c++) p[c] += (col[c] - p[c]) * a;
                }
            continue;
        }
        for (int i = st[s].first; i < st[s].first + st[s].n; i++) {
            int n = map_poly(d, i, q, 2048, ox, oy, sz, sz);
            Box b = coverage(q, n, cov, W, H);
            float a = chan(d->poly[i].rgba, 3);
            for (int y = b.y0; y < b.y1; y++)
                for (int x = b.x0; x < b.x1; x++) {
                    float cv = cov[y * W + x];
                    if (cv <= 0.f) continue;
                    float *p = &m->px[(y * W + x) * 3], *l = &lay[y * W + x];
                    if (k == 'a') { for (int c = 0; c < 3; c++) p[c] += (col[c] - p[c]) * cv; }
                    else if (k == 'c') {
                        for (int c = 0; c < 3; c++) {
                            float sp = pm[c] * (1.f - a) + col[c] * a;
                            float mn = p[c] < sp ? p[c] : sp;
                            p[c] = p[c] * (1.f - cv) + mn * cv;
                        }
                    }
                    else if (k == 'b' || k == 'f') *l += cv * (1.f - *l);
                    else if (k == 'd') { if (cv * a > *l) *l = cv * a; }
                }
            if (b.x0 < bb.x0) bb.x0 = b.x0;
            if (b.y0 < bb.y0) bb.y0 = b.y0;
            if (b.x1 > bb.x1) bb.x1 = b.x1;
            if (b.y1 > bb.y1) bb.y1 = b.y1;
        }
        if (k == 'a' || k == 'c') continue;
        for (int y = bb.y0; y < bb.y1; y++)
            for (int x = bb.x0; x < bb.x1; x++) {
                float *l = &lay[y * W + x];
                if (*l <= 0.f) continue;
                if (k == 'f') {
                    /* the sheet layer: this stroke's colour over what is
                     * there by its coverage, the layer's alpha the most any
                     * stroke gave this pixel */
                    float *g = &lrgb[(y * W + x) * 3];
                    float over = sheet_a[y * W + x] > 0.f ? *l : 1.f;
                    for (int c = 0; c < 3; c++) g[c] += (col[c] - g[c]) * over;
                    float A = st[s].a * *l;
                    if (A > sheet_a[y * W + x]) sheet_a[y * W + x] = A;
                } else {
                    float a = k == 'b' ? st[s].a * *l : *l;
                    float *p = &m->px[(y * W + x) * 3];
                    for (int c = 0; c < 3; c++) p[c] += (col[c] - p[c]) * a;
                }
                *l = 0.f;
            }
    }
    if (k == 'f')
        for (int i = 0; i < W * H; i++) {
            float A = sheet_a[i];
            for (int c = 0; c < 3; c++) m->px[i * 3 + c] += (lrgb[i * 3 + c] - m->px[i * 3 + c]) * A;
        }
    free(cov); free(lay); free(lrgb); free(sheet_a);
    return 0;
}

/* -------------------------------------------------------------- board */
static int board(int argc, char **argv)
{
    if (argc < 7) return 2;
    UtttGame g; int32_t seed = 0;
    if (!uttt_replay_read(argv[2], &g, &seed)) { fprintf(stderr, "not a game: %s\n", argv[2]); return 1; }
    float side = (float)atof(argv[3]), sc = (float)atof(argv[4]);
    const char *mode = argv[5];
    int pad = (int)(side * sc * .06f);
    int W = (int)(side * sc) + 2 * pad;
    Img m = img_new(W, W);
    img_paper(&m);
    UtttDL d; uttt_dl_init(&d, pool, 600000, polys, 160000);
    UtttDrawOpts o = uttt_draw_opts(seed);
    int rc = uttt_draw_board(&d, &g, &o);
    fprintf(stderr, "seed %d plies %d over %d polys %d%s\n", seed, g.n_plies, g.over, d.n_poly,
            rc ? " OVERFLOW" : "");
    if (!strcmp(mode, "cg")) fill_cg(&m, &d, pad, pad, side * sc, side * sc, 0);
    else if (composite(&m, &d, pad, pad, side * sc, mode)) {
        fprintf(stderr, "unknown mode %s\n", mode); return 2;
    }
    img_write(&m, argv[6]);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "door"))  return door(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "marks")) return marks(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "board")) return board(argc, argv);
    fprintf(stderr, "usage: look door W H SCALE OUT | marks FIRST COUNT SCALE OUT"
                    " | board CODE SIDE SCALE MODE OUT\n");
    return 2;
}
