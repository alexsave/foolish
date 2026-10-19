/* ease_test.c - the cubic-bezier solver (ease.h): the ends are exact, the
 * linear curve is the identity, and for the repo's named curves the solved y
 * agrees with the curve walked by its parameter (x(u), y(u)), including the
 * overshooting ones (y above 1) and the one with a flat start.
 *
 * Exits 1 on any failure. */
#include "test/check.h"
#include "ease.h"

int main(void)
{
    TEST("ease: the ends and the identity");
    CHECK(ease_cubic_bezier(.25f, .46f, .45f, .94f, 0.f) == 0.f, "x=0 is 0");
    CHECK(ease_cubic_bezier(.25f, .46f, .45f, .94f, 1.f) == 1.f, "x=1 is 1");
    CHECK(ease_cubic_bezier(.25f, .46f, .45f, .94f, -3.f) == 0.f, "below 0 clamps");
    CHECK(ease_cubic_bezier(.25f, .46f, .45f, .94f, 7.f) == 1.f, "above 1 clamps");
    for (int i = 1; i < 20; i++) {
        const float x = (float)i / 20.f;
        const float y = ease_cubic_bezier(0.f, 0.f, 1.f, 1.f, x);
        CHECK(ease_absf(y - x) < 1e-4f, "linear at %g gave %g", (double)x, (double)y);
    }

    TEST("ease: the solved y lies on the curve");
    static const float C[][4] = {
        { .25f, .46f, .45f, .94f }, { .3f, 1.22f, .45f, 1.f }, { .2f, 1.5f, .5f, 1.f },
        { .55f, 0.f, .75f, .2f }, { .25f, .8f, .45f, 1.f }, { 0.f, 0.f, .58f, 1.f },
        { .32f, .72f, .4f, 1.f }, { .3f, .66f, .36f, 1.f }, { .4f, .8f, .4f, 1.f },
        /* flat or vertical tangents: Newton's derivative vanishes and the bisection fallback decides */
        { 0.f, 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f, 1.f }, { 1.f, 1.f, 0.f, 0.f },
        { 0.f, 1.f, 0.f, 1.f }, { 1.f, 0.f, 1.f, 1.f }, { 1.f, .2f, 0.f, .8f },
    };
    for (unsigned c = 0; c < sizeof C / sizeof C[0]; c++) {
        for (int i = 1; i < 40; i++) {
            const float u = (float)i / 40.f, v = 1.f - u;
            const float x = 3 * v * v * u * C[c][0] + 3 * v * u * u * C[c][2] + u * u * u;
            const float y = 3 * v * v * u * C[c][1] + 3 * v * u * u * C[c][3] + u * u * u;
            const float got = ease_cubic_bezier(C[c][0], C[c][1], C[c][2], C[c][3], x);
            CHECK(ease_absf(got - y) < 2e-3f, "curve %u at u=%g: x=%g wanted y=%g got %g", c, (double)u,
                  (double)x, (double)y, (double)got);
        }
    }

    TEST("ease: both ends are exact for every curve (x <= 0 is 0 and x >= 1 is 1, bit for bit)");
    for (unsigned c = 0; c < sizeof C / sizeof C[0]; c++) {
        CHECK(ease_cubic_bezier(C[c][0], C[c][1], C[c][2], C[c][3], 0.f) == 0.f, "curve %u at 0", c);
        CHECK(ease_cubic_bezier(C[c][0], C[c][1], C[c][2], C[c][3], 1.f) == 1.f, "curve %u at 1", c);
        CHECK(ease_cubic_bezier(C[c][0], C[c][1], C[c][2], C[c][3], -1e-9f) == 0.f, "curve %u just below 0", c);
        CHECK(ease_cubic_bezier(C[c][0], C[c][1], C[c][2], C[c][3], 1.000001f) == 1.f, "curve %u just above 1", c);
    }

    TEST("ease: the cubic ease-in {0,0,0,0} has y = x");
    for (int i = 1; i < 100; i++) {
        const float x = (float)i / 100.f;
        const float y = ease_cubic_bezier(0.f, 0.f, 0.f, 0.f, x);
        CHECK(ease_absf(y - x) < 2e-3f, "x=%g gave %g", (double)x, (double)y);
    }
    return report("ease_test");
}
