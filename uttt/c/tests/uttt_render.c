/* Rasterise the kernel's display list to a PPM, so the C pen can be compared
 * against the design document without an Xcode in sight.
 *
 *     make -C uttt/c render && open uttt/c/build/board.png
 *
 * This is a TEST HARNESS, not a renderer anybody ships: iOS fills these
 * polygons with Core Graphics and the browser fills them with a canvas. What
 * it proves is that the geometry is right before either of them is written.
 */
#include "../src/uttt_draw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 740
#define H 740

static float fb[H][W][3];

static void blend(int x, int y, uint32_t rgba, float cov)
{
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    float a = (rgba & 0xff) / 255.f * cov;
    float r = ((rgba >> 24) & 0xff) / 255.f;
    float g = ((rgba >> 16) & 0xff) / 255.f;
    float b = ((rgba >>  8) & 0xff) / 255.f;
    fb[y][x][0] += (r - fb[y][x][0]) * a;
    fb[y][x][1] += (g - fb[y][x][1]) * a;
    fb[y][x][2] += (b - fb[y][x][2]) * a;
}

/* Scanline fill with 3x3 supersampling - the strokes are about a pixel wide
 * at these sizes and alias into nothing without it. */
static void fill_poly(const UtttPt *p, int n, uint32_t rgba)
{
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        if (p[i].x < minx) minx = p[i].x;
        if (p[i].x > maxx) maxx = p[i].x;
        if (p[i].y < miny) miny = p[i].y;
        if (p[i].y > maxy) maxy = p[i].y;
    }
    int x0 = (int)floorf(minx) - 1, x1 = (int)ceilf(maxx) + 1;
    int y0 = (int)floorf(miny) - 1, y1 = (int)ceilf(maxy) + 1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W; if (y1 > H) y1 = H;

    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int hits = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    float px = x + (sx + .5f) / 3.f, py = y + (sy + .5f) / 3.f;
                    /* NONZERO winding, because that is what fills these on
                     * the far side - CGContextFillPath and SwiftUI's
                     * GraphicsContext.fill both default to it. Even-odd
                     * punches a hole wherever a stroke laid as one polygon
                     * crosses itself, which is a hole nobody would ever
                     * see on a phone. */
                    int wind = 0;
                    for (int i = 0, j = n - 1; i < n; j = i++) {
                        if ((p[i].y > py) == (p[j].y > py)) continue;
                        float xc = (p[j].x - p[i].x) * (py - p[i].y)
                                   / (p[j].y - p[i].y) + p[i].x;
                        if (px < xc) wind += p[j].y > p[i].y ? 1 : -1;
                    }
                    hits += wind != 0;
                }
            if (hits) blend(x, y, rgba, hits / 9.f);
        }
}

/* THE KERNEL'S SHEET, not a copy of it. This function used to carry its own
 * napkin, which is how the same paper came to exist in three places. */
static void paper(void)
{
    static uint8_t px[H][W][4];
    uttt_paper(&px[0][0][0], W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int c = 0; c < 3; c++) fb[y][x][c] = px[y][x][c] / 255.f;
}

static void write_ppm(void)
{
    FILE *f = fopen("build/board.ppm", "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int c = 0; c < 3; c++) {
                float v = fb[y][x][c];
                fputc((int)(v < 0 ? 0 : v > 1 ? 255 : v * 255.f + .5f), f);
            }
    fclose(f);
}

static UtttPt   pool[400000];
static UtttPoly polys[120000];

/* `./build/uttt_render rulebook [side] [dump]`
 *
 * The button, ALONE and BLOWN UP, because the only way to know whether a
 * squiggle matches the design document is to look at it. `side` is the size
 * in POINTS the button is built at - 54 is the one that ships, and the image
 * is that shape magnified, not a bigger button.
 *
 * `dump` prints the strokes back as centrelines in point space, recovered
 * from the ribbon (its two halves straddle the sample, one forward and one
 * back), so the port can be diffed against rough.js itself rather than
 * eyeballed. */
static int rulebook(int argc, char **argv)
{
    float side = argc > 2 ? (float)atof(argv[2]) : 54.f;
    int   dump = argc > 3 && !strcmp(argv[3], "dump");

    UtttDL d; uttt_dl_init(&d, pool, 400000, polys, 120000);
    int rc = uttt_draw_rulebook(&d, side, side);

    if (dump) {
        for (int i = 0; i < d.n_poly; i++) {
            const UtttPt *q = &d.pt[d.poly[i].first];
            int m = d.poly[i].n / 2;
            for (int k = 0; k + 1 < m; k++) {
                float ax = (q[k].x + q[2*m-1-k].x) / 2 * side;
                float ay = (q[k].y + q[2*m-1-k].y) / 2 * side;
                float bx = (q[k+1].x + q[2*m-2-k].x) / 2 * side;
                float by = (q[k+1].y + q[2*m-2-k].y) / 2 * side;
                printf("%.4f %.4f %.4f %.4f\n", ax, ay, bx, by);
            }
        }
        return 0;
    }

    fprintf(stderr, "rulebook at %gpt: polys %d  points %d  overflow %s\n",
            side, d.n_poly, d.n_pt, rc ? "YES" : "no");

    paper();
    const float M = 40.f, SZ = W - 2 * M;
    for (int i = 0; i < d.n_poly; i++) {
        UtttPt tmp[64];
        int n = d.poly[i].n > 64 ? 64 : d.poly[i].n;
        for (int k = 0; k < n; k++) {
            tmp[k].x = M + d.pt[d.poly[i].first + k].x * SZ;
            tmp[k].y = M + d.pt[d.poly[i].first + k].y * SZ;
        }
        fill_poly(tmp, n, d.poly[i].rgba);
    }
    write_ppm();
    fprintf(stderr, "wrote build/board.ppm\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "rulebook")) return rulebook(argc, argv);

    UtttGame g; uttt_init(&g);
    int moves[] = { 34,67,44,80,76,43,69,62,79,63,4,40,39,31,37,16,70,71,72,3,
                    29,19,17,73,14,50,45,6,59,47,23,53,75,30,35,74,22,42,60,54,
                    0,2,24,58,38,18,1,9,5,77 };
    int upto = argc > 1 ? atoi(argv[1]) : (int)(sizeof moves / sizeof *moves);
    for (int i = 0; i < upto; i++) uttt_play(&g, (uint8_t)moves[i]);

    paper();

    UtttDL d; uttt_dl_init(&d, pool, 400000, polys, 120000);

    UtttDrawOpts o = uttt_draw_opts(77);
    o.last = upto ? moves[upto - 1] : -1;
    if (!g.over) {
        int f = g.forced;
        o.active = (f != UTTT_ANY && g.block[f] == UTTT_OPEN) ? f : 9;
    }
    int rc = uttt_draw_board(&d, &g, &o);
    fprintf(stderr, "polys %d  points %d  overflow %s\n",
            d.n_poly, d.n_pt, rc ? "YES" : "no");

    const float M = 28.f, SZ = W - 2 * M;
    for (int i = 0; i < d.n_poly; i++) {
        UtttPt tmp[64];
        int n = d.poly[i].n > 64 ? 64 : d.poly[i].n;
        for (int k = 0; k < n; k++) {
            tmp[k].x = M + d.pt[d.poly[i].first + k].x * SZ;
            tmp[k].y = M + d.pt[d.poly[i].first + k].y * SZ;
        }
        fill_poly(tmp, n, d.poly[i].rgba);
    }

    write_ppm();
    fprintf(stderr, "wrote build/board.ppm (%d plies)\n", g.n_plies);
    return 0;
}
