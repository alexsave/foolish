/* Rasterise the app icon out of the kernel, at any size, to a PPM on stdout.
 *
 *     ./build/uttt_icon 120 90 > icon.ppm
 *
 * A BUILD-TIME TOOL, not part of the app: the PNGs live in the asset
 * catalogues and Apple reads them out of the bundle. It exists so the icon is
 * drawn by the same pen as the board rather than traced by hand in a drawing
 * program, which is a second pen that drifts the moment the first one changes.
 *
 * Every size is drawn at ITS OWN size, not resampled from the big one: the
 * pen's grain and its minimum stroke width are in points, so a 1024-wide
 * drawing shrunk to 54 loses the ink and a 54-wide one blown up loses the
 * paper. Each call is the same hand drawing the same thing on smaller paper.
 */
#include "../src/uttt_draw.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define SS 3                     /* supersampling, per axis */

static float *fb;                /* w*h*3 */
static int    W, H;

static void blend(int x, int y, uint32_t rgba, float cov)
{
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    float a = (rgba & 0xff) / 255.f * cov;
    float c[3] = { ((rgba >> 24) & 0xff) / 255.f,
                   ((rgba >> 16) & 0xff) / 255.f,
                   ((rgba >>  8) & 0xff) / 255.f };
    float *p = fb + (y * (size_t)W + x) * 3;
    for (int i = 0; i < 3; i++) p[i] += (c[i] - p[i]) * a;
}

/* Nonzero winding, because that is what both iOS renderers fill with - an
 * even-odd rule punches a hole wherever a rough stroke crosses itself. */
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
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++) {
                    float px = x + (sx + .5f) / SS, py = y + (sy + .5f) / SS;
                    int wind = 0;
                    for (int i = 0, j = n - 1; i < n; j = i++) {
                        if (p[i].y <= py) {
                            if (p[j].y > py &&
                                (p[j].x - p[i].x) * (py - p[i].y)
                              - (px - p[i].x) * (p[j].y - p[i].y) > 0) wind++;
                        } else if (p[j].y <= py &&
                                (p[j].x - p[i].x) * (py - p[i].y)
                              - (px - p[i].x) * (p[j].y - p[i].y) < 0) wind--;
                    }
                    hits += wind != 0;
                }
            if (hits) blend(x, y, rgba, hits / (float)(SS * SS));
        }
}

static UtttPt   pool[400000];
static UtttPoly polys[120000];

int main(int argc, char **argv)
{
    W = argc > 1 ? atoi(argv[1]) : 120;
    H = argc > 2 ? atoi(argv[2]) : 90;
    /* The scale the icon is DRAWN at. Apple's numbers are points times a
     * scale, and the pen wants points - so 120x90 is the 60x45 icon at 2x and
     * has to be drawn as a 60-point icon, not a 120-point one. */
    float scale = argc > 3 ? (float)atof(argv[3]) : 1.f;
    if (W <= 0 || H <= 0 || scale <= 0) return 2;

    fb = malloc((size_t)W * H * 3 * sizeof *fb);
    if (!fb) return 1;

    {   /* the kernel's sheet, converted once into the float buffer the
         * scanline filler blends into */
        uint8_t *px = malloc((size_t)W * H * 4);
        if (!px) return 1;
        uttt_paper(px, W, H);
        for (int i = 0; i < W * H; i++)
            for (int c = 0; c < 3; c++) fb[i * 3 + c] = px[i * 4 + c] / 255.f;
        free(px);
    }

    UtttDL d; uttt_dl_init(&d, pool, 400000, polys, 120000);
    if (uttt_draw_icon(&d, W / scale, H / scale) != 0)
        fprintf(stderr, "icon %dx%d: display list overflowed\n", W, H);

    for (int i = 0; i < d.n_poly; i++) {
        /* a whole polygon: an O's ribbon runs past a hundred points at app
         * icon sizes, and a cut one fills as a chord across the ring */
        static UtttPt tmp[4096];
        int n = d.poly[i].n > 4096 ? 4096 : d.poly[i].n;
        for (int k = 0; k < n; k++) {
            tmp[k].x = d.pt[d.poly[i].first + k].x * W;
            tmp[k].y = d.pt[d.poly[i].first + k].y * H;
        }
        fill_poly(tmp, n, d.poly[i].rgba);
    }

    printf("P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H * 3; i++) {
        float v = fb[i];
        putchar((int)(v < 0 ? 0 : v > 1 ? 255 : v * 255.f + .5f));
    }
    return 0;
}
