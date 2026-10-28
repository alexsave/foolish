/* textures.c - the wool, the wood and the fern (textures.h). Every expression keeps the TS's
 * order of operations, and JS's conversions are spelled out: a store into a Uint8ClampedArray
 * (clamp8), the `| 0` and `^` of int32 (toint32), a store into a Float32Array (a float cast). */
#include "textures.h"

/* sin and cos rounded correctly as Chrome's are (crtrig.c); tan is musl's (jsmath.c), which the
 * wool's one use of it (a threshold) has never told apart from Chrome's. */
double cr_sin(double), cr_cos(double), js_tan(double), js_fmod(double, double);
#define js_sin cr_sin
#define js_cos cr_cos

#define floor_ __builtin_floor
#define PI 3.141592653589793

/* ToUint8Clamp: NaN and below 0 are 0, above 255 is 255, and a half rounds to even. */
static uint8_t clamp8(double v) {
  if (!(v > 0)) return 0;
  if (v >= 255) return 255;
  double f = floor_(v);
  if (f + 0.5 < v) return (uint8_t)(f + 1);
  if (v < f + 0.5) return (uint8_t)f;
  return (uint8_t)((int32_t)f & 1 ? f + 1 : f);
}

/* ToInt32: NaN and the infinities are 0, then the integer part modulo 2^32. */
static int32_t toint32(double v) {
  if (!(v - v == 0)) return 0;
  double t = __builtin_trunc(v);
  if (t >= -2147483648.0 && t <= 2147483647.0) return (int32_t)t;
  double m = t - 4294967296.0 * floor_(t / 4294967296.0);
  return (int32_t)(uint32_t)m;
}

static double fmax_(double a, double b) { return a > b ? a : b; } /* Math.max, neither is NaN here */
static double fabs_(double a) { return a < 0 ? -a : a; }

double tex_random(uint32_t *state) { /* mulberry32 */
  uint32_t a = *state += 0x6D2B79F5u;
  uint32_t t = (a ^ a >> 15) * (1u | a);
  t = (t + (t ^ t >> 7) * (61u | t)) ^ t;
  return (double)(t ^ t >> 14) / 4294967296.0;
}

/* ---- the wool (woolPixels.ts paintWool) ---------------------------------- */

void wool_begin(Wool *t, uint8_t *px, int32_t w, int32_t h, double offx, double offy) {
  *t = (Wool){px, w, h, offx, offy, 0, 0, 0, 0, 0, 1};
  for (int32_t i = 0; i < w * h * 4; i += 4) { px[i] = 113; px[i + 1] = 65; px[i + 2] = 27; px[i + 3] = 255; }
  t->max = (int32_t)floor_(((double)w * h / (1920 * 1080)) * 2000000);
  t->switch_at = (int32_t)floor_(t->max * 0.4);
}

static double z_value(const Wool *t) {
  double cr = js_cos(t->r) * 1000;
  return cr - floor_(cr);
}

static void write_pixel(Wool *t, double x, double y, int32_t red, int32_t green, int32_t blue, double size) {
  double xi = floor_(x), yi = floor_(y);
  int32_t n = (int32_t)__builtin_ceil(size);
  double fx = x - xi, fy = y - yi;
  for (int32_t dy = 0; dy < n; dy++)
    for (int32_t dx = 0; dx < n; dx++) {
      double px = xi + dx, py = yi + dy;
      if (px >= 0 && px < t->w && py >= 0 && py < t->h) {
        int32_t idx = ((int32_t)py * t->w + (int32_t)px) << 2;
        double dist_x = fabs_(dx - fx), dist_y = fabs_(dy - fy);
        double edge = fmax_(0.1, 1 - (dist_x + dist_y) / 4);
        double alpha = edge * 0.99, inv = 1 - alpha;
        uint8_t *p = t->px + idx;
        p[0] = clamp8(p[0] * inv + red * alpha);
        p[1] = clamp8(p[1] * inv + green * alpha);
        p[2] = clamp8(p[2] * inv + blue * alpha);
        p[3] = 255;
      }
    }
}

int wool_run(Wool *t, int32_t steps) {
  int32_t w = t->w, h = t->h;
  for (int32_t end = t->i + steps; t->i < t->max && t->i < end; t->i++) {
    int32_t i = t->i;
    if (i == t->switch_at) { t->horizontal = 0; t->r = 0; }
    if (t->horizontal) { /* horizontal fibres */
      if (i % w == 0) { t->u = z_value(t) * 500 + 100; t->r += 5; }
      double phase = js_sin(i / t->u);
      double dx = js_sin((double)i - 1) + phase * 6;
      double z = z_value(t);
      int32_t red = js_tan((double)(toint32(floor_((t->r + t->offx) / 80 + z / 4)) ^ toint32(floor_((i % h + t->offy) / 80)))) > 0.3 ? 100 : 0;
      double x = t->r + dx, y = i % h;
      if (x >= 0 && x < w && y >= 0 && y < h)
        write_pixel(t, x, y, toint32(209 + 46 * phase + red), toint32(208 + 45 * phase - red),
                    toint32(183 + 53 * phase - red / 2.0), 2);
    } else { /* vertical fibres */
      if (i % h == 0) { t->u = z_value(t) * 500 + 100; t->r += 3; }
      double phase = js_sin(i / t->u);
      double z = z_value(t);
      int32_t red = js_tan((double)(toint32(floor_((t->r + t->offy) / 80 + z / 4)) ^ toint32(floor_((i % w + t->offx) / 80)))) > 0.3 ? 100 : 0;
      double dx = 4 * js_sin((double)i - 1) + js_sin(i / t->u) * 4;
      double x = i % w, y = t->r + dx;
      if (x >= 0 && x < w && y >= 0 && y < h)
        write_pixel(t, x, y, toint32(189 + 46 * phase + red), toint32(188 + 45 * phase - red),
                    toint32(163 + 53 * phase - red / 2.0), 1.4 * (phase + 1.7));
    }
  }
  return t->i >= t->max;
}

/* ---- the wood (WoodTexture.tsx generateWoodPixelData) -------------------- */

void wood_begin(Wood *t, float *col, float *ifac, uint8_t *px, int32_t w, int32_t h, double offx, double offy) {
  *t = (Wood){col, ifac, px, w, h, 0, offx, offy};
  for (int32_t i = 0; i < w * h; i++) {
    col[i * 3] = (float)(70 / 255.0);
    col[i * 3 + 1] = (float)(14 / 255.0);
    col[i * 3 + 2] = (float)(9 / 255.0);
  }
  for (int32_t i = 0; i < h; i++) ifac[i] = (float)((i + offy) * 0.001);
}

static void wood_column(Wood *t, double T) {
  int32_t w = t->w;
  double xf = js_fmod((T + t->offx / 200) * 200, w);
  int32_t xpos = toint32(xf), xend = xpos + 40 < w ? xpos + 40 : w, xcenter = xpos + 20;
  for (int32_t I = t->h - 1; I >= 0; I--) {
    double ifac = t->ifac[I];
    double b = (T + t->offx * 0.01) / 24;
    for (int32_t k = 24; k >= 0; k--) {
      double half = (b * b) * 0.5;
      b = js_cos(ifac + js_cos(half) * b + 4 + t->offy * 0.001) * b - 2.8;
      if (b > 0) {
        double red = (b * 120) / 255, green = (b * b * 14) / 255, blue = 9 / 255.0;
        for (int32_t x = xpos; x < xend; x++) {
          int32_t idx = (I * w + x) * 3;
          if (idx < 0) continue; /* a typed array ignores a store below 0 */
          double dist = fabs_((double)(x - xcenter)) / (40 / 2);
          double edge = fmax_(0.3, 1 - dist * 0.5);
          double ea = 0.1 * edge, iea = 1 - ea;
          float *c = t->col + idx;
          c[0] = (float)(red * ea + c[0] * iea);
          c[1] = (float)(green * ea + c[1] * iea);
          c[2] = (float)(blue * ea + c[2] * iea);
        }
      }
    }
  }
}

int wood_run(Wood *t, int32_t columns) {
  for (int32_t end = t->column + columns; t->column < 576 && t->column < end; t->column++) wood_column(t, t->column / 60.0);
  if (t->column < 576) return 0;
  if (t->column == 576) { /* the blit: each float colour to a byte, as the GL draw writes it */
    for (int32_t i = 0; i < t->w * t->h; i++) {
      for (int k = 0; k < 3; k++) {
        double v = t->col[i * 3 + k] * 255.0 + 0.5;
        t->px[i * 4 + k] = v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)v;
      }
      t->px[i * 4 + 3] = 255;
    }
    t->column++;
  }
  return 1;
}

/* ---- the fern (fernFractal.tsx generateFernPoints, createCardParams) ----- */

static const double FT[4][6] = { /* a, b, c, d, e, f */
  {0.02317, -0.0013, 0, 0.21422, 0, 0},
  {0.789, 0.1533, -0.1877, 0.8734, 0.0617, 2},
  {-0.4556, -0.2832, -0.3847, 0.3305, 0, 1},
  {0.3, 0.2, -0.2, 0.2, 0, 0},
};
static const double CIRCLE[2][3] = {{2.803, 0.5296, 0.4817}, {-4.5784, 1.4463, 0.2894}};
static const uint8_t PALETTE[3][3] = {{255, 215, 0}, {255, 0, 0}, {189, 120, 0}};

void fern_begin(Fern *t, uint8_t *px, uint8_t *smp, int32_t w, int32_t h, int32_t iterations, uint32_t seed) {
  __builtin_memset(t, 0, sizeof *t);
  t->px = px; t->smp = smp; t->w = w; t->h = h; t->iterations = iterations; t->rng = seed;
  __builtin_memset(px, 0, (uint32_t)(w * h * 4));
  if (smp) __builtin_memset(smp, 0, (uint32_t)(w * h * 4));
  const double p[6] = {0.01, 0.8, 0.08, 0.08, 0.02, 0.01};
  double sum = 0, acc = 0;
  for (int k = 0; k < 6; k++) sum = sum + p[k]; /* values.reduce((a, b) => a + b, 0) */
  for (int k = 0; k < 6; k++) { acc += p[k] / sum; t->cum[k] = acc; }
  t->scale = 14 * ((double)(w < h ? w : h) / 70);
  double rot = -67 * PI / 180;
  t->cos_rot = js_cos(rot);
  t->sin_rot = js_sin(rot);
}

static int pick(Fern *t) {
  double r = tex_random(&t->rng);
  for (int k = 0; k < 6; k++) if (r < t->cum[k]) return k;
  return 5;
}

static void circle_point(Fern *t, int c, double *x, double *y) {
  double u = tex_random(&t->rng);
  double angle = tex_random(&t->rng) * PI * 2;
  double radius = __builtin_sqrt(u) * CIRCLE[c][2];
  *x = CIRCLE[c][0] + radius * js_cos(angle);
  *y = CIRCLE[c][1] + radius * js_sin(angle);
}

static void step(Fern *t, int k) {
  if (k < 4) {
    const double *f = FT[k];
    double nx = f[0] * t->x + f[1] * t->y + f[4], ny = f[2] * t->x + f[3] * t->y + f[5];
    t->x = nx; t->y = ny;
  } else circle_point(t, k - 4, &t->x, &t->y);
}

/* Metal's standard 4-sample positions within a pixel, y down. */
static const double SAMPLE[4][2] = {{0.375, 0.875}, {0.875, 0.625}, {0.125, 0.375}, {0.625, 0.125}};

/* A 1-pixel point at (x, y): every sample inside its square takes the colour, the later point winning. */
static void cover(Fern *t, double x, double y) {
  for (int32_t j = (int32_t)floor_(y - 0.5); j <= (int32_t)floor_(y + 0.5); j++)
    for (int32_t k = (int32_t)floor_(x - 0.5); k <= (int32_t)floor_(x + 0.5); k++) {
      if (j < 0 || j >= t->h || k < 0 || k >= t->w) continue;
      for (int s = 0; s < 4; s++) {
        double sx = k + SAMPLE[s][0], sy = j + SAMPLE[s][1];
        if (sx >= x - 0.5 && sx < x + 0.5 && sy >= y - 0.5 && sy < y + 0.5) t->smp[(j * t->w + k) * 4 + s] = (uint8_t)(t->color + 1);
      }
    }
}

static double round_(double v) { return floor_(v + 0.5); } /* Math.round for v >= 0 */

/* The resolve and the PNG: the samples' premultiplied average, then unpremultiplied. */
static void resolve(Fern *t) {
  for (int32_t p = 0; p < t->w * t->h; p++) {
    int n = 0, sum[3] = {0, 0, 0};
    for (int s = 0; s < 4; s++) {
      int c = t->smp[p * 4 + s];
      if (c) { n++; for (int k = 0; k < 3; k++) sum[k] += PALETTE[c - 1][k]; }
    }
    uint8_t *o = t->px + p * 4;
    if (!n) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
    double a = round_(n * 255 / 4.0);
    for (int k = 0; k < 3; k++) o[k] = (uint8_t)round_(round_(sum[k] / 4.0) * 255 / a);
    o[3] = (uint8_t)a;
  }
}

int fern_run(Fern *t, int32_t steps) {
  if (t->i == 0) for (int k = 0; k < 20; k++) step(t, pick(t)); /* the warm-up */
  for (int32_t end = t->i + steps; t->i < t->iterations && t->i < end; t->i++) {
    int k = pick(t);
    step(t, k);
    double dx = t->x, dy = t->y;
    if (k == 4 || k == 5) { circle_point(t, k - 4, &dx, &dy); t->color = k == 4 ? 1 : 2; }
    else if (k == 0) t->color = 0;
    if (tex_random(&t->rng) < 0.5) { dx = -dx; dy = -dy; }
    double rx = dx * t->cos_rot - dy * t->sin_rot, ry = dx * t->sin_rot + dy * t->cos_rot;
    double px = t->w / 2.0 + rx * t->scale + 0, py = t->h / 2.0 - (ry * t->scale - 0);
    if (t->smp) { cover(t, (float)px, (float)py); continue; }
    /* Without samples: a 1-pixel point at the float32 position covers the pixel it falls in. */
    double fx = floor_((double)(float)px), fy = floor_((double)(float)py);
    if (fx >= 0 && fx < t->w && fy >= 0 && fy < t->h) {
      uint8_t *p = t->px + (((int32_t)fy * t->w + (int32_t)fx) << 2);
      p[0] = PALETTE[t->color][0]; p[1] = PALETTE[t->color][1]; p[2] = PALETTE[t->color][2]; p[3] = 255;
    }
  }
  if (t->i < t->iterations) return 0;
  if (t->smp && t->i == t->iterations) { resolve(t); t->i++; }
  return 1;
}
