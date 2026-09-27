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
 *   look board CODE SIDE SCALE OUT     a replay code's finished board, SIDE
 *                                      points square at SCALE
 *
 * The ink-compositing options the owner chose between (TESTFLIGHT_PLAN.md
 * 19: a-f) were rendered by this tool at d7fae92d; option e is now the pen
 * itself (uttt_ink, one outline per stroke), so they are not kept here.
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

/* -------------------------------------------------------------- board */
static int board(int argc, char **argv)
{
    if (argc < 6) return 2;
    UtttGame g; int32_t seed = 0;
    if (!uttt_replay_read(argv[2], &g, &seed)) { fprintf(stderr, "not a game: %s\n", argv[2]); return 1; }
    float side = (float)atof(argv[3]), sc = (float)atof(argv[4]);
    int pad = (int)(side * sc * .06f);
    int W = (int)(side * sc) + 2 * pad;
    Img m = img_new(W, W);
    img_paper(&m);
    UtttDL d; uttt_dl_init(&d, pool, 600000, polys, 160000);
    UtttDrawOpts o = uttt_draw_opts(seed);
    int rc = uttt_draw_board(&d, &g, &o);
    fprintf(stderr, "seed %d plies %d over %d polys %d%s\n", seed, g.n_plies, g.over, d.n_poly,
            rc ? " OVERFLOW" : "");
    fill_cg(&m, &d, pad, pad, side * sc, side * sc, 0);
    img_write(&m, argv[5]);
    return 0;
}

/* `look moves CODE`: the seed and the moves, for the preview harness's
 * --seed and --moves (so a screenshot can show any finished game). */
static int moves(int argc, char **argv)
{
    UtttGame g; int32_t seed = 0;
    if (argc < 3 || !uttt_replay_read(argv[2], &g, &seed)) return 1;
    printf("%d ", seed);
    for (int i = 0; i < g.n_plies; i++) printf(i ? ",%d" : "%d", g.move[i]);
    printf("\n");
    return 0;
}

/* `look piece mark|door SCALE OUT`: the you-are O (46 points, the owner's
 * game's seed) or an SE Copy-code door (141.5 x 46). */
static int piece(int argc, char **argv)
{
    if (argc < 5) return 2;
    int door = !strcmp(argv[2], "door");
    float sc = (float)atof(argv[3]);
    float w = door ? 141.5f : 46.f, h = 46.f;
    int pad = (int)(6 * sc);
    Img m = img_new((int)ceilf(w * sc) + 2 * pad, (int)ceilf(h * sc) + 2 * pad);
    img_paper(&m);
    UtttDL d; uttt_dl_init(&d, pool, 600000, polys, 160000);
    if (door) uttt_draw_door(&d, w, h);
    else uttt_draw_mark(&d, UTTT_O, 1790219291 + 4, 0.f);
    fill_cg(&m, &d, pad, pad, w * sc, h * sc, 0);
    img_write(&m, argv[4]);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "piece")) return piece(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "moves")) return moves(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "door"))  return door(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "marks")) return marks(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "board")) return board(argc, argv);
    fprintf(stderr, "usage: look door W H SCALE OUT | marks FIRST COUNT SCALE OUT"
                    " | board CODE SIDE SCALE OUT | piece mark|door SCALE OUT"
                    " | moves CODE\n");
    return 2;
}
