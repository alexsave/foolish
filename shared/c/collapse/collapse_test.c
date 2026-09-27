/* collapse_test.c - the auto-collapse's push (collapse.h).
 *
 *   cc -std=c11 -Wall -Wextra -Werror collapse_test.c -lm -o collapse_test && ./collapse_test
 *
 * No -I: the header is beside this file. Exits 1 on any failure. The two
 * numbers, the whole travel at the flip, the host's critically damped spring
 * through the slide, and exactly nothing from the end on, reached without a
 * step. */
#include <math.h>
#include <stdio.h>
#include "collapse.h"

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

/* Within 0.05 points, the tolerance the drawn sheet is held to. */
static int near(float a, float b) { return fabsf(a - b) < 0.05f; }
/* Within a part in 10^5 of the curve as it stood when it was lifted. */
static int pinned(float a, float b) { return fabsf(a - b) <= 1e-5f * fabsf(b); }

int main(void)
{
    OK(COLLAPSE_MS == 600 && COLLAPSE_RESPONSE_MS == 338, "the numbers: 600ms of push on a 338ms response");

    OK(collapse_push(500.f, 0) == 500.f && collapse_push(500.f, -5) == 500.f
       && collapse_push(584.f, 0) == 584.f, "the whole travel at the flip, and before it");

    /* At half the response, (1 + pi) e^-pi of the travel is left (0.178976),
     * less the tail's share. */
    OK(near(collapse_push(500.f, 169), 500.f * 0.178976f), "the host's spring at half its response");
    OK(pinned(collapse_push(1.f, 169), 0.17892541f) && pinned(collapse_push(584.f, 300), 14.4855738f)
       && pinned(collapse_push(1.f, 599), 3.28528677e-06f), "mid-slide and at the last step, on the pinned curve");

    int mono = 1, host = 1;
    float prev = 1e9f;
    for (int t = 0; t <= COLLAPSE_MS; t++) {
        float p = collapse_push(500.f, t);
        if (p > prev + 1e-4f || p < -1e-4f) mono = 0;
        prev = p;
        double w = 2.0 * 3.14159265358979 / 338.0;
        double want = 500.0 * (1.0 + w * t) * exp(-w * t);
        if (fabs(p - want) > 1.5) host = 0;
    }
    OK(mono, "the push only ever falls, and never below zero");
    OK(host, "and rides a critically damped spring on a 338ms response to within a point and a half");

    OK(collapse_push(500.f, COLLAPSE_MS) == 0.f && collapse_push(584.f, COLLAPSE_MS) == 0.f
       && collapse_push(500.f, 5000) == 0.f, "exactly nothing from the end on");
    OK(fabsf(collapse_push(500.f, COLLAPSE_MS - 1)) < .05f, "and no step to it at the release");
    OK(collapse_push(500.f, COLLAPSE_MS - 1) > 0.f, "the last step before the end still has something left");

    printf("collapse: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
