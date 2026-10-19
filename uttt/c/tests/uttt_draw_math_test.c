/* Two private pieces of the drawing's arithmetic, held to the libm they replace:
 *   - the round cap's fan (uttt_pen.c CAP_COS / CAP_SIN) is cos and sin of
 *     pi * j / 6, to 1e-6, entry 0 unused, and the fan closes at both ends;
 *   - pow75 (uttt_draw.c) is x^0.75 as sqrtf(x) * sqrtf(sqrtf(x)): within 2 ulp
 *     of powf over the range the drawing feeds it (REF / length, and the
 *     roughness ratio).
 * The test compiles both .c files into its own unit (they are linked out of the
 * list in the Makefile) to reach their statics. */
#include "../src/uttt_pen.c"
#include "../src/uttt_draw.c"
#include <math.h>
#include <stdio.h>
#define CHECK_PRINT_CAP 1000000
#include "../../../shared/c/test/check.h"

static int ulps(float a, float b)
{
    if (a == b) return 0;
    int32_t x, y;
    memcpy(&x, &a, 4); memcpy(&y, &b, 4);
    return abs(x - y);
}

int main(void)
{
    TEST("the cap fan");
    const double PI = 3.14159265358979323846;
    for (int j = 1; j < CAP; j++) {
        OK(fabs((double)CAP_COS[j] - cos(PI * j / CAP)) < 1e-6, "CAP_COS[j] = cos(pi j / 6)");
        OK(fabs((double)CAP_SIN[j] - sin(PI * j / CAP)) < 1e-6, "CAP_SIN[j] = sin(pi j / 6)");
        OK(fabs((double)CAP_COS[j] * CAP_COS[j] + (double)CAP_SIN[j] * CAP_SIN[j] - 1.0) < 1e-6, "a unit vector");
    }
    OK(CAP == 6, "six steps make the half circle");
    OK(CAP_COS[3] == 0.f && CAP_SIN[3] == 1.f, "the middle of the fan is straight up, exactly");

    TEST("pow75");
    int worst = 0;
    OK(pow75(1.f) == 1.f, "1 -> 1");
    OK(pow75(0.f) == 0.f, "0 -> 0");
    for (int i = 0; i <= 4000; i++) {
        float x = 1e-3f * (float)pow(1.0035, i);   /* 1e-3 .. ~1e3, geometric */
        int u = ulps(pow75(x), powf(x, .75f));
        if (u > worst) worst = u;
        OK(u <= 2, "within 2 ulp of powf(x, .75)");
    }
    printf("pow75: worst %d ulp over 4001 points\n", worst);
    return report("uttt_draw_math_test");
}
