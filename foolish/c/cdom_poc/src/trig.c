/* trig.c - sin, cos and tan for the wool and the fern, and JS's % for the wood: fast, within an
 * ulp of Chrome's, which is all the textures that use them can tell (tools/texparity.mjs measures
 * it on every run). The wood's cos is crtrig.c's, rounded correctly: its 25 iterations of
 * `b = cos(...) * b - 2.8` grow a last-bit difference into a different picture.
 *
 * The polynomials are musl's (FreeBSD's fdlibm k_sin.c, k_cos.c, k_tan.c; MIT licence, Copyright
 * 1993 Sun Microsystems and the musl authors). The argument reduction is not: musl's reduces
 * |x| >= 2^20 pi/2 through its 1,200-bit 2/pi table, which made the wool's sin(i - 1) for
 * i up to 8,294,400 the slowest part of the texture. Here pi/2 is split in three parts of 25 bits
 * and a fourth, so k * part is exact for |k| < 2^28 and x - k pi/2 comes out as a double-double
 * good to about 2^-100 of x (Cody and Waite). The textures stay far below 2^28 (the largest
 * argument is the wool's last fibre index); beyond it the function traps rather than guess. */
#include <stdint.h>

static const double P1 = 0x1.921fb5p+0, P2 = 0x1.110b46p-26, P3 = 0x1.1a6263p-54, P4 = 0x1.8a2e03707344ap-81,
                    INV_PIO2 = 0x1.45f306dc9c883p-1;

/* x - k pi/2 as hi + lo, and k mod 4. */
static int reduce(double x, double *hi, double *lo) {
  if (!(__builtin_fabs(x) < 0x1p28)) __builtin_trap();
  double k = __builtin_floor(x * INV_PIO2 + 0.5);
  double r = (x - k * P1) - k * P2; /* both exact: k * P1 and k * P2 fit 53 bits, and x is close */
  double t = k * P3, s = r - t;     /* exact product, rounded difference: keep what it dropped */
  *hi = s;
  *lo = ((r - s) - t) - k * P4;
  return (int)((int64_t)k & 3);
}

static const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                    S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                    S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;

/* sin(x + y) on |x + y| <= pi/4 (musl __sin with iy = 1). */
static double k_sin(double x, double y) {
  double z = x * x, w = z * z, r = S2 + z * (S3 + z * S4) + z * w * (S5 + z * S6), v = z * x;
  return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

static const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                    C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                    C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;

/* cos(x + y) on |x + y| <= pi/4 (musl __cos). */
static double k_cos(double x, double y) {
  double z = x * x, w = z * z, r = z * (C1 + z * (C2 + z * C3)) + w * w * (C4 + z * (C5 + z * C6));
  double hz = 0.5 * z;
  w = 1.0 - hz;
  return w + (((1.0 - w) - hz) + (z * r - x * y));
}

static const double T[] = {
  3.33333333333334091986e-01, 1.33333333333201242699e-01, 5.39682539762260521377e-02, 2.18694882948595424599e-02,
  8.86323982359930005737e-03, 3.59207910759131235356e-03, 1.45620945432529025516e-03, 5.88041240820264096874e-04,
  2.46463134818469906812e-04, 7.81794442939557092300e-05, 7.14072491382608190305e-05, -1.85586374855275456654e-05,
  2.59073051863633712884e-05,
};
static const double PIO4 = 7.85398163397448278999e-01, PIO4LO = 3.06161699786838301793e-17;

static double low_word_zero(double v) { return __builtin_bit_cast(double, __builtin_bit_cast(uint64_t, v) & 0xffffffff00000000ull); }

/* tan(x + y) on |x + y| <= pi/4 (odd = 0) or -1/tan(x + y) (odd = 1) (musl __tan). */
static double k_tan(double x, double y, int odd) {
  double z, r, v, w, s, a, w0, a0;
  int big = (__builtin_bit_cast(uint64_t, x) >> 32 & 0x7fffffff) >= 0x3FE59428, sign = x < 0; /* |x| >= 0.6744 */
  if (big) {
    if (sign) { x = -x; y = -y; }
    x = (PIO4 - x) + (PIO4LO - y);
    y = 0.0;
  }
  z = x * x;
  w = z * z;
  r = T[1] + w * (T[3] + w * (T[5] + w * (T[7] + w * (T[9] + w * T[11]))));
  v = z * (T[2] + w * (T[4] + w * (T[6] + w * (T[8] + w * (T[10] + w * T[12])))));
  s = z * x;
  r = y + z * (s * (r + v) + y) + s * T[0];
  w = x + r;
  if (big) {
    s = 1 - 2 * odd;
    v = s - 2.0 * (x + (r - w * w / (w + s)));
    return sign ? -v : v;
  }
  if (!odd) return w;
  w0 = low_word_zero(w); /* -1/(x + r) has up to 2 ulp error, so computed accurately */
  v = r - (w0 - x);
  a0 = a = -1.0 / w;
  a0 = low_word_zero(a0);
  return a0 + a * (1.0 + a0 * w0 + a0 * v);
}

double tex_sin(double x) {
  double hi, lo;
  switch (reduce(x, &hi, &lo)) {
  case 0: return k_sin(hi, lo);
  case 1: return k_cos(hi, lo);
  case 2: return -k_sin(hi, lo);
  default: return -k_cos(hi, lo);
  }
}

double tex_cos(double x) {
  double hi, lo;
  switch (reduce(x, &hi, &lo)) {
  case 0: return k_cos(hi, lo);
  case 1: return -k_sin(hi, lo);
  case 2: return -k_cos(hi, lo);
  default: return k_sin(hi, lo);
  }
}

double tex_tan(double x) {
  double hi, lo;
  int q = reduce(x, &hi, &lo);
  return k_tan(hi, lo, q & 1);
}

/* JS's x % y on doubles, exact as C's fmod (musl's fmod.c). The wood's one use: where a column's
 * stripe starts, ((T + offsetX / 200) * 200) % width. */
double tex_fmod(double x, double y) {
  union { double f; uint64_t i; } ux = {x}, uy = {y};
  int ex = ux.i >> 52 & 0x7ff, ey = uy.i >> 52 & 0x7ff, sx = ux.i >> 63;
  uint64_t i, uxi = ux.i;
  if (uy.i << 1 == 0 || y != y || ex == 0x7ff) return (x * y) / (x * y);
  if (uxi << 1 <= uy.i << 1) return uxi << 1 == uy.i << 1 ? 0 * x : x;
  if (!ex) { for (i = uxi << 12; i >> 63 == 0; ex--, i <<= 1) {} uxi <<= -ex + 1; }
  else { uxi &= -1ULL >> 12; uxi |= 1ULL << 52; }
  if (!ey) { for (i = uy.i << 12; i >> 63 == 0; ey--, i <<= 1) {} uy.i <<= -ey + 1; }
  else { uy.i &= -1ULL >> 12; uy.i |= 1ULL << 52; }
  for (; ex > ey; ex--) {
    i = uxi - uy.i;
    if (i >> 63 == 0) { if (i == 0) return 0 * x; uxi = i; }
    uxi <<= 1;
  }
  i = uxi - uy.i;
  if (i >> 63 == 0) { if (i == 0) return 0 * x; uxi = i; }
  for (; uxi >> 52 == 0; uxi <<= 1, ex--) {}
  if (ex > 0) { uxi -= 1ULL << 52; uxi |= (uint64_t)ex << 52; }
  else uxi >>= -ex + 1;
  uxi |= (uint64_t)sx << 63;
  ux.i = uxi;
  return ux.f;
}
