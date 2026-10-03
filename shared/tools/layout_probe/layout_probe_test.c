/* layout_probe.h: the pattern is what it says, and the judge counts what it
 * says it counts. */
#include "layout_probe.h"
#include "../../c/test/check.h"

#define N 27
#define MAXSIDE (N * 4)
static uint8_t img[MAXSIDE * MAXSIDE * 4], small[MAXSIDE * MAXSIDE * 4];

int main(void)
{
    TEST("the states cover the grid evenly");
    {
        int count[LP_STATES] = {0};
        for (uint32_t i = 0; i < 243 * 243; i++) {
            int s = lp_state(i);
            CHECK(s >= 0 && s < LP_STATES, "state %d at %u", s, i);
            count[s]++;
        }
        for (int k = 0; k < LP_STATES; k++)
            CHECK(count[k] > 19000 && count[k] < 20400, "state %d holds %d of 59049", k, count[k]);
        CHECK(lp_state(0) != lp_state(1) || lp_state(1) != lp_state(2) || lp_state(2) != lp_state(3),
              "the first four cells are one state");
    }

    TEST("a pattern judged as painted is whole");
    for (int grey = 0; grey <= 1; grey++)
        for (int p = 1; p <= 4; p++) {
            lp_fill(img, N, p, grey);
            LpVerdict v = lp_judge(img, N * p, N * p, N, grey);
            CHECK(v.cells == N * N, "cells %d", v.cells);
            CHECK(v.wrong == 0, "grey %d p %d: %d wrong", grey, p, v.wrong);
            CHECK(v.exact == N * N, "grey %d p %d: %d exact", grey, p, v.exact);
            CHECK(v.max_err == 0, "grey %d p %d: max_err %d", grey, p, v.max_err);
            CHECK(img[3] == 255, "alpha %d", img[3]);
        }

    TEST("one repainted cell is one wrong cell");
    {
        const int p = 3, cx = 5, cy = 11, side = N * p;
        lp_fill(img, N, p, LP_GREY);
        const int was = lp_state(cy * N + cx);
        const uint8_t *c = lp_rgb((was + 1) % LP_STATES, LP_GREY);
        for (int dy = 0; dy < p; dy++)
            for (int dx = 0; dx < p; dx++) {
                uint8_t *o = img + ((cy * p + dy) * side + cx * p + dx) * 4;
                o[0] = c[0]; o[1] = c[1]; o[2] = c[2];
            }
        LpVerdict v = lp_judge(img, side, side, N, LP_GREY);
        CHECK(v.wrong == 1, "%d wrong", v.wrong);
        CHECK(v.exact == N * N - 1, "%d exact", v.exact);
        CHECK(v.max_err >= 127, "max_err %d", v.max_err);
    }

    TEST("noise inside the margin decodes, noise past it does not");
    {
        const int p = 2, side = N * p;
        /* grey levels sit 127 and 128 apart, so 63 either way is still nearest */
        lp_fill(img, N, p, LP_GREY);
        for (int i = 0; i < side * side * 4; i++) {
            if (i % 4 == 3) continue;
            int d = (i / 4) % 2 ? 63 : -63, x = img[i] + d;
            img[i] = (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
        }
        LpVerdict v = lp_judge(img, side, side, N, LP_GREY);
        CHECK(v.wrong == 0, "%d wrong at 63", v.wrong);
        CHECK(v.max_err == 63, "max_err %d", v.max_err);
        CHECK(v.exact < N * N, "%d exact", v.exact);

        lp_fill(img, N, p, LP_GREY);
        for (int i = 0; i < side * side * 4; i++) {
            if (i % 4 == 3) continue;
            int x = img[i] + 70;
            img[i] = (uint8_t)(x > 255 ? 255 : x);
        }
        v = lp_judge(img, side, side, N, LP_GREY);
        CHECK(v.wrong > N * N / 4, "%d wrong at 70", v.wrong);
    }

    TEST("a picture that came back smaller is judged at its own size");
    {
        /* 4 px per cell, then every other pixel dropped: 2 px per cell */
        const int p = 4, side = N * p, half = side / 2;
        lp_fill(img, N, p, LP_COLOUR);
        for (int y = 0; y < half; y++)
            for (int x = 0; x < half; x++)
                for (int ch = 0; ch < 4; ch++)
                    small[(y * half + x) * 4 + ch] = img[((y * 2) * side + x * 2) * 4 + ch];
        LpVerdict v = lp_judge(small, half, half, N, LP_COLOUR);
        CHECK(v.wrong == 0, "%d wrong", v.wrong);
        CHECK(v.exact == N * N, "%d exact", v.exact);
    }

    TEST("the sample is the cell's centre, on each axis by that axis's size");
    {
        /* only the middle pixel of each 3 x 3 cell is right */
        const int p = 3, side = N * p;
        lp_fill(img, N, p, LP_GREY);
        for (int y = 0; y < side; y++)
            for (int x = 0; x < side; x++)
                if (x % p != 1 || y % p != 1) {
                    uint8_t *o = img + (y * side + x) * 4;
                    const uint8_t *c = lp_rgb((lp_state((uint32_t)((y / p) * N + x / p)) + 1) % LP_STATES, LP_GREY);
                    o[0] = c[0]; o[1] = c[1]; o[2] = c[2];
                }
        LpVerdict v = lp_judge(img, side, side, N, LP_GREY);
        CHECK(v.wrong == 0, "%d wrong", v.wrong);

        /* twice as wide as it is tall: 2 px by 1 px per cell */
        const int w = N * 2, h = N;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t *c = lp_rgb(lp_state((uint32_t)(y * N + x / 2)), LP_GREY);
                uint8_t *o = small + (y * w + x) * 4;
                o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = 255;
            }
        v = lp_judge(small, w, h, N, LP_GREY);
        CHECK(v.wrong == 0, "%d wrong at %d x %d", v.wrong, w, h);
    }

    TEST("the two palettes are two palettes");
    {
        lp_fill(img, N, 1, LP_COLOUR);
        LpVerdict v = lp_judge(img, N, N, N, LP_GREY);
        CHECK(v.exact < N * N / 2, "%d exact", v.exact);
        CHECK(v.max_err > 0, "max_err %d", v.max_err);
    }

    return report("layout_probe");
}
