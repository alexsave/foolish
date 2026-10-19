/* libm_test.c - the freestanding wasm libm's sincos / sincosf (libm.c) and the
 * sc_sincos(f) the products call (sincos.h).
 *
 * libm.c defines sin, cos, sincos and the rest under the platform's own names,
 * so, as a product's wrapper does, every name is renamed (wl_) before the file
 * is included. What is held:
 *
 *   - sincos(x) is BIT-IDENTICAL to sin(x) and cos(x) (one range reduction
 *     instead of two, the same series, the quadrant swap and negate), and
 *     sincosf(x) to sinf(x) and cosf(x), over a dense sweep, the quadrant
 *     edges and one ulp either side of them, tiny and huge angles, NaN and
 *     both infinities (the same bits, whatever they are, on this platform);
 *   - the pair is the right pair: sin^2 + cos^2 = 1 and both agree with the
 *     platform's libm to 1e-9 where the reduction is exact (|x| < 1000), so
 *     "identical to each other" cannot hide two copies of one mistake;
 *   - sc_sincos / sc_sincosf (sincos.h), native spelling, agree with sin/cos.
 *
 * Native-only: the wasm kernel links libm.c itself, which is what this runs. */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "../test/check.h"

#define sin     wl_sin
#define cos     wl_cos
#define sincos  wl_sincos
#define exp     wl_exp
#define log     wl_log
#define log2    wl_log2
#define sinf    wl_sinf
#define cosf    wl_cosf
#define sincosf wl_sincosf
#define logf    wl_logf
#define powf    wl_powf
#define acosf   wl_acosf
#define lroundf wl_lroundf
#define roundf  wl_roundf
#define fminf   wl_fminf
#define fmaxf   wl_fmaxf
#include "libm.c"
#undef sin
#undef cos
#undef sincos
#undef sinf
#undef cosf
#undef sincosf
#undef exp
#undef log
#undef log2
#undef logf
#undef powf
#undef acosf
#undef lroundf
#undef roundf
#undef fminf
#undef fmaxf
#include "../sincos.h"

static int same_d(double a, double b) { return memcmp(&a, &b, sizeof a) == 0; }
static int same_f(float a, float b)   { return memcmp(&a, &b, sizeof a) == 0; }

static void both(double x)
{
    double s, c;
    wl_sincos(x, &s, &c);
    CHECK(same_d(s, wl_sin(x)), "sincos sin bits at %.17g", x);
    CHECK(same_d(c, wl_cos(x)), "sincos cos bits at %.17g", x);
    float fs, fc, xf = (float)x;
    wl_sincosf(xf, &fs, &fc);
    CHECK(same_f(fs, wl_sinf(xf)), "sincosf sin bits at %.9g", (double)xf);
    CHECK(same_f(fc, wl_cosf(xf)), "sincosf cos bits at %.9g", (double)xf);
}

int main(void)
{
    TEST("sincos: a dense sweep, bit-identical to sin and cos");
    for (int i = -100000; i <= 100000; i++) both((double)i * 0.00123457);

    TEST("sincos: every quadrant edge and one ulp either side, both signs");
    for (int k = -40; k <= 40; k++) {
        double e = (double)k * (3.14159265358979323846 / 2);
        both(e);
        both(nextafter(e, 1e300));
        both(nextafter(e, -1e300));
        both(e + 0.78539816339744830962);     /* the middle of the quadrant switch: k + 1/2 */
        both(e - 0.78539816339744830962);
    }

    TEST("sincos: zero, tiny, huge, NaN and the infinities");
    const double odd[] = { 0.0, -0.0, 1e-300, -1e-300, 5e-324, 1e-9, 1e6, -1e6, 1e12, 1e15, -1e15,
                           (double)NAN, (double)INFINITY, -(double)INFINITY, 1e300, -1e300
    };
    for (size_t i = 0; i < sizeof odd / sizeof odd[0]; i++) both(odd[i]);
    double s, c;
    /* NaN and the infinities in, NaN out - and no float-to-integer overflow on the way (the sanitized lane
     * checks that, -fsanitize=float-cast-overflow). */
    const double bad[] = { (double)NAN, (double)INFINITY, -(double)INFINITY };
    for (size_t i = 0; i < 3; i++) {
        wl_sincos(bad[i], &s, &c);
        CHECK(isnan(s) && isnan(c) && isnan(wl_sin(bad[i])) && isnan(wl_cos(bad[i])), "sin/cos/sincos of a non-finite angle is NaN");
    }
    wl_sincos(1e300, &s, &c);
    CHECK(s == s && c == c, "a huge finite angle gives numbers, not NaN");
    wl_sincos(0.0, &s, &c);
    CHECK(s == 0.0 && c == 1.0, "sincos(0) is (0, 1): %g %g", s, c);
    wl_sincos(3.14159265358979323846 / 2, &s, &c);
    CHECK(fabs(s - 1) < 1e-12 && fabs(c) < 1e-12, "sincos(pi/2) is (1, 0): %g %g", s, c);
    wl_sincos(3.14159265358979323846, &s, &c);
    CHECK(fabs(s) < 1e-12 && fabs(c + 1) < 1e-12, "sincos(pi) is (0, -1): %g %g", s, c);
    wl_sincos(3 * 3.14159265358979323846 / 2, &s, &c);
    CHECK(fabs(s + 1) < 1e-12 && fabs(c) < 1e-12, "sincos(3pi/2) is (-1, 0): %g %g", s, c);

    TEST("sincos: the pair is the right pair (the platform's libm, and the identity)");
    for (int i = -20000; i <= 20000; i++) {
        double x = (double)i * 0.0471;
        wl_sincos(x, &s, &c);
        CHECK(fabs(s - sin(x)) < 1e-9 && fabs(c - cos(x)) < 1e-9, "sincos vs libm at %.17g: %g %g", x, s - sin(x), c - cos(x));
        CHECK(fabs(s * s + c * c - 1) < 1e-9, "sin^2 + cos^2 at %.17g", x);
    }

    TEST("sc_sincos: the call the products make");
    for (int i = -2000; i <= 2000; i++) {
        double x = (double)i * 0.0173;
        double ds, dc;
        sc_sincos(x, &ds, &dc);
        CHECK(same_d(ds, sin(x)) && same_d(dc, cos(x)), "sc_sincos at %.17g", x);
        float fs, fc;
        sc_sincosf((float)x, &fs, &fc);
        CHECK(same_f(fs, sinf((float)x)) && same_f(fc, cosf((float)x)), "sc_sincosf at %.9g", x);
    }
    return report("libm_test");
}
