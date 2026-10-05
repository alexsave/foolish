/* cn_texgen: bakes build/cn_tex.pack (the layout is in src/cn_tex.h) and the
 * flat images the Swift host paints with. Host-only: libc, stdio and libm
 * (the planks' march), nothing else (no CoreGraphics).
 *
 *   cn_texgen --font tools/fonts/IMFeENrm28P.ttf --pack build/cn_tex.pack
 *   cn_texgen --font ... --compare REFDIR [--sheet OUT.png]
 *   cn_texgen --font ... --images DIR      the planks (2 texels a point, nails in), the verdigris
 *                                          and the bone tiles, as uncompressed PNGs (the Makefile's
 *                                          tex-ios recompresses them with sips for the bundle)
 *   cn_texgen --font ... --compare-planks STUDY_516x830.rgba [--sheet OUT.png]
 *                                          the planks at the study's own scale, without the nails
 *                                          (the study lays its nails over the tile as SVG), against
 *                                          the study's canvas captured by tools/cn_tex_capture.mjs
 *
 * THE TILES are the study's own generators (docs/UI.html, TEX): the integer
 * hash, value noise on a wrapping lattice, fbm, and the verdigris (verdWith
 * over VERD) and bone (stone over DIE_MATS.tallow) palettes, in IEEE double
 * with the JavaScript's own order of operations, so every texel is the one
 * the study's canvas stores (ImageData rounds half to even), bit for bit.
 * (-ffp-contract=off, set by the Makefile, keeps a fused multiply-add from
 * rounding once where JavaScript rounds twice.)
 *
 * THE NUMERALS are IM Fell English (SIL OFL, tools/fonts/), read from the
 * TrueType file by a small glyf parser here and rasterized with 16 sub-scanlines
 * a row and exact horizontal span coverage, nonzero winding. Placement is the
 * canvas's: textAlign 'center' (the pen at minus half the advance) and
 * textBaseline 'middle', which Chromium puts at the middle of the em box it
 * derives from OS/2 sTypoAscender / sTypoDescender (measured: 418.9 units of
 * 2048 above the baseline, the formula gives 418.92).
 *
 * --compare reads raw RGBA and bump files captured from the study in headless
 * Chromium (one file per texture, NAME_WxH.rgba / .bump), derives the same
 * textures through src/cn_tex.c from the pack just baked, prints the mean
 * absolute difference of each, and with --sheet writes a contact sheet PNG
 * (reference, C, difference times four) with a stored-deflate PNG writer. */
#include "../src/cn_tex.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the study's noise, exactly ------------------------------------------------------ */
static double tg_hash(uint32_t ix, uint32_t iy, uint32_t seed) { return cn_tex_hash(ix, iy, seed) / 4294967296.0; }
static double tg_floor(double x) { double t = (double)(long long)x; return t > x ? t - 1 : t; }
static int tg_wrap(long long i, int p) { return (int)(((i % p) + p) % p); }
static double tg_vnoise(double x, double y, int px, int py, uint32_t seed)
{
    double x0 = tg_floor(x), y0 = tg_floor(y), fx = x - x0, fy = y - y0;
    double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    long long ix = (long long)x0, iy = (long long)y0;
    double a = tg_hash((uint32_t)tg_wrap(ix, px), (uint32_t)tg_wrap(iy, py), seed), b = tg_hash((uint32_t)tg_wrap(ix + 1, px), (uint32_t)tg_wrap(iy, py), seed);
    double c = tg_hash((uint32_t)tg_wrap(ix, px), (uint32_t)tg_wrap(iy + 1, py), seed), d = tg_hash((uint32_t)tg_wrap(ix + 1, px), (uint32_t)tg_wrap(iy + 1, py), seed);
    double top = a + (b - a) * sx, bot = c + (d - c) * sx;
    return top + (bot - top) * sy;
}
/* fbm over a unit tile: cx by cy cells at octave 0, doubling each octave; gain .5 */
static double tg_fbm(double u, double v, int cx, int cy, int octaves, uint32_t seed)
{
    double sum = 0, amp = 1, norm = 0;
    for (int o = 0; o < octaves; o++) {
        sum += amp * tg_vnoise(u * cx, v * cy, cx, cy, seed + (uint32_t)o * 101);
        norm += amp; amp *= .5; cx *= 2; cy *= 2;
    }
    return sum / norm;
}
static double tg_sstep(double a, double b, double x) { double t = (x - a) / (b - a); t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }
static double tg_mix(double a, double b, double t) { return a + (b - a) * t; }
static double tg_abs(double x) { return x < 0 ? -x : x; }
/* ImageData's Uint8ClampedArray: clamped, rounded half to even */
static uint8_t tg_store(double v)
{
    if (!(v > 0)) return 0;
    if (v >= 255) return 255;
    int i = (int)v; double f = v - i;
    if (f > .5 || (f == .5 && (i & 1))) i++;
    return (uint8_t)i;
}

/* TEX.verdWith(VERD) */
static void tg_verd(double u, double v, double *o)
{
    const uint32_t seed = 37;
    double cl = (tg_fbm(u, v, 4, 4, 5, seed) - .5) * 2 * 12;
    double gr = (tg_fbm(u, v, 64, 64, 2, seed + 7) - .5) * 2 * 5;
    double bl = tg_sstep(.42, .60, tg_fbm(u, v, 7, 7, 4, seed + 13)) * .9;
    double ch = tg_sstep(.62, .74, tg_fbm(u, v, 14, 14, 3, seed + 19)) * bl * .75;
    double ru = tg_sstep(.76, .86, tg_fbm(u, v, 9, 9, 3, seed + 29)) * (1 - bl) * .7;
    static const double base[3] = { 58, 48, 32 }, bloom[3] = { 46, 78, 70 }, chalk[3] = { 104, 138, 122 }, rust[3] = { 104, 62, 38 };
    for (int k = 0; k < 3; k++) {
        double c = base[k] + cl + gr;
        c = tg_mix(c, bloom[k], bl); c = tg_mix(c, chalk[k], ch); c = tg_mix(c, rust[k], ru);
        o[k] = c + gr;
    }
}
/* TEX.stone(DIE_MATS.tallow): a cloud, a grain, veins, pores and a light crazing */
static void tg_bone(double u, double v, double *o)
{
    const uint32_t seed = 34;
    static const double base[3] = { 214, 206, 168 }, tilt[3] = { 1, .85, .3 }, vein[3] = { 132, 118, 84 }, crack[3] = { 156, 134, 94 };
    double cl = (tg_fbm(u, v, 4, 4, 4, seed) - .5) * 2 * 12;
    double gr = (tg_fbm(u, v, 64, 64, 2, seed + 7) - .5) * 2 * 5;
    double c[3];
    for (int k = 0; k < 3; k++) c[k] = base[k] + cl * tilt[k] + gr;
    double ridge = 1 - tg_abs(2 * tg_fbm(u, v, 3, 12, 4, seed + 13) - 1), kv = tg_sstep(.9, 1, ridge) * .35;
    for (int k = 0; k < 3; k++) c[k] = tg_mix(c[k], vein[k], kv);
    double kp = tg_sstep(.8, .9, tg_fbm(u, v, 90, 90, 2, seed + 19)) * 14;
    for (int k = 0; k < 3; k++) c[k] -= kp;
    double cr = 1 - tg_abs(2 * tg_fbm(u, v, 4, 4, 4, seed + 31) - 1), kc = tg_sstep(.968, 1, cr) * .7;
    for (int k = 0; k < 3; k++) o[k] = tg_mix(c[k], crack[k], kc);
}
/* TEX.bake: fn at each texel's centre, RGB */
static void tg_bake(int w, int h, void (*fn)(double, double, double *), uint8_t *rgb)
{
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        double c[3]; fn((x + .5) / w, (y + .5) / h, c);
        for (int k = 0; k < 3; k++) rgb[(y * w + x) * 3 + k] = tg_store(c[k]);
    }
}

/* ---- THE PLANKS: the study's table (TABLE_MATS.woodgrey2) ------------------------------------
 * TEX.tableFn(woodDeck(WOODS.greyDark)): foolish's streak march (streakWood, its palette a drowned
 * grey-teal, three linear gains, no orange), read plank by plank, under the stone passes the study
 * names for it (a cloud, a grain, veins, a stain, the crust's pale patches and their grit, a wet
 * sheen), then the gaps between the six planks and each plank's end in a running bond. The tile is
 * TG_PLANK_W * TG_PLANKS wide (516 points) and 830 tall, and it is seamless both ways: the gaps
 * fall on the tile's left and right edges, every noise wraps, and the march's one non-periodic
 * seam lands under each plank's end.
 *
 * THE NAILS (tableStage's fixSVG 'nails', which the study draws as SVG over the tile per stage)
 * are baked INTO the tile here: rows across every plank, one rose head just inside each edge,
 * the head's four hammered facets lit from the top-left, a dark halo and an iron bleed. The rows
 * are the study's rule (22 + up to 40 down, then every 92 to 116) with a row that would crowd the
 * tile's seam left out, so they repeat with the tile.
 *
 * `scale` is texels a point: 1 is the study's own canvas (and, without nails, its texels: the
 * capture comparison); 2 is what ships (cn_planks.jpg). At 2 the march (texel-sized by nature) is
 * read bilinearly, as a browser shows the study's canvas on a 2x screen, and every other pass is
 * evaluated at the finer texel. Uses libm's cos (the march's chaotic map) and atan2 (the nail). */
#include <math.h>
enum { TG_PLANK_W = 86, TG_PLANKS = 6, TG_TILE_W = TG_PLANK_W * TG_PLANKS, TG_TILE_H = 830 };
/* the shipped tile's texels a point (cn_planks.jpg is 1032 by 1660) */
#define CN_TEXGEN_PLANK_SCALE 2

/* streakWood(830, 516, WOODS.greyDark): the march, RGB floats, w across (a plank's length) */
static float *tg_streaks(void)
{
    enum { W = TG_TILE_H, H = TG_TILE_W, RECT = 40, STREAKS = 576, TDIV = 60 };
    static const double base[3] = { 9, 10, 10 }, gain[3] = { 8, 9, 9 }, alpha = .08;
    float *S = malloc(sizeof(float) * W * H * 3);
    for (int i = 0; i < W * H; i++) for (int k = 0; k < 3; k++) S[i * 3 + k] = (float)base[k];
    double step = 200.0 / TDIV, travel = STREAKS * step;
    int cropX = (int)tg_floor((travel - W) / 2); if (cropX < RECT) cropX = RECT;
    double al[RECT], ia[RECT];
    for (int dx = 0; dx < RECT; dx++) { double dist = tg_abs(dx - RECT / 2.0) / (RECT / 2.0), m = 1 - dist * .5; al[dx] = alpha * (m > .3 ? m : .3); ia[dx] = 1 - al[dx]; }
    /* JavaScript's Float32Array stores each blend rounded to float, so the buffer is float here too */
    for (int n = 0; n < STREAKS; n++) {
        double T = (double)n / TDIV; int start = (int)tg_floor(T * 200) - cropX;
        if (start >= W || start + RECT <= 0) continue;
        int kLo = start < 0 ? -start : 0, kHi = W - start < RECT ? W - start : RECT;
        for (int I = H - 1; I >= 0; I--) {
            double iF = I * .001, b = T / 24;
            for (int k = 24; k >= 0; k--) {
                b = cos(iF + cos(b * b * .5) * b + 4) * b - 2.8;
                if (b > 0) {
                    double c0 = b * gain[0], c1 = b * gain[1], c2 = b * gain[2];
                    for (int dx = kLo; dx < kHi; dx++) {
                        float *p = S + ((size_t)I * W + start + dx) * 3;
                        p[0] = (float)(c0 * al[dx] + p[0] * ia[dx]); p[1] = (float)(c1 * al[dx] + p[1] * ia[dx]); p[2] = (float)(c2 * al[dx] + p[2] * ia[dx]);
                    }
                }
            }
        }
    }
    return S;
}

/* the march as the plank pass reads it: plank pi takes its own band of rows, shifted along by `shift` */
static void tg_under(const float *S, double u, double v, int pi, double shift, int scale, double *o)
{
    enum { W = TG_TILE_H, H = TG_TILE_W };
    double band = (double)H / TG_PLANKS, off = tg_hash((uint32_t)pi, 4, 509) * (H - band);
    double fx = fmod(fmod(v + shift, 1) + 1, 1) * W, fy = (u * TG_PLANKS - pi) * band + off;
    if (scale == 1) {
        int x = (int)tg_floor(fx), y = (int)tg_floor(fy);
        if (x > W - 1) x = W - 1;
        if (y > H - 1) y = H - 1;
        for (int k = 0; k < 3; k++) o[k] = S[((size_t)y * W + x) * 3 + k];
        return;
    }
    fx -= .5; fy -= .5;
    int x0 = (int)tg_floor(fx), y0 = (int)tg_floor(fy); double tx = fx - x0, ty = fy - y0;
    int xa = x0 < 0 ? 0 : x0 > W - 1 ? W - 1 : x0, xb = x0 + 1 > W - 1 ? W - 1 : x0 + 1 < 0 ? 0 : x0 + 1;
    int ya = y0 < 0 ? 0 : y0 > H - 1 ? H - 1 : y0, yb = y0 + 1 > H - 1 ? H - 1 : y0 + 1 < 0 ? 0 : y0 + 1;
    for (int k = 0; k < 3; k++) {
        double a = S[((size_t)ya * W + xa) * 3 + k], b = S[((size_t)ya * W + xb) * 3 + k];
        double c = S[((size_t)yb * W + xa) * 3 + k], d = S[((size_t)yb * W + xb) * 3 + k];
        double top = a + (b - a) * tx, bot = c + (d - c) * tx;
        o[k] = top + (bot - top) * ty;
    }
}

/* TEX.stone(woodDeck(WOODS.greyDark)) at one texel */
static void tg_plank_texel(const float *S, double u0, double v0, int scale, double *o)
{
    const uint32_t seed = 73;
    static const double tilt[3] = { .8, 1, .9 }, vein[3] = { 4, 6, 5 }, stain[3] = { 5, 8, 8 }, sheen[3] = { 34, 40, 40 };
    static const double patch[3] = { 30, 48, 40 }, fleck[3] = { 80, 96, 80 };
    int pi = (int)tg_floor(u0 * TG_PLANKS); double shift = (pi % 2) * .5, u = u0, v = fmod(v0 + shift, 1);
    double cl = (tg_fbm(u, v, 4, 4, 4, seed) - .5) * 2 * 5;
    double gr = (tg_fbm(u, v, 64, 64, 2, seed + 7) - .5) * 2 * 3;
    double c[3]; tg_under(S, u0, v0, pi, shift, scale, c);
    for (int k = 0; k < 3; k++) c[k] += cl * tilt[k] + gr;
    double ridge = 1 - tg_abs(2 * tg_fbm(u, v, 36, 2, 4, seed + 13) - 1), kv = tg_sstep(.9, 1, ridge) * .3;
    for (int k = 0; k < 3; k++) c[k] = tg_mix(c[k], vein[k], kv);
    double ks = tg_sstep(.45, .8, tg_fbm(u, v, 4, 4, 3, seed + 23)) * .3;
    for (int k = 0; k < 3; k++) c[k] = tg_mix(c[k], stain[k], ks);
    double pa = tg_sstep(.58, .78, tg_fbm(u0, v0, 7, 7, 4, seed + 17)) * .3;
    double fl = tg_sstep(.70, .86, tg_fbm(u0, v0, 140, 140, 2, seed + 13)) * .45 * (.45 + .9 * pa);
    for (int k = 0; k < 3; k++) { c[k] = tg_mix(c[k], patch[k], pa); c[k] = tg_mix(c[k], fleck[k], fl); }
    double kh = tg_sstep(.64, .94, tg_fbm(u, v, 3, 3, 3, seed + 53)) * .3;
    for (int k = 0; k < 3; k++) c[k] = tg_mix(c[k], sheen[k], kh);
    /* the gap between planks (warped a little: no saw is straight), the lit edge beside it, the plank's end */
    double warp = (tg_fbm(u0, v0, 2, 12, 2, seed + 59) - .5) * .02;
    double f = fmod(fmod(u0 * TG_PLANKS + warp, 1) + 1, 1), d = tg_abs(f - .5) * 2;
    double gap = tg_sstep(.9, .985, d), lit = tg_sstep(.78, .9, d) * (1 - gap) * 9;
    c[0] = tg_mix(c[0], 6, gap) + lit; c[1] = tg_mix(c[1], 8, gap) + lit; c[2] = tg_mix(c[2], 6, gap) + lit;
    double e = 1 - shift, de = tg_abs(fmod(fmod(v0 - e, 1) + 1.5, 1) - .5);
    if (de < .006) { c[0] -= 52; c[1] -= 52; c[2] -= 50; } else if (de < .014) { c[0] += 10; c[1] += 9; c[2] += 6; }
    for (int k = 0; k < 3; k++) o[k] = c[k];
}

/* ---- the nails: the study's rose head, rasterized with 4 by 4 samples a texel ---- */
typedef struct { double x, y; } TgPt;
static int tg_in_poly(const TgPt *p, int n, double x, double y)
{
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((p[i].y > y) != (p[j].y > y) && x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x) in = !in;
    return in;
}
/* source-over a colour at alpha a on one texel of a float RGB image */
static void tg_over(double *px, const double *rgb, double a) { for (int k = 0; k < 3; k++) px[k] = px[k] * (1 - a) + rgb[k] * a; }
static double tg_hh(int ix, int iy, int seed) { return tg_hash((uint32_t)ix, (uint32_t)iy, (uint32_t)seed); }
/* one nail at (x, y) points, radius r points, into a W by H float image at `scale` texels a point, wrapping */
static void tg_nail(double *img, int W, int H, int scale, double x, double y, double r, int seed)
{
    const double PI = 3.14159265358979323846;
#define J(i, a) ((tg_hh((i), seed, 601) - .5) * 2 * (a))
    double rot = tg_hh(seed, 7, 601) * PI, ax = J(1, .18) * r, ay = J(2, .18) * r;
    TgPt pts[8];
    for (int i = 0; i < 8; i++) { double a = i / 8.0 * PI * 2 + rot + J(10 + i, .12), rr = r * (.84 + .3 * tg_hh(20 + i, seed, 601)); pts[i].x = cos(a) * rr; pts[i].y = sin(a) * rr; }
#undef J
    static const double shade[4][3] = { { 0x3a, 0x38, 0x33 }, { 0x28, 0x27, 0x23 }, { 0x22, 0x21, 0x20 }, { 0x14, 0x14, 0x13 } };
    static const double black[3] = { 0, 0, 0 }, bleed[3] = { 0x2a, 0x24, 0x20 }, apexc[3] = { 0x4a, 0x48, 0x42 };
    double tint = tg_hh(seed, 8, 601), haloOp = .45 + .25 * tg_hh(seed, 9, 601), bleedOp = .5 + .3 * tg_hh(seed, 3, 601);
    TgPt facet[4][4]; int fk[4];
    for (int f = 0; f < 4; f++) {
        TgPt a = pts[(f * 2) % 8], b = pts[(f * 2 + 1) % 8], c = pts[(f * 2 + 2) % 8];
        double mid = atan2((a.y + c.y) / 2 - ay, (a.x + c.x) / 2 - ax), toward = cos(mid - (-3 * PI / 4));
        fk[f] = toward > .5 ? 0 : toward > -.2 ? (tint > .5 ? 1 : 2) : 3;
        facet[f][0].x = ax; facet[f][0].y = ay; facet[f][1] = a; facet[f][2] = b; facet[f][3] = c;
    }
    double R = r * 2.1;
    for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {
        double cx = (x + ox * (double)W / scale) * scale, cy = (y + oy * (double)H / scale) * scale, Rs = R * scale;
        int x0 = (int)tg_floor(cx - Rs) - 1, x1 = (int)tg_floor(cx + Rs) + 1, y0 = (int)tg_floor(cy - Rs) - 1, y1 = (int)tg_floor(cy + Rs) + 1;
        if (x1 < 0 || y1 < 0 || x0 >= W || y0 >= H) continue;
        for (int py = y0 < 0 ? 0 : y0; py <= y1 && py < H; py++) for (int px = x0 < 0 ? 0 : x0; px <= x1 && px < W; px++) {
            double halo = 0, bl = 0, fac[4] = { 0 }, ap = 0;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                double lx = ((px + (sx + .5) / 4) - cx) / scale, ly = ((py + (sy + .5) / 4) - cy) / scale, dist = sqrt(lx * lx + ly * ly);
                double t = dist / R;            /* g-nailhalo: black .5 to .45, then to 0 */
                if (t < 1) halo += t < .45 ? .5 : .5 * (1 - (t - .45) / .55);
                t = dist / (r * 1.3);           /* g-nailbleed: .9 to .6, then to 0 */
                if (t < 1) bl += t < .6 ? .9 : .9 * (1 - (t - .6) / .4);
                for (int f = 0; f < 4; f++) if (tg_in_poly(facet[f], 4, lx, ly)) { fac[f] += 1; break; }
                if ((lx - ax) * (lx - ax) + (ly - ay) * (ly - ay) < (r * .28) * (r * .28)) ap += 1;
            }
            double *p = img + ((size_t)py * W + px) * 3;
            if (halo > 0) tg_over(p, black, halo / 16 * haloOp);
            if (bl > 0) tg_over(p, bleed, bl / 16 * bleedOp);
            for (int f = 0; f < 4; f++) if (fac[f] > 0) tg_over(p, shade[fk[f]], fac[f] / 16);
            if (ap > 0) tg_over(p, apexc, ap / 16 * .5);
        }
    }
}

/* The nail rows down the tile, in points: the study's rule from its stage seed (tableStage's seed 5,
 * fixSVG's + 7), stopping where a row would come within a short gap of the next tile's first row.
 * Writes up to `cap` and returns the count. */
static int tg_nail_rows(double *ys, int cap)
{
    const int seed = 5 + 7; int n = 0;
    double first = 22 + tg_hh(1, 9, seed) * 40;
    for (double y = first; y < TG_TILE_H - 8 && n < cap; y += 92 + tg_hh((int)y, 10, seed) * 24) {
        if (TG_TILE_H + first - y < 92) break;
        ys[n++] = y;
    }
    return n;
}

/* The whole tile, TG_TILE_W * scale by TG_TILE_H * scale, RGB, into rgb; nails when `nails`. */
void cn_texgen_planks(int scale, int nails, uint8_t *rgb)
{
    int W = TG_TILE_W * scale, H = TG_TILE_H * scale;
    float *S = tg_streaks();
    double *img = malloc(sizeof(double) * (size_t)W * H * 3);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) tg_plank_texel(S, (x + .5) / W, (y + .5) / H, scale, img + ((size_t)y * W + x) * 3);
    free(S);
    if (nails) {
        /* the canvas's own rounding first: the tile is an 8-bit image the nails' SVG is laid over */
        for (size_t i = 0; i < (size_t)W * H * 3; i++) img[i] = tg_store(img[i]);
        const int seed = 5 + 7; double ys[16]; int rows = tg_nail_rows(ys, 16);
        for (int ri = 0; ri < rows; ri++) {
            double y = ys[ri];
            for (int i = 0; i < TG_PLANKS; i++) {
                double xs[2] = { i * TG_PLANK_W + 7.0, (i + 1) * TG_PLANK_W - 7.0 };
                for (int k = 0; k < 2; k++) {
                    double x = xs[k];
                    double nx = x + (tg_hh(i, (int)y, seed) - .5) * 2.5, ny = y + (tg_hh((int)y, (int)(i + x), seed) - .5) * 5;
                    tg_nail(img, W, H, scale, nx, ny, 2.9, (int)(i * 97 + y + x));
                }
            }
        }
    }
    for (size_t i = 0; i < (size_t)W * H * 3; i++) rgb[i] = tg_store(img[i]);
    free(img);
}

/* ---- TrueType: the few tables a digit needs ---------------------------------------------- */
typedef struct { const uint8_t *b; size_t n; size_t glyf, loca, head, hhea, hmtx, cmap, os2, maxp; } Font;
static unsigned be16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }
static int bes16(const uint8_t *p) { return (int16_t)be16(p); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int font_open(Font *f, const uint8_t *b, size_t n)
{
    memset(f, 0, sizeof *f); f->b = b; f->n = n;
    if (n < 12) return 0;
    unsigned nt = be16(b + 4);
    for (unsigned i = 0; i < nt && 12 + 16 * (size_t)(i + 1) <= n; i++) {
        const uint8_t *r = b + 12 + 16 * i; uint32_t off = be32(r + 8);
        if (off >= n) return 0;
        if (!memcmp(r, "glyf", 4)) f->glyf = off; else if (!memcmp(r, "loca", 4)) f->loca = off;
        else if (!memcmp(r, "head", 4)) f->head = off; else if (!memcmp(r, "hhea", 4)) f->hhea = off;
        else if (!memcmp(r, "hmtx", 4)) f->hmtx = off; else if (!memcmp(r, "cmap", 4)) f->cmap = off;
        else if (!memcmp(r, "OS/2", 4)) f->os2 = off; else if (!memcmp(r, "maxp", 4)) f->maxp = off;
    }
    return f->glyf && f->loca && f->head && f->hhea && f->hmtx && f->cmap && f->os2 && f->maxp;
}
/* the glyph id of a character through a format 4 subtable (Windows Unicode BMP, or Unicode) */
static int font_gid(const Font *f, unsigned ch)
{
    const uint8_t *c = f->b + f->cmap; unsigned n = be16(c + 2);
    for (unsigned i = 0; i < n; i++) {
        unsigned pl = be16(c + 4 + 8 * i), en = be16(c + 6 + 8 * i); const uint8_t *t = c + be32(c + 8 + 8 * i);
        if (!((pl == 3 && en == 1) || pl == 0) || be16(t) != 4) continue;
        unsigned segs = be16(t + 6) / 2;
        const uint8_t *ends = t + 14, *starts = ends + 2 * segs + 2, *deltas = starts + 2 * segs, *ranges = deltas + 2 * segs;
        for (unsigned s = 0; s < segs; s++) {
            if (be16(ends + 2 * s) < ch) continue;
            if (be16(starts + 2 * s) > ch) return 0;
            unsigned ro = be16(ranges + 2 * s);
            if (!ro) return (int)((ch + be16(deltas + 2 * s)) & 0xFFFF);
            unsigned g = be16(ranges + 2 * s + ro + 2 * (ch - be16(starts + 2 * s)));
            return g ? (int)((g + be16(deltas + 2 * s)) & 0xFFFF) : 0;
        }
    }
    return 0;
}
static unsigned font_advance(const Font *f, int gid)
{
    unsigned nh = be16(f->b + f->hhea + 34);
    return be16(f->b + f->hmtx + 4 * ((unsigned)gid < nh ? (unsigned)gid : nh - 1));
}
/* the glyph's outline as line segments (each quadratic cut in 16), in font units; returns the count or -1 */
typedef struct { double x0, y0, x1, y1; } Edge;
static int font_outline(const Font *f, int gid, Edge *e, int cap, int *bbox)
{
    int long_loca = bes16(f->b + f->head + 50);
    uint32_t o0 = long_loca ? be32(f->b + f->loca + 4 * gid) : 2u * be16(f->b + f->loca + 2 * gid);
    const uint8_t *g = f->b + f->glyf + o0;
    int nc = bes16(g);
    if (nc <= 0) return -1;                    /* empty or composite: no digit is either */
    for (int i = 0; i < 4; i++) bbox[i] = bes16(g + 2 + 2 * i);
    const uint8_t *ends = g + 10; int np = (int)be16(ends + 2 * (nc - 1)) + 1;
    const uint8_t *p = ends + 2 * nc; p += 2 + be16(p);
    uint8_t *fl = malloc((size_t)np); int *xs = malloc(sizeof(int) * (size_t)np), *ys = malloc(sizeof(int) * (size_t)np);
    for (int i = 0; i < np;) { uint8_t c = *p++; fl[i++] = c; if (c & 8) { int r = *p++; while (r-- && i < np) fl[i++] = c; } }
    int v = 0;
    for (int i = 0; i < np; i++) { if (fl[i] & 2) { int d = *p++; v += fl[i] & 16 ? d : -d; } else if (!(fl[i] & 16)) { v += bes16(p); p += 2; } xs[i] = v; }
    v = 0;
    for (int i = 0; i < np; i++) { if (fl[i] & 4) { int d = *p++; v += fl[i] & 32 ? d : -d; } else if (!(fl[i] & 32)) { v += bes16(p); p += 2; } ys[i] = v; }
    int ne = 0, s = 0;
    double *qx = malloc(sizeof(double) * (size_t)(np + 1)), *qy = malloc(sizeof(double) * (size_t)(np + 1)); uint8_t *qo = malloc((size_t)np + 1);
#define EMIT(ax, ay, bx, by) do { if (ne < cap) { e[ne].x0 = (ax); e[ne].y0 = (ay); e[ne].x1 = (bx); e[ne].y1 = (by); } ne++; } while (0)
#define QUAD(ax, ay, cx_, cy_, bx, by) do { double px_ = (ax), py_ = (ay); \
        for (int t_ = 1; t_ <= 16; t_++) { double u_ = t_ / 16.0, a_ = (1 - u_) * (1 - u_), b_ = 2 * u_ * (1 - u_), c_ = u_ * u_; \
            double nx_ = a_ * (ax) + b_ * (cx_) + c_ * (bx), ny_ = a_ * (ay) + b_ * (cy_) + c_ * (by); EMIT(px_, py_, nx_, ny_); px_ = nx_; py_ = ny_; } } while (0)
    for (int c = 0; c < nc; c++) {
        int en = (int)be16(ends + 2 * c), n = en - s + 1, m = 0;
        /* start on an on-curve point (the first, else the last, else the midpoint of the two), and end the sequence on it */
        double sx, sy;
        if (fl[s] & 1) { sx = xs[s]; sy = ys[s]; for (int i = 1; i < n; i++) { qx[m] = xs[s + i]; qy[m] = ys[s + i]; qo[m++] = fl[s + i] & 1; } }
        else if (fl[en] & 1) { sx = xs[en]; sy = ys[en]; for (int i = 0; i < n - 1; i++) { qx[m] = xs[s + i]; qy[m] = ys[s + i]; qo[m++] = fl[s + i] & 1; } }
        else { sx = (xs[s] + xs[en]) / 2.0; sy = (ys[s] + ys[en]) / 2.0; for (int i = 0; i < n; i++) { qx[m] = xs[s + i]; qy[m] = ys[s + i]; qo[m++] = fl[s + i] & 1; } }
        qx[m] = sx; qy[m] = sy; qo[m++] = 1;
        double cx = sx, cy = sy, kx = 0, ky = 0; int ctrl = 0;
        for (int i = 0; i < m; i++) {
            if (qo[i]) {
                if (ctrl) QUAD(cx, cy, kx, ky, qx[i], qy[i]); else EMIT(cx, cy, qx[i], qy[i]);
                cx = qx[i]; cy = qy[i]; ctrl = 0;
            } else {
                if (ctrl) { double mx = (kx + qx[i]) / 2, my = (ky + qy[i]) / 2; QUAD(cx, cy, kx, ky, mx, my); cx = mx; cy = my; }
                kx = qx[i]; ky = qy[i]; ctrl = 1;
            }
        }
        s = en + 1;
    }
#undef QUAD
#undef EMIT
    free(qx); free(qy); free(qo);
    free(fl); free(xs); free(ys);
    return ne <= cap ? ne : -1;
}

/* nonzero coverage of the edges (pixel units) into a w by h byte mask: 16 sub-scanlines, exact spans */
static int cmp_x(const void *a, const void *b) { double d = ((const double *)a)[0] - ((const double *)b)[0]; return d < 0 ? -1 : d > 0; }
static void raster(const Edge *e, int ne, int w, int h, uint8_t *mask)
{
    enum { SS = 16 };
    double *acc = calloc((size_t)w, sizeof(double)), (*xs)[2] = malloc(sizeof(double[2]) * (size_t)(ne + 1));
    for (int py = 0; py < h; py++) {
        memset(acc, 0, sizeof(double) * (size_t)w);
        for (int k = 0; k < SS; k++) {
            double sy = py + (k + .5) / SS; int n = 0;
            for (int i = 0; i < ne; i++) {
                double y0 = e[i].y0, y1 = e[i].y1;
                if (y0 == y1 || sy < (y0 < y1 ? y0 : y1) || sy >= (y0 < y1 ? y1 : y0)) continue;
                xs[n][0] = e[i].x0 + (sy - y0) * (e[i].x1 - e[i].x0) / (y1 - y0); xs[n][1] = y1 > y0 ? 1 : -1; n++;
            }
            qsort(xs, (size_t)n, sizeof xs[0], cmp_x);
            int wind = 0;
            for (int i = 0; i + 1 < n; i++) {
                wind += (int)xs[i][1];
                if (!wind) continue;
                double a = xs[i][0], b = xs[i + 1][0];
                if (a < 0) a = 0;
                if (b > w) b = w;
                for (int ix = (int)a; ix < w && ix < b; ix++) { double l = a > ix ? a : ix, r = b < ix + 1 ? b : ix + 1; if (r > l) acc[ix] += (r - l) / SS; }
            }
        }
        for (int x = 0; x < w; x++) { double c = acc[x] > 1 ? 1 : acc[x]; mask[py * w + x] = (uint8_t)(int)(c * 255 + .5); }
    }
    free(acc); free(xs);
}

typedef struct { int w, h, ox, oy; uint8_t *px; } Glyph;
/* the digit at px as the canvas places it: the anchor at (0, 0), centred, middle baseline */
static int bake_glyph(const Font *f, int digit, int px, Glyph *out)
{
    int gid = font_gid(f, (unsigned)('0' + digit)), bbox[4];
    if (gid <= 0) return 0;
    static Edge e[8192];
    int ne = font_outline(f, gid, e, 8192, bbox);
    if (ne < 0) return 0;
    double upem = be16(f->b + f->head + 18), s = px / upem;
    double ta = bes16(f->b + f->os2 + 68), td = bes16(f->b + f->os2 + 70);
    double mid = ta * upem / (ta - td) - upem / 2;            /* the em box's middle above the baseline, font units */
    double penx = -(double)font_advance(f, gid) * s / 2, base = mid * s;
    int ox = (int)tg_floor(penx + bbox[0] * s) - 1, oy = (int)tg_floor(base - bbox[3] * s) - 1;
    int x1 = (int)tg_floor(penx + bbox[2] * s) + 2, y1 = (int)tg_floor(base - bbox[1] * s) + 2;
    for (int i = 0; i < ne; i++) {
        e[i].x0 = penx + e[i].x0 * s - ox; e[i].x1 = penx + e[i].x1 * s - ox;
        e[i].y0 = base - e[i].y0 * s - oy; e[i].y1 = base - e[i].y1 * s - oy;
    }
    out->w = x1 - ox; out->h = y1 - oy; out->ox = ox; out->oy = oy;
    out->px = malloc((size_t)out->w * out->h);
    raster(e, ne, out->w, out->h, out->px);
    return 1;
}

/* ---- the pack ------------------------------------------------------------------------------ */
static void put16(uint8_t *b, unsigned v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *fp = fopen(path, "rb"); if (!fp) return 0;
    fseek(fp, 0, SEEK_END); long l = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *b = l > 0 ? malloc((size_t)l) : 0;
    if (b && fread(b, 1, (size_t)l, fp) != (size_t)l) { free(b); b = 0; }
    fclose(fp); *n = b ? (size_t)l : 0; return b;
}

/* the whole pack in memory (malloc'd); 0 when the font cannot be read */
uint8_t *cn_texgen_pack(const char *font_path, uint32_t *len)
{
    size_t fn; uint8_t *fb = read_file(font_path, &fn); Font f;
    if (!fb || !font_open(&f, fb, fn)) { fprintf(stderr, "cn_texgen: cannot read the font %s\n", font_path); free(fb); return 0; }
    static uint8_t verd[CN_TEX_VERD * CN_TEX_VERD * 3], bone[CN_TEX_BONE * CN_TEX_BONE * 3];
    tg_bake(CN_TEX_VERD, CN_TEX_VERD, tg_verd, verd);
    tg_bake(CN_TEX_BONE, CN_TEX_BONE, tg_bone, bone);
    Glyph gl[CN_TEX_GLYPH_SIZES][CN_TEX_DIGITS];
    static const int SIZES[CN_TEX_GLYPH_SIZES] = { CN_TEX_NUMERAL_SMALL, CN_TEX_NUMERAL_LARGE };
    for (int s = 0; s < CN_TEX_GLYPH_SIZES; s++) for (int d = 0; d < CN_TEX_DIGITS; d++)
        if (!bake_glyph(&f, d, SIZES[s], &gl[s][d])) { fprintf(stderr, "cn_texgen: no glyph for %d\n", d); free(fb); return 0; }
    free(fb);
    /* sizes: the header, the records, then each block 4-aligned in record order */
    uint32_t off = CN_TEX_HEAD_BYTES + CN_TEX_ENTRIES * CN_TEX_ENTRY_BYTES, offs[CN_TEX_ENTRIES], lens[CN_TEX_ENTRIES];
    lens[0] = sizeof verd; lens[1] = sizeof bone;
    for (int s = 0; s < CN_TEX_GLYPH_SIZES; s++) for (int d = 0; d < CN_TEX_DIGITS; d++) lens[2 + s * CN_TEX_DIGITS + d] = (uint32_t)(gl[s][d].w * gl[s][d].h);
    for (int i = 0; i < CN_TEX_ENTRIES; i++) { off = (off + 3) & ~3u; offs[i] = off; off += lens[i]; }
    uint32_t total = (off + 3) & ~3u;
    uint8_t *b = calloc(total, 1);
    put32(b, CN_TEX_MAGIC); put16(b + 4, CN_TEX_VERSION); put16(b + 6, CN_TEX_ENTRIES); put32(b + 8, total);
    for (int i = 0; i < CN_TEX_ENTRIES; i++) {
        uint8_t *e = b + CN_TEX_HEAD_BYTES + i * CN_TEX_ENTRY_BYTES;
        if (i < 2) {
            int sz = i == 0 ? CN_TEX_VERD : CN_TEX_BONE;
            e[0] = i == 0 ? CN_TEXK_VERD : CN_TEXK_BONE; e[1] = 3; put16(e + 6, (unsigned)sz); put16(e + 8, (unsigned)sz);
            memcpy(b + offs[i], i == 0 ? verd : bone, lens[i]);
        } else {
            int s = (i - 2) / CN_TEX_DIGITS, d = (i - 2) % CN_TEX_DIGITS; Glyph *g = &gl[s][d];
            e[0] = CN_TEXK_GLYPH; e[1] = 1; e[2] = (uint8_t)d; put16(e + 4, (unsigned)SIZES[s]);
            put16(e + 6, (unsigned)g->w); put16(e + 8, (unsigned)g->h); put16(e + 10, (unsigned)(uint16_t)(int16_t)g->ox); put16(e + 12, (unsigned)(uint16_t)(int16_t)g->oy);
            memcpy(b + offs[i], g->px, lens[i]); free(g->px);
        }
        put32(e + 16, offs[i]);
    }
    put32(b + 12, cn_tex_fnv1a(b + CN_TEX_HEAD_BYTES, total - CN_TEX_HEAD_BYTES));
    *len = total;
    return b;
}

#ifndef CN_TEXGEN_NO_MAIN
/* ---- PNG, stored deflate ------------------------------------------------------------------ */
static uint32_t crc_tab[256];
static uint32_t crc(uint32_t c, const uint8_t *b, size_t n)
{
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; crc_tab[i] = v; }
    c = ~c; for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ b[i]) & 255] ^ (c >> 8); return ~c;
}
static void chunk(FILE *fp, const char *tag, const uint8_t *d, size_t n)
{
    uint8_t h[8]; h[0] = (uint8_t)(n >> 24); h[1] = (uint8_t)(n >> 16); h[2] = (uint8_t)(n >> 8); h[3] = (uint8_t)n; memcpy(h + 4, tag, 4);
    fwrite(h, 1, 8, fp); if (n) fwrite(d, 1, n, fp);
    uint32_t c = crc(crc(0, h + 4, 4), d, n); uint8_t t[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c }; fwrite(t, 1, 4, fp);
}
static int write_png(const char *path, const uint8_t *rgb, int w, int h)
{
    size_t raw = (size_t)h * (1 + (size_t)w * 3), nblk = (raw + 65534) / 65535;
    uint8_t *z = malloc(2 + raw + nblk * 5 + 4), *r = malloc(raw); size_t o = 0;
    for (int y = 0; y < h; y++) { r[y * (1 + (size_t)w * 3)] = 0; memcpy(r + y * (1 + (size_t)w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    z[o++] = 0x78; z[o++] = 1;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; i++) { a = (a + r[i]) % 65521; b = (b + a) % 65521; }
    for (size_t p = 0; p < raw; p += 65535) {
        size_t n = raw - p < 65535 ? raw - p : 65535;
        z[o++] = p + n == raw; z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, r + p, n); o += n;
    }
    uint32_t ad = b << 16 | a; z[o++] = (uint8_t)(ad >> 24); z[o++] = (uint8_t)(ad >> 16); z[o++] = (uint8_t)(ad >> 8); z[o++] = (uint8_t)ad;
    FILE *fp = fopen(path, "wb"); if (!fp) { free(z); free(r); return 0; }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 }; fwrite(sig, 1, 8, fp);
    uint8_t ih[13] = { 0 }; ih[0] = (uint8_t)(w >> 24); ih[1] = (uint8_t)(w >> 16); ih[2] = (uint8_t)(w >> 8); ih[3] = (uint8_t)w;
    ih[4] = (uint8_t)(h >> 24); ih[5] = (uint8_t)(h >> 16); ih[6] = (uint8_t)(h >> 8); ih[7] = (uint8_t)h; ih[8] = 8; ih[9] = 2;
    chunk(fp, "IHDR", ih, 13); chunk(fp, "IDAT", z, o); chunk(fp, "IEND", 0, 0);
    fclose(fp); free(z); free(r); return 1;
}

/* ---- --compare: the study's captures against the pack's derivations -------------------------- */
typedef struct { uint8_t *rgb; int w, h; } Sheet;
static void sheet_put(Sheet *s, const uint8_t *rgba, int w, int h, int X, int Y, int down)
{
    for (int y = 0; y < h / down; y++) for (int x = 0; x < w / down; x++) {
        if (X + x >= s->w || Y + y >= s->h) continue;
        for (int c = 0; c < 3; c++) {
            int sum = 0; for (int j = 0; j < down; j++) for (int i = 0; i < down; i++) sum += rgba[(((size_t)y * down + j) * w + (size_t)x * down + i) * 4 + c];
            s->rgb[((size_t)(Y + y) * s->w + X + x) * 3 + c] = (uint8_t)(sum / (down * down));
        }
    }
}
static double mad(const uint8_t *a, const uint8_t *b, size_t texels, int stride, int chans, int *maxd)
{
    double s = 0; *maxd = 0;
    for (size_t i = 0; i < texels; i++) for (int c = 0; c < chans; c++) {
        int d = (int)a[i * stride + c] - (int)b[i * stride + c]; if (d < 0) d = -d;
        s += d; if (d > *maxd) *maxd = d;
    }
    return s / ((double)texels * chans);
}
static double mad_i8(const int8_t *a, const int8_t *b, size_t n, int *maxd)
{
    double s = 0; *maxd = 0;
    for (size_t i = 0; i < n; i++) { int d = a[i] - b[i]; if (d < 0) d = -d; s += d; if (d > *maxd) *maxd = d; }
    return s / (double)n;
}

static int compare(const CnTexPack *p, const char *dir, const char *sheet_path)
{
    static const uint32_t SEEDS[3] = { 1, 7, 42 };
    const char *KINDS[7] = { "side", "inner", "floor", "crown_c3_112", "crown_c5_184", "crown_out", "die" };
    char path[1024]; size_t n; int worst_fail = 0;
    double worst = 0;
    Sheet sh = { 0, 3 * 768 + 32, 0 };
    /* the sheet: the two tiles, then seed 7's seven textures; each row reference | C | difference x4 */
    int rows_h[9] = { 256, 128, 256, 256, 256, 256, 256, 256, 128 };
    for (int i = 0; i < 9; i++) sh.h += rows_h[i] + 8;
    sh.rgb = calloc((size_t)sh.w * sh.h * 3, 1);
    for (size_t i = 0; i < (size_t)sh.w * sh.h * 3; i++) sh.rgb[i] = 40;
    int rowY = 0;
    /* the tiles: bit for bit */
    for (int t = 0; t < 2; t++) {
        int S = t ? CN_TEX_BONE : CN_TEX_VERD; const uint8_t *src = t ? p->bone : p->verd;
        snprintf(path, sizeof path, "%s/%s_%dx%d.rgba", dir, t ? "bone" : "verd", S, S);
        uint8_t *ref = read_file(path, &n); if (!ref || n != (size_t)S * S * 4) { fprintf(stderr, "missing %s\n", path); return 1; }
        uint8_t *mine = malloc((size_t)S * S * 4), *diff = malloc((size_t)S * S * 4);
        for (int i = 0; i < S * S; i++) { memcpy(mine + i * 4, src + i * 3, 3); mine[i * 4 + 3] = 255; }
        int mx; double m = mad(ref, mine, (size_t)S * S, 4, 3, &mx);
        printf("%-22s MAD %.4f  max %3d  (texels identical: %s)\n", t ? "bone tile" : "verdigris tile", m, mx, mx ? "no" : "yes");
        if (m > worst) worst = m;
        for (int i = 0; i < S * S * 4; i++) { int d = (int)ref[i] - mine[i]; d = (d < 0 ? -d : d) * 4; diff[i] = (uint8_t)(d > 255 ? 255 : d); }
        sheet_put(&sh, ref, S, S, 0, rowY, 1); sheet_put(&sh, mine, S, S, 768 + 16, rowY, 1); sheet_put(&sh, diff, S, S, 2 * (768 + 16), rowY, 1);
        rowY += rows_h[t] + 8; free(ref); free(mine); free(diff);
    }
    for (int si = 0; si < 3; si++) for (int k = 0; k < 7; k++) {
        uint32_t seed = SEEDS[si];
        CnTexWhat what = k == 0 ? CN_TEX_SIDE : k == 1 ? CN_TEX_INNER : k == 2 ? CN_TEX_FLOOR : k == 6 ? CN_TEX_DIE : CN_TEX_CROWN_T;
        int w, h; cn_tex_size(what, &w, &h);
        const char *kind = KINDS[k];
        char name[64];
        if (k >= 3 && k <= 5) snprintf(name, sizeof name, "crown_%u_%s", seed, kind + 6); else snprintf(name, sizeof name, "%s_%u", kind, seed);
        snprintf(path, sizeof path, "%s/%s_%dx%d.rgba", dir, name, w, h);
        uint8_t *ref = read_file(path, &n); if (!ref || n != (size_t)w * h * 4) { fprintf(stderr, "missing %s\n", path); return 1; }
        snprintf(path, sizeof path, "%s/%s_%dx%d.bump", dir, name, w, h);
        size_t bn; int8_t *rb = (int8_t *)read_file(path, &bn);
        uint8_t *mine = malloc((size_t)w * h * 4); int8_t *mb = malloc((size_t)w * h * 2);
        switch (k) {
        case 0: cn_tex_cup_side(p, seed, mine); cn_tex_relief(mine, w, h, CN_TEX_RELIEF_SIDE, mb); break;
        case 1: cn_tex_cup_inner(p, seed, mine); cn_tex_relief(mine, w, h, CN_TEX_RELIEF_CUP, mb); break;
        case 2: cn_tex_cup_floor(p, seed, mine); cn_tex_relief(mine, w, h, CN_TEX_RELIEF_CUP, mb); break;
        case 3: cn_tex_cup_crown(p, seed, 3, 0, 112, mine); cn_tex_relief(mine, w, h, CN_TEX_RELIEF_CUP, mb); break;
        case 4: cn_tex_cup_crown(p, seed, 5, 0, 184, mine); cn_tex_relief(mine, w, h, CN_TEX_RELIEF_CUP, mb); break;
        case 5: cn_tex_cup_crown(p, seed, 0, 1, 112, mine); cn_tex_relief(mine, w, h, CN_TEX_RELIEF_CUP, mb); break;
        default: cn_tex_die_atlas(p, seed, mine); cn_tex_die_relief(mine, seed, mb); break;
        }
        int mx, bmx = 0; double m = mad(ref, mine, (size_t)w * h, 4, 3, &mx), bm = -1;
        if (rb && bn == (size_t)w * h * 2) bm = mad_i8(rb, mb, (size_t)w * h * 2, &bmx);
        printf("%-22s MAD %.4f  max %3d   bump MAD %.4f max %3d\n", name, m, mx, bm, bmx);
        if (m > worst) worst = m;
        if (m > 1.0) worst_fail = 1;
        if (seed == 7) {
            uint8_t *diff = malloc((size_t)w * h * 4);
            for (int i = 0; i < w * h * 4; i++) { int d = (int)ref[i] - mine[i]; d = (d < 0 ? -d : d) * 4; diff[i] = (uint8_t)(d > 255 ? 255 : d); }
            int down = w > 768 ? 2 : 1;
            sheet_put(&sh, ref, w, h, 0, rowY, down); sheet_put(&sh, mine, w, h, 768 + 16, rowY, down); sheet_put(&sh, diff, w, h, 2 * (768 + 16), rowY, down);
            rowY += rows_h[2 + k] + 8; free(diff);
        }
        free(ref); free(rb); free(mine); free(mb);
    }
    printf("worst MAD %.4f (tolerance 1.0 of 255 a channel)\n", worst);
    if (sheet_path) { if (!write_png(sheet_path, sh.rgb, sh.w, sh.h)) return 1; printf("wrote %s (%dx%d)\n", sheet_path, sh.w, sh.h); }
    free(sh.rgb);
    return worst_fail;
}

/* the planks at the study's scale, no nails, against the study's canvas; a sheet: study | C | difference x4 */
static int compare_planks(const char *ref_path, const char *sheet_path)
{
    enum { W = TG_TILE_W, H = TG_TILE_H };
    size_t n; uint8_t *ref = read_file(ref_path, &n);
    if (!ref || n != (size_t)W * H * 4) { fprintf(stderr, "cn_texgen: %s is not a %dx%d RGBA capture\n", ref_path, W, H); free(ref); return 1; }
    uint8_t *mine = malloc((size_t)W * H * 3); cn_texgen_planks(1, 0, mine);
    uint8_t *m4 = malloc((size_t)W * H * 4);
    for (int i = 0; i < W * H; i++) { memcpy(m4 + i * 4, mine + i * 3, 3); m4[i * 4 + 3] = 255; }
    int mx; double m = mad(ref, m4, (size_t)W * H, 4, 3, &mx);
    size_t same = 0; for (int i = 0; i < W * H; i++) same += !memcmp(ref + i * 4, m4 + i * 4, 3);
    printf("planks %dx%d  MAD %.4f  max %d  texels identical %.2f%%\n", W, H, m, mx, 100.0 * same / (W * H));
    if (sheet_path) {
        Sheet sh = { 0, 3 * W + 32, H };
        sh.rgb = calloc((size_t)sh.w * sh.h * 3, 1);
        uint8_t *diff = malloc((size_t)W * H * 4);
        for (int i = 0; i < W * H * 4; i++) { int d = (int)ref[i] - m4[i]; d = (d < 0 ? -d : d) * 4; diff[i] = (uint8_t)(d > 255 ? 255 : d); }
        sheet_put(&sh, ref, W, H, 0, 0, 1); sheet_put(&sh, m4, W, H, W + 16, 0, 1); sheet_put(&sh, diff, W, H, 2 * (W + 16), 0, 1);
        if (!write_png(sheet_path, sh.rgb, sh.w, sh.h)) return 1;
        printf("wrote %s\n", sheet_path); free(diff); free(sh.rgb);
    }
    free(ref); free(mine); free(m4);
    return m > 1.0;
}

int main(int argc, char **argv)
{
    const char *font = 0, *pack = 0, *ref = 0, *sheet = 0, *images = 0, *planks_ref = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--font")) font = argv[i + 1]; else if (!strcmp(argv[i], "--pack")) pack = argv[i + 1];
        else if (!strcmp(argv[i], "--compare")) ref = argv[i + 1]; else if (!strcmp(argv[i], "--sheet")) sheet = argv[i + 1];
        else if (!strcmp(argv[i], "--images")) images = argv[i + 1]; else if (!strcmp(argv[i], "--compare-planks")) planks_ref = argv[i + 1];
        else { fprintf(stderr, "cn_texgen: unknown %s\n", argv[i]); return 2; }
    }
    if (planks_ref) return compare_planks(planks_ref, sheet);
    if (!font || (!pack && !ref && !images)) { fprintf(stderr, "usage: cn_texgen --font F.ttf (--pack OUT | --images DIR | --compare REFDIR [--sheet OUT.png]) | --compare-planks REF.rgba [--sheet OUT.png]\n"); return 2; }
    uint32_t len; uint8_t *b = cn_texgen_pack(font, &len);
    if (!b) return 1;
    CnTexPack p; int rc = cn_tex_pack_open(&p, b, len);
    if (rc) { fprintf(stderr, "cn_texgen: the pack does not open (%d)\n", rc); return 1; }
    if (pack) {
        FILE *fp = fopen(pack, "wb");
        if (!fp || fwrite(b, 1, len, fp) != len) { fprintf(stderr, "cn_texgen: cannot write %s\n", pack); return 1; }
        fclose(fp);
        printf("wrote %s (%u B, fnv1a %08x)\n", pack, len, cn_tex_fnv1a(b, len));
    }
    if (images) {
        char path[1024];
        enum { S = CN_TEXGEN_PLANK_SCALE };
        uint8_t *pl = malloc((size_t)TG_TILE_W * S * TG_TILE_H * S * 3);
        cn_texgen_planks(S, 1, pl);
        snprintf(path, sizeof path, "%s/cn_planks.png", images);
        if (!write_png(path, pl, TG_TILE_W * S, TG_TILE_H * S)) { fprintf(stderr, "cn_texgen: cannot write %s\n", path); return 1; }
        printf("wrote %s (%dx%d)\n", path, TG_TILE_W * S, TG_TILE_H * S);
        free(pl);
        snprintf(path, sizeof path, "%s/cn_verd.png", images);
        if (!write_png(path, p.verd, CN_TEX_VERD, CN_TEX_VERD)) return 1;
        snprintf(path, sizeof path, "%s/cn_bone.png", images);
        if (!write_png(path, p.bone, CN_TEX_BONE, CN_TEX_BONE)) return 1;
        printf("wrote %s/cn_verd.png and cn_bone.png\n", images);
    }
    if (ref) rc = compare(&p, ref, sheet);
    free(b);
    return rc;
}
#endif
