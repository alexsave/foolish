// fern.c - the card-back fern for the printed promo cards, a faithful port of the app's
// generator so the print is the same fern the game draws, not a picture of it.
//
// The iterated function system, constants, probabilities, colours, -67 degree rotation,
// point mirror and colour propagation are foolish/src/utils/fernFractal.tsx
// (DEFAULT_FERN_PARAMS, generateFernPoints) verbatim. Two plotters:
//
//   web  - fernFractal.tsx's 2D-canvas renderer: Math.random is the e2e test's seeded LCG,
//          every dot overwrites, transparent field. `make_cards.py --verify-fern` runs the
//          shipped title fern (1400 x 1960, 12M points, 2 px dots) through it twice, the way
//          the no-WebGL path does, and holds the pixels to the digest
//          foolish/e2e/fern_texture.test.ts recorded from the real code.
//   ios  - shared/swift/Textures/FernCardBack.swift, the owner's chosen look (round 7:
//          1 px dots, density 14 per pixel, alpha 0.20 blended over black), its fixed-seed
//          xorshift. This is what bakes foolish/ios/FoolishKit/Resources/fern-back.jpg.
//
// usage:
//   fern web W H ITERS DOT SCALE SEED PASSES OUT.rgba
//   fern ios W H ITERS DOT ALPHA SCALE CX CY OUT.rgba
// Writes W*H*4 bytes of RGBA. Prints the number of random draws taken (web) to stdout.
//
// Build: cc -O2 -ffp-contract=off -o fern fern.c -lm   (no FMA: the JS does not fuse)

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double a, b, c, d, e, f; } Affine;
static const Affine T[4] = {
    {0.02317, -0.0013, 0, 0.21422, 0, 0},       // f1 stem
    {0.789, 0.1533, -0.1877, 0.8734, 0.0617, 2}, // f2 main
    {-0.4556, -0.2832, -0.3847, 0.3305, 0, 1},   // f3 left
    {0.3, 0.2, -0.2, 0.2, 0, 0},                 // f4 right
};
typedef struct { double cx, cy, r; } Circle;
static const Circle C1 = {2.803, 0.5296, 0.4817};    // #ff0000
static const Circle C2 = {-4.5784, 1.4463, 0.2894};  // #bd7800
static const double W6[6] = {0.01, 0.8, 0.08, 0.08, 0.02, 0.01};
static const uint8_t PAL[3][3] = {{0xff, 0xd7, 0x00}, {0xff, 0x00, 0x00}, {0xbd, 0x78, 0x00}};

// ---- random streams -----------------------------------------------------------------
static int use_lcg;
static uint32_t lcg;
static uint64_t xs;
static uint64_t draws;
static double rnd(void) {
    draws++;
    if (use_lcg) { lcg = lcg * 1664525u + 1013904223u; return lcg / 4294967296.0; }
    xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17;
    return (double)(xs >> 11) * (1.0 / 9007199254740992.0);
}

static double cum[6];
static void cumulative(void) {
    double sum = 0, acc = 0;
    for (int i = 0; i < 6; i++) sum += W6[i];
    for (int i = 0; i < 6; i++) { acc += W6[i] / sum; cum[i] = acc; }
}
static int pick_web(void) {  // fernFractal.tsx pickTransformIndex
    double r = rnd();
    for (int i = 0; i < 6; i++) if (r < cum[i]) return i;
    return 5;
}
static int pick_ios(void) {  // FernCardBack.swift pick
    double r = rnd();
    int i = 0;
    while (i < 5 && r >= cum[i]) i++;
    return i;
}
static void in_circle(const Circle *c, double *x, double *y) {
    double t = rnd();
    double ang = rnd() * M_PI * 2;
    double rad = sqrt(t) * c->r;
    *x = c->cx + rad * cos(ang);
    *y = c->cy + rad * sin(ang);
}
static void affine(int i, double *x, double *y) {
    double nx = T[i].a * *x + T[i].b * *y + T[i].e;
    double ny = T[i].c * *x + T[i].d * *y + T[i].f;
    *x = nx; *y = ny;
}

// ---- web: generateFernPoints + generateFernPatternFallback ---------------------------
static void web(uint8_t *px, int w, int h, long iters, int dot, double scale, int plot) {
    double rot = -67 * M_PI / 180, cr = cos(rot), sr = sin(rot);
    double cx = w / 2.0, cy = h / 2.0;
    double x = 0, y = 0;
    int prop = 0;
    for (int i = 0; i < 20; i++) {
        int idx = pick_web();
        if (idx < 4) affine(idx, &x, &y);
        else in_circle(idx == 4 ? &C1 : &C2, &x, &y);
    }
    for (long i = 0; i < iters; i++) {
        int idx = pick_web();
        if (idx < 4) affine(idx, &x, &y);
        else in_circle(idx == 4 ? &C1 : &C2, &x, &y);
        double dx = x, dy = y;
        if (idx == 4 || idx == 5) { in_circle(idx == 4 ? &C1 : &C2, &dx, &dy); prop = idx == 4 ? 1 : 2; }
        else if (idx == 0) prop = 0;
        if (rnd() < 0.5) { dx = -dx; dy = -dy; }
        double rx = dx * cr - dy * sr, ry = dx * sr + dy * cr;
        double fx = cx + rx * scale + 0, fy = cy - (ry * scale - 0);
        if (!plot) continue;
        int X = (int)fx, Y = (int)fy;
        for (int a = 0; a < dot; a++)
            for (int b = 0; b < dot; b++) {
                int xx = X + b, yy = Y + a;
                if (xx >= 0 && xx < w && yy >= 0 && yy < h) {
                    uint8_t *o = px + ((size_t)yy * w + xx) * 4;
                    o[0] = PAL[prop][0]; o[1] = PAL[prop][1]; o[2] = PAL[prop][2]; o[3] = 255;
                }
            }
    }
}

// ---- ios: FernCardBack.renderCGImage ----------------------------------------------------
static void blend(uint8_t *px, int w, int h, int dot, double alpha, double fx, double fy, const uint8_t *c) {
    int X = (int)fx, Y = (int)fy;
    double ia = 1 - alpha, r = c[0] * alpha, g = c[1] * alpha, b = c[2] * alpha;
    for (int yy = Y; yy < Y + dot; yy++) {
        if (yy < 0 || yy >= h) continue;
        for (int xx = X; xx < X + dot; xx++) {
            if (xx < 0 || xx >= w) continue;
            uint8_t *o = px + ((size_t)yy * w + xx) * 4;
            double v0 = r + o[0] * ia, v1 = g + o[1] * ia, v2 = b + o[2] * ia;
            o[0] = (uint8_t)(v0 > 255 ? 255 : v0);
            o[1] = (uint8_t)(v1 > 255 ? 255 : v1);
            o[2] = (uint8_t)(v2 > 255 ? 255 : v2);
        }
    }
}
static void ios(uint8_t *px, int w, int h, long iters, int dot, double alpha, double sc, double cx, double cy) {
    for (size_t i = 0; i < (size_t)w * h; i++) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 0; px[i * 4 + 3] = 255; }
    double rot = -67.0 * M_PI / 180, cr = cos(rot), sr = sin(rot);
    double x = 0, y = 0;
    int prop = 0;
#define PLACE(dx0, dy0, FX, FY) do { double dx_ = (dx0), dy_ = (dy0); \
        if (rnd() < 0.5) { dx_ = -dx_; dy_ = -dy_; } \
        double rx_ = dx_ * cr - dy_ * sr, ry_ = dx_ * sr + dy_ * cr; \
        FX = cx + rx_ * sc; FY = cy - (ry_ * sc); } while (0)
    for (int i = 0; i < 20; i++) {
        int idx = pick_ios();
        if (idx == 4) in_circle(&C1, &x, &y);
        else if (idx == 5) in_circle(&C2, &x, &y);
        else affine(idx, &x, &y);
    }
    for (long i = 0; i < iters; i++) {
        int idx = pick_ios();
        double fx, fy, dx, dy;
        if (idx == 4 || idx == 5) {
            const Circle *c = idx == 4 ? &C1 : &C2;
            in_circle(c, &x, &y);
            in_circle(c, &dx, &dy);
            PLACE(dx, dy, fx, fy);
            blend(px, w, h, dot, alpha, fx, fy, PAL[idx == 4 ? 1 : 2]);
            prop = idx == 4 ? 1 : 2;
            continue;
        }
        affine(idx, &x, &y);
        if (idx == 0) prop = 0;
        PLACE(x, y, fx, fy);
        blend(px, w, h, dot, alpha, fx, fy, PAL[prop]);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: see the header of fern.c\n"); return 2; }
    cumulative();
    int w = atoi(argv[2]), h = atoi(argv[3]);
    long iters = atol(argv[4]);
    int dot = atoi(argv[5]);
    uint8_t *px = calloc((size_t)w * h, 4);
    const char *out;
    if (!strcmp(argv[1], "web") && argc == 10) {
        use_lcg = 1;
        double scale = atof(argv[6]);
        lcg = (uint32_t)strtoul(argv[7], 0, 10);
        int passes = atoi(argv[8]);
        for (int p = 0; p < passes; p++) web(px, w, h, iters, dot, scale, p == passes - 1);
        out = argv[9];
        printf("%llu\n", (unsigned long long)draws);
    } else if (!strcmp(argv[1], "ios") && argc == 11) {
        use_lcg = 0;
        xs = 0x9E3779B97F4A7C15ull;
        ios(px, w, h, iters, dot, atof(argv[6]), atof(argv[7]), atof(argv[8]), atof(argv[9]));
        out = argv[10];
    } else { fprintf(stderr, "usage: see the header of fern.c\n"); return 2; }
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(px, 1, (size_t)w * h * 4, f) != (size_t)w * h * 4) { perror(out); return 1; }
    fclose(f);
    free(px);
    return 0;
}
