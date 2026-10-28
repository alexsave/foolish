/* crtrig.c - cos rounded correctly, as Chrome's Math.cos is (V8 takes them
 * from LLVM's libc). The wood texture iterates b = cos(...) * b - 2.8 twenty-five times per pixel
 * row, so a last-bit difference in cos grows into a different picture; this makes the C cos the
 * engine's, bit for bit, wherever the parity test looked.
 *
 * How: the argument is reduced by pi/2 in double-double (|x| < 2^19; beyond that musl's, which the
 * textures never reach), the first terms of the series are summed in double-double and the rest
 * in double, and the result is the double nearest that sum. When the sum lies too close to the
 * midpoint between two doubles for its error bound to decide, the whole series is summed in
 * double-double instead. No FMA: products are split (Dekker), as wasm has no fused multiply-add. */
#include <stdint.h>

double tex_cos(double);

typedef struct { double hi, lo; } dd;

static dd two_sum(double a, double b) {
  double s = a + b, bb = s - a;
  return (dd){s, (a - (s - bb)) + (b - bb)};
}
static dd fast_two_sum(double a, double b) { double s = a + b; return (dd){s, b - (s - a)}; }
static void split(double a, double *h, double *l) {
  double c = 134217729.0 * a;
  *h = c - (c - a);
  *l = a - *h;
}
static dd two_prod(double a, double b) {
  double p = a * b, ah, al, bh, bl;
  split(a, &ah, &al);
  split(b, &bh, &bl);
  return (dd){p, ((ah * bh - p) + ah * bl + al * bh) + al * bl};
}
static dd dd_mul(dd a, dd b) {
  dd p = two_prod(a.hi, b.hi);
  return fast_two_sum(p.hi, p.lo + (a.hi * b.lo + a.lo * b.hi));
}
static dd dd_add(dd a, dd b) {
  dd s = two_sum(a.hi, b.hi);
  return fast_two_sum(s.hi, s.lo + a.lo + b.lo);
}

/* pi/2 in three parts of 33 bits (fdlibm's pio2_1, pio2_2, pio2_3): k * part is exact for |k| < 2^20. */
static const double P1 = 1.57079632673412561417e+00, P2 = 6.07710050630396597660e-11, P3 = 2.02226624871116645580e-21,
                    P3T = 8.47842766036889956997e-32, INV_PIO2 = 6.36619772367581382433e-01;

/* x - k pi/2 as a double-double, about 2^-115 of x. */
static dd reduce(double x, int *q) {
  double k = __builtin_floor(x * INV_PIO2 + 0.5);
  *q = (int)((int64_t)k & 3);
  double t = x - k * P1; /* exact: k * P1 is exact and t is close to x */
  dd r = two_sum(t, -k * P2);
  dd p3 = two_prod(k, P3);
  r = dd_add(r, (dd){-p3.hi, -p3.lo});
  return dd_add(r, (dd){-k * P3T, 0});
}

/* 1/n! as double-doubles, n = 0..29, made once. */
static dd inv_fact[30];
static int facts_made;
static void make_facts(void) {
  inv_fact[0] = (dd){1, 0};
  for (int n = 1; n < 30; n++) { /* divide by n: q = hi / n, then one correction */
    dd a = inv_fact[n - 1];
    double q = a.hi / n;
    dd p = two_prod(q, (double)n);
    double q2 = ((a.hi - p.hi - p.lo) + a.lo) / n;
    inv_fact[n] = fast_two_sum(q, q2);
  }
  facts_made = 1;
}

/* sin r (odd = 1) or cos r on |r| <= pi/4, all in double-double: the slow path. */
static dd series_dd(dd r, int odd) {
  dd z = dd_mul(r, r), s = {0, 0};
  int top = odd ? 27 : 28;
  for (int n = top; n >= (odd ? 1 : 0); n -= 2) {
    dd c = inv_fact[n];
    if ((n / 2) & 1) { c.hi = -c.hi; c.lo = -c.lo; }
    s = dd_add(dd_mul(s, z), c);
  }
  return odd ? dd_mul(s, r) : s;
}

/* The fast path: the first three terms in double-double, the rest in double. Returns the sum and,
 * in *err, a bound on its error (absolute). */
static dd series_fast(dd r, int odd, double *err) {
  dd z = dd_mul(r, r);
  double zh = z.hi, tail;
  dd head;
  if (odd) { /* r - r^3/3! + r^5/5!, then r^7 (-1/7! + z/9! - ...) */
    tail = 0;
    for (int n = 27; n >= 7; n -= 2) tail = tail * zh + ((n / 2) & 1 ? -inv_fact[n].hi : inv_fact[n].hi);
    dd h = dd_add((dd){-inv_fact[3].hi, -inv_fact[3].lo}, dd_mul(inv_fact[5], z));
    head = dd_add((dd){1, 0}, dd_mul(h, z));
    double z3 = zh * zh * zh;
    head = dd_mul(dd_add(head, (dd){tail * z3, 0}), r);
    *err = __builtin_fabs(r.hi) * (__builtin_fabs(tail * z3) * 0x1p-50 + 0x1p-98);
  } else { /* 1 - r^2/2! + r^4/4! - r^6/6!, then r^8 (1/8! - ...) */
    tail = 0;
    for (int n = 28; n >= 8; n -= 2) tail = tail * zh + ((n / 2) & 1 ? -inv_fact[n].hi : inv_fact[n].hi);
    dd h = dd_add(inv_fact[4], dd_mul((dd){-inv_fact[6].hi, -inv_fact[6].lo}, z));
    h = dd_add((dd){-inv_fact[2].hi, -inv_fact[2].lo}, dd_mul(h, z));
    head = dd_add((dd){1, 0}, dd_mul(h, z));
    double z4 = (zh * zh) * (zh * zh);
    head = dd_add(head, (dd){tail * z4, 0});
    *err = __builtin_fabs(tail * z4) * 0x1p-50 + 0x1p-98;
  }
  return head;
}

/* The double nearest v, if v's error bound cannot reach the midpoint; else NaN. */
static double nearest(dd v, double err) {
  double y = v.hi + v.lo;
  double e = (v.hi - y) + v.lo; /* what rounding dropped */
  uint64_t u = __builtin_bit_cast(uint64_t, y);
  /* the gap on the side e points to: below a power of two it is half the one above */
  int away = (e > 0) == (y > 0);
  double ulp = __builtin_fabs(__builtin_bit_cast(double, away ? u + 1 : u - 1) - y);
  double half = ulp / 2;
  double d = __builtin_fabs(e) - half;
  return (d < 0 ? -d : d) > err ? y : __builtin_nan("");
}

double cr_cos(double x) {
  if (!(__builtin_fabs(x) < 0x1p19)) return tex_cos(x); /* trig.c's; the wood never gets here */
  if (!facts_made) make_facts();
  if (__builtin_fabs(x) < 0x1p-27) return 1.0; /* cos x = 1, once rounded */
  int q;
  dd r = reduce(x, &q);
  int odd = q & 1, neg = q == 1 || q == 2; /* q 0 cos, 1 -sin, 2 -cos, 3 sin */
  double err;
  dd v = series_fast(r, odd, &err);
  double y = nearest(v, err);
  if (y != y) {
    v = series_dd(r, odd);
    y = v.hi + v.lo;
  }
  return neg ? -y : y;
}
