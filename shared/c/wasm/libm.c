/* The freestanding maths for a wasm32 kernel built -nostdlib -ffreestanding:
 * what include/math.h declares and does not map to a wasm instruction.
 *
 * Ordinary range-reduced series, in double: close to a platform libm, not
 * bit-identical to one. Checked by drawing whole games with it and with the
 * native libm and comparing every point - the difference is below what a
 * canvas can show, which is the bar a picture has. A kernel whose DECISIONS
 * depended on these would need a stricter one. */
#include <math.h>

#define PI  3.14159265358979323846
#define LN2 0.69314718055994530942

/* sin on [-pi/4, pi/4] and cos on the same, then quadrant by quadrant */
static double sin_k(double x)
{
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156))))));
}
static double cos_k(double x)
{
    double x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132)))));
}
static double quad(double x, int *q)
{
    double k = __builtin_floor(x / (PI / 2) + .5);
    *q = (int)((long long)k & 3);
    return x - k * (PI / 2);
}
double sin(double x)
{
    int q; double r = quad(x, &q);
    switch (q) { case 0: return sin_k(r); case 1: return cos_k(r); case 2: return -sin_k(r); default: return -cos_k(r); }
}
double cos(double x)
{
    int q; double r = quad(x, &q);
    switch (q) { case 0: return cos_k(r); case 1: return -sin_k(r); case 2: return -cos_k(r); default: return sin_k(r); }
}

/* e^x = 2^k e^r, |r| <= ln2/2, and 2^k built straight into the exponent */
double exp(double x)
{
    if (x > 709) return __builtin_inf();
    if (x < -745) return 0;
    double k = __builtin_floor(x / LN2 + .5), r = x - k * LN2, t = 1, s = 1;
    for (int i = 1; i < 14; i++) { t *= r / i; s += t; }
    union { double d; unsigned long long u; } p;
    int e = (int)k;
    /* split a scale that would leave the normal range into two steps */
    if (e < -1000) { s *= 0x1p-1000; e += 1000; }
    if (e > 1000)  { s *= 0x1p1000;  e -= 1000; }
    p.u = (unsigned long long)(e + 1023) << 52;
    return s * p.d;
}

/* ln x = e ln2 + 2 atanh((m-1)/(m+1)), m in [sqrt(1/2), sqrt(2)) */
double log(double x)
{
    if (!(x > 0)) return x == 0 ? -__builtin_inf() : __builtin_nan("");
    union { double d; unsigned long long u; } v = { x };
    int e = (int)((v.u >> 52) & 0x7ff);
    if (e == 0) { v.d = x * 0x1p54; e = (int)((v.u >> 52) & 0x7ff) - 54; }
    e -= 1023;
    v.u = (v.u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
    double m = v.d;
    if (m > 1.41421356237309504880) { m /= 2; e++; }
    double z = (m - 1) / (m + 1), z2 = z * z, t = z, s = 0;
    for (int i = 1; i < 40; i += 2) { s += t / i; t *= z2; }
    return e * LN2 + 2 * s;
}
double log2(double x) { return log(x) / LN2; }

float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float logf(float x) { return (float)log(x); }

float powf(float x, float y)
{
    if (y == 0) return 1;
    if (x == 0) return y > 0 ? 0 : __builtin_inff();
    if (x < 0) return __builtin_nanf("");   /* the kernel never asks */
    return (float)exp(y * log(x));
}

/* atan by halving the angle until the series is short, then acos from it */
static double atan_(double x)
{
    int neg = x < 0; if (neg) x = -x;
    int inv = x > 1; if (inv) x = 1 / x;
    int h = 0;
    while (x > .2) { x = x / (1 + __builtin_sqrt(1 + x * x)); h++; }
    double x2 = x * x, t = x, s = 0;
    for (int i = 1; i < 30; i += 2) { s += t / i; t *= -x2; }
    s *= (double)(1 << h);
    if (inv) s = PI / 2 - s;
    return neg ? -s : s;
}
float acosf(float x)
{
    if (x >= 1) return 0;
    if (x <= -1) return (float)PI;
    return (float)(2 * atan_(__builtin_sqrt((1 - (double)x) / (1 + (double)x))));
}

/* libm's NaN rule: a NaN argument loses to the number */
float fminf(float a, float b) { return a != a ? b : b != b ? a : a < b ? a : b; }
float fmaxf(float a, float b) { return a != a ? b : b != b ? a : a > b ? a : b; }

long lroundf(float x) { return (long)(x < 0 ? x - .5f : x + .5f); }
