/* textures.c - the wool, the wood and the fern (textures.h). Every expression keeps the TS's
 * order of operations, and JS's conversions are spelled out: a store into a Uint8ClampedArray
 * (clamp8), the `| 0` and `^` of int32 (toint32), a store into a Float32Array (a float cast). */
#include "textures.h"

/* The wood's cos is rounded correctly as Chrome's is (crtrig.c): its iterated map grows a last-bit
 * difference into a different picture. The wool and the fern take trig.c's, within an ulp, which
 * their pictures do not tell apart (tools/texparity.mjs measures it). */
double cr_cos(double), tex_sin(double), tex_cos(double), tex_tan(double), tex_fmod(double, double);

#define floor_ __builtin_floor
#define PI 3.141592653589793

/* ToUint8Clamp: NaN and below 0 are 0, above 255 is 255, and a half rounds to even, which is what
 * rint does in the default rounding mode (wasm's f64.nearest). */
static uint8_t clamp8(double v) {
  if (!(v > 0)) return 0;
  if (v >= 255) return 255;
  return (uint8_t)__builtin_rint(v);
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

static double z_value(const Wool *t);

void wool_begin(Wool *t, uint8_t *px, int32_t w, int32_t h, int32_t cx, int32_t cy, int32_t cw, int32_t ch,
                double offx, double offy) {
  *t = (Wool){.px = px, .w = w, .h = h, .cx = cx, .cy = cy, .cw = cw, .ch = ch, .offx = offx, .offy = offy, .horizontal = 1};
  for (int32_t i = 0; i < cw * ch * 3; i += 3) { px[i] = 113; px[i + 1] = 65; px[i + 2] = 27; } /* RGB: the weave is opaque */
  t->max = (int32_t)floor_(((double)w * h / (1920 * 1080)) * 2000000);
  t->switch_at = (int32_t)floor_(t->max * 0.4);
  t->z = z_value(t);
  t->tan_key = -1; /* no key is negative: both sides of the ^ are floors of non-negative values here */
}

/* zValue() reads only r, so it is worked out when r changes rather than on every fibre step. */
static double z_value(const Wool *t) {
  double cr = tex_cos(t->r) * 1000;
  return cr - floor_(cr);
}

/* Math.tan(key) > 0.3 for the red plaid, remembered for the last key: it changes every 80 rows. */
static int32_t red_of(Wool *t, int32_t key) {
  if (key != t->tan_key) { t->tan_key = key; t->tan_red = tex_tan((double)key) > 0.3 ? 100 : 0; }
  return t->tan_red;
}

/* writePixel, into the crop: a pixel of the full picture outside the crop is not kept. The loops
 * run over the part of the n x n square inside the crop, which is inside the picture. */
static void write_pixel(Wool *t, double x, double y, int32_t red, int32_t green, int32_t blue, double size) {
  double xi = floor_(x), yi = floor_(y);
  int32_t n = (int32_t)__builtin_ceil(size), x0 = (int32_t)xi, y0 = (int32_t)yi;
  double fx = x - xi, fy = y - yi;
  int32_t dx0 = t->cx - x0 > 0 ? t->cx - x0 : 0, dx1 = t->cx + t->cw - x0 < n ? t->cx + t->cw - x0 : n;
  int32_t dy0 = t->cy - y0 > 0 ? t->cy - y0 : 0, dy1 = t->cy + t->ch - y0 < n ? t->cy + t->ch - y0 : n;
  for (int32_t dy = dy0; dy < dy1; dy++) {
    uint8_t *row = t->px + ((y0 + dy - t->cy) * t->cw + (x0 - t->cx)) * 3;
    double dist_y = fabs_(dy - fy);
    for (int32_t dx = dx0; dx < dx1; dx++) {
      double dist_x = fabs_(dx - fx);
      double edge = fmax_(0.1, 1 - (dist_x + dist_y) / 4);
      double alpha = edge * 0.99, inv = 1 - alpha;
      uint8_t *p = row + dx * 3;
      p[0] = clamp8(p[0] * inv + red * alpha);
      p[1] = clamp8(p[1] * inv + green * alpha);
      p[2] = clamp8(p[2] * inv + blue * alpha);
    }
  }
}

/* A fibre step whose pixels all fall outside the crop changes nothing in it, so it is skipped
 * before its trig: a horizontal step writes columns r - 7 .. r + 8 and rows y, y + 1; a vertical
 * one columns x .. x + 3 and rows r - 8 .. r + 11 (its size is at most 1.4 * 2.7, 4 pixels). A
 * whole run of steps with one r outside the crop is jumped over. The margins below are wider. */
int wool_run(Wool *t, int32_t steps) {
  int32_t w = t->w, h = t->h, x0 = t->cx, x1 = t->cx + t->cw, y0 = t->cy, y1 = t->cy + t->ch;
  for (int32_t end = t->i + steps; t->i < t->max && t->i < end; t->i++) {
    int32_t i = t->i;
    if (i == t->switch_at) { t->horizontal = 0; t->r = 0; t->z = z_value(t); }
    if (t->horizontal) { /* horizontal fibres */
      if (i % w == 0) { t->u = t->z * 500 + 100; t->r += 5; t->z = z_value(t); }
      if (t->r < x0 - 9 || t->r > x1 + 9) { /* the rest of this run of w steps, up to the switch */
        int32_t next = i - i % w + w;
        if (next > t->switch_at && i < t->switch_at) next = t->switch_at;
        t->i = (next < t->max ? next : t->max) - 1;
        continue;
      }
      int32_t row = i % h;
      if (row < y0 - 2 || row > y1) continue;
      double phase = tex_sin(i / t->u);
      double dx = tex_sin((double)i - 1) + phase * 6;
      int32_t red = red_of(t, toint32(floor_((t->r + t->offx) / 80 + t->z / 4)) ^ toint32(floor_((row + t->offy) / 80)));
      double x = t->r + dx, y = row;
      if (x >= 0 && x < w && y >= 0 && y < h)
        write_pixel(t, x, y, toint32(209 + 46 * phase + red), toint32(208 + 45 * phase - red),
                    toint32(183 + 53 * phase - red / 2.0), 2);
    } else { /* vertical fibres */
      if (i % h == 0) { t->u = t->z * 500 + 100; t->r += 3; t->z = z_value(t); }
      if (t->r < y0 - 12 || t->r > y1 + 9) {
        int32_t next = i - i % h + h;
        t->i = (next < t->max ? next : t->max) - 1;
        continue;
      }
      int32_t col = i % w;
      if (col < x0 - 4 || col > x1) continue;
      double phase = tex_sin(i / t->u);
      int32_t red = red_of(t, toint32(floor_((t->r + t->offy) / 80 + t->z / 4)) ^ toint32(floor_((col + t->offx) / 80)));
      double dx = 4 * tex_sin((double)i - 1) + phase * 4;
      double x = col, y = t->r + dx;
      if (x >= 0 && x < w && y >= 0 && y < h)
        write_pixel(t, x, y, toint32(189 + 46 * phase + red), toint32(188 + 45 * phase - red),
                    toint32(163 + 53 * phase - red / 2.0), 1.4 * (phase + 1.7));
    }
  }
  return t->i >= t->max;
}

/* ---- the wood (WoodTexture.tsx generateWoodPixelData) -------------------- */

void wood_begin(Wood *t, float *col, uint8_t *px, int32_t w, int32_t h, double offx, double offy, const TexRect *win, int32_t nwin) {
  if (nwin > WOOD_MAX_WIN) __builtin_trap();
  *t = (Wood){.col = col, .px = px, .w = w, .h = h, .nwin = nwin, .offx = offx, .offy = offy};
  for (int32_t k = 0; k < nwin; k++) t->rect[k] = win[k];
  uint8_t base[3] = {70, 14, 9}; /* what the blit makes of the base colour, as below */
  for (int k = 0; k < 3; k++) { double v = (float)(base[k] / 255.0) * 255.0 + 0.5; base[k] = (uint8_t)v; }
  for (int32_t i = 0; i < w * h; i++) { px[i * 3] = base[0]; px[i * 3 + 1] = base[1]; px[i * 3 + 2] = base[2]; } /* RGB: opaque */
}

static void rect_start(Wood *t) {
  const TexRect *r = &t->rect[t->win];
  for (int32_t i = 0; i < r->w * r->h; i++) {
    t->col[i * 3] = (float)(70 / 255.0);
    t->col[i * 3 + 1] = (float)(14 / 255.0);
    t->col[i * 3 + 2] = (float)(9 / 255.0);
  }
}

/* One column D(T) of the TS, kept inside rectangle r. The TS's row I writes x in [xpos, xend) of
 * row I; an x below 0 lands at w + x of row I - 1 (a Float32Array index, I * w + x, does not care
 * which row it is), except on row 0, where the index is below 0 and the store is dropped. So a
 * pixel of row R hears from row R, or, near the right edge, from row R + 1; never both in one T. */
static void wood_column(Wood *t, const TexRect *r, double T) {
  int32_t w = t->w;
  double xf = tex_fmod((T + t->offx / 200) * 200, w);
  int32_t xpos = toint32(xf), xend = xpos + 40 < w ? xpos + 40 : w, xcenter = xpos + 20;
  int32_t rx1 = r->x + r->w, ry1 = r->y + r->h;
  /* the stripe's part at x >= 0 inside the rectangle's columns, and its part below 0 (at w + x) */
  int32_t a0 = xpos > r->x ? xpos : r->x, a1 = xend < rx1 ? xend : rx1;
  int32_t b0 = xpos > r->x - w ? xpos : r->x - w, b1 = xend < 0 ? xend : 0;
  if (b1 > rx1 - w) b1 = rx1 - w;
  int here = a0 < a1, wrapped = xpos < 0 && b0 < b1;
  if (!here && !wrapped) return;
  int32_t top = r->y, bottom = ry1 + (wrapped ? 1 : 0); /* source rows */
  if (bottom > t->h) bottom = t->h;
  for (int32_t I = bottom - 1; I >= top; I--) {
    int row_here = here && I < ry1, row_wrapped = wrapped && I - 1 >= r->y && I >= 1;
    if (!row_here && !row_wrapped) continue;
    double ifac = (float)((I + t->offy) * 0.001); /* the TS keeps these in a Float32Array */
    double b = (T + t->offx * 0.01) / 24;
    for (int32_t k = 24; k >= 0; k--) {
      double half = (b * b) * 0.5;
      b = cr_cos(ifac + cr_cos(half) * b + 4 + t->offy * 0.001) * b - 2.8;
      if (b > 0) {
        double red = (b * 120) / 255, green = (b * b * 14) / 255, blue = 9 / 255.0;
        for (int part = 0; part < 2; part++) {
          int32_t x0 = part ? b0 : a0, x1 = part ? b1 : a1, ty = part ? I - 1 : I, shift = part ? w : 0;
          if (!(part ? row_wrapped : row_here)) continue;
          float *c = t->col + ((ty - r->y) * r->w + (x0 + shift - r->x)) * 3;
          for (int32_t x = x0; x < x1; x++, c += 3) {
            double dist = fabs_((double)(x - xcenter)) / (40 / 2);
            double edge = fmax_(0.3, 1 - dist * 0.5);
            double ea = 0.1 * edge, iea = 1 - ea;
            c[0] = (float)(red * ea + c[0] * iea);
            c[1] = (float)(green * ea + c[1] * iea);
            c[2] = (float)(blue * ea + c[2] * iea);
          }
        }
      }
    }
  }
}

/* The blit for one rectangle: each float colour to a byte, as the GL draw writes it. */
static void rect_blit(Wood *t) {
  const TexRect *r = &t->rect[t->win];
  for (int32_t y = 0; y < r->h; y++)
    for (int32_t x = 0; x < r->w; x++) {
      const float *c = t->col + (y * r->w + x) * 3;
      uint8_t *o = t->px + ((r->y + y) * t->w + r->x + x) * 3;
      for (int k = 0; k < 3; k++) {
        double v = c[k] * 255.0 + 0.5;
        o[k] = v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)v;
      }
    }
}

int wood_run(Wood *t, int32_t columns) {
  for (int32_t n = 0; t->win < t->nwin && n < columns; n++) {
    if (t->column == 0) rect_start(t);
    wood_column(t, &t->rect[t->win], t->column / 60.0);
    if (++t->column == 576) { rect_blit(t); t->column = 0; t->win++; }
  }
  return t->win >= t->nwin;
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
  t->cos_rot = tex_cos(rot);
  t->sin_rot = tex_sin(rot);
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
  *x = CIRCLE[c][0] + radius * tex_cos(angle);
  *y = CIRCLE[c][1] + radius * tex_sin(angle);
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
