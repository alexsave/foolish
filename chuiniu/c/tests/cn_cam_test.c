/* The camera (src/cn_cam.c): pinned to the study's own numbers, read out of
 * chuiniu/docs/UI.html in headless Chromium by tools/dump_study_layout.mjs
 * (tools/study_layout.txt): the turn's angle and distance, the study's
 * toScreen and fromScreen; then the CATransform3D a host applies is the CSS
 * transform the study paints, the eye's projection keeps the table 1:1, and
 * the peek's tip and tween are the study's. */
#include "../src/cn_cam.h"
#include "../src/cn_geom.h"
#include "cn_check.h"
#include <math.h>

typedef struct { int W, H, mine; double boardH, ox, oy, theta, D, zoom; double ts[4][3]; double fs[3][2]; } GoldCam;
/* W H mine boardH origin x y | theta D zoom | toScreen dy -> y scale x4 | fromScreen y -> dy x3: one per distinct camera.
 * 390 by 340 on their turn is the study's camera before package U, when the short board grew to 320 on their turn; the
 * board no longer does (cn_lay.h), but this is still the camera over a 358 by 320 board, so it stays pinned. */
static const GoldCam GOLD_CAM[] = {
    { 390, 340, 1, 220, 195, 194, 0.148491, 570.8949, 1.06,
      { { -400, -185.949, 0.960442 }, { -150, 42.6336, 1.020337 }, { -40, 152.4968, 1.049125 }, { 30, 225.6965, 1.068305 } },
      { { 0, -194.377 }, { 70, -122.0231 }, { 250, 52.6887 } } },
    { 390, 340, 0, 320, 195, 242, 0.220972, 593.9731, 1.06,
      { { -400, -118.4827, 0.923666 }, { -150, 95.0025, 1.004406 }, { -40, 201.2327, 1.044582 }, { 30, 273.3741, 1.071866 } },
      { { 0, -256.1044 }, { 70, -177.1812 }, { 250, 7.7132 } } },
    { 390, 718, 1, 576, 195, 520, 0.354210, 619.793, 1.06,
      { { -400, 195.059, 0.866120 }, { -150, 382.4195, 0.977911 }, { -40, 481.1029, 1.036792 }, { 30, 550.3352, 1.078100 } },
      { { 0, -739.4836 }, { 70, -606.1693 }, { 250, -320.2472 } } },
    { 390, 718, 0, 676, 195, 620, 0.403985, 635.5788, 1.06,
      { { -400, 307.4519, 0.849776 }, { -150, 486.2109, 0.970012 }, { -40, 581.9543, 1.034410 }, { 30, 649.7929, 1.080039 } },
      { { 0, -1048.6777 }, { 70, -866.8057 }, { 250, -496.0862 } } },
    { 375, 541, 1, 399, 187.5, 343, 0.260137, 595.8803, 1.06,
      { { -400, -6.4058, 0.903927 }, { -150, 198.6931, 0.995541 }, { -40, 302.722, 1.042009 }, { 30, 374.1332, 1.073907 } },
      { { 0, -391.4275 }, { 70, -301.1599 }, { 250, -94.4937 } } },
    { 375, 541, 0, 499, 187.5, 443, 0.314197, 608.7313, 1.06,
      { { -400, 107.8246, 0.881072 }, { -150, 302.4848, 0.984988 }, { -40, 403.4783, 1.038902 }, { 30, 473.711, 1.076395 } },
      { { 0, -565.6311 }, { 70, -455.5809 }, { 250, -212.0597 } } },
    { 430, 830, 1, 688, 215, 632, 0.409790, 637.5759, 1.06,
      { { -400, 320.8735, 0.848029 }, { -150, 498.6627, 0.969157 }, { -40, 594.0589, 1.034151 }, { 30, 661.7243, 1.080251 } },
      { { 0, -1094.742 }, { 70, -904.9228 }, { 250, -520.7708 } } },
    { 430, 830, 0, 788, 215, 732, 0.456738, 655.0298, 1.06,
      { { -400, 432.2018, 0.835096 }, { -150, 602.3881, 0.962767 }, { -40, 694.9441, 1.032201 }, { 30, 761.1287, 1.081852 } },
      { { 0, -1596.512 }, { 70, -1309.2587 }, { 250, -768.9592 } } },
    { 390, 584, 1, 442, 195, 386, 0.283665, 601.1902, 1.06,
      { { -400, 42.8452, 0.893599 }, { -150, 243.3178, 0.990811 }, { -40, 346.0386, 1.040622 }, { 30, 416.9616, 1.075014 } },
      { { 0, -460.6529 }, { 70, -362.9978 }, { 250, -142.5091 } } },
    { 390, 584, 0, 542, 195, 486, 0.336720, 614.7875, 1.06,
      { { -400, 156.616, 0.872454 }, { -150, 347.1239, 0.980926 }, { -40, 446.8232, 1.037693 }, { 30, 516.5061, 1.077370 } },
      { { 0, -657.3848 }, { 70, -535.4534 }, { 250, -270.1325 } } },
};

static int near(double a, double b, double eps) { return fabs(a - b) <= eps; }

static void test_golden(void)
{
    TEST("the camera is the study's: theta, D, zoom, toScreen, fromScreen at every size");
    const int n = (int)(sizeof GOLD_CAM / sizeof GOLD_CAM[0]);
    CHECK(n == 10, "five sizes, my turn and theirs (%d)", n);
    for (int i = 0; i < n; i++) {
        const GoldCam *g = &GOLD_CAM[i];
        CnCam c;
        cn_cam_make(&c, (float)(g->W - 32), (float)g->boardH, (float)g->ox, (float)g->oy, 1);
        CHECK(near(c.theta, g->theta, 2e-6), "%dx%d %s: theta %.6f, the study %.6f", g->W, g->H, g->mine ? "mine" : "theirs", c.theta, g->theta);
        CHECK(near(c.D, g->D, .01), "%dx%d: D %.4f, the study %.4f", g->W, g->H, c.D, g->D);
        CHECK(near(c.zoom, g->zoom, 1e-6), "%dx%d: zoom", g->W, g->H);
        CHECK(near(c.eye_y, g->boardH * 1.35, 1e-3) && near(c.eye_z, 560, 1e-6) && near(c.eye_x, (g->W - 32) / 2.0, 1e-6), "%dx%d: the leaning head's eye", g->W, g->H);
        for (int k = 0; k < 4; k++) {
            float sc, y = cn_cam_to_screen(&c, (float)g->ts[k][0], &sc);
            CHECK(near(y, g->ts[k][1], .01) && near(sc, g->ts[k][2], 2e-6), "%dx%d: toScreen(%g) %.4f x%.6f, the study %.4f x%.6f", g->W, g->H, g->ts[k][0], y, sc, g->ts[k][1], g->ts[k][2]);
        }
        for (int k = 0; k < 3; k++) {
            float dy = cn_cam_from_screen(&c, (float)g->fs[k][0]);
            CHECK(near(dy, g->fs[k][1], .01), "%dx%d: fromScreen(%g) %.4f, the study %.4f", g->W, g->H, g->fs[k][0], dy, g->fs[k][1]);
        }
    }
}

/* the CSS transform-origin: o; transform: perspective(D) rotateX(theta) scale(zoom), by hand */
static void css(const CnCam *c, double x, double y, double *sx, double *sy)
{
    const double dx = (x - c->origin_x) * c->zoom, dy = (y - c->origin_y) * c->zoom;
    const double Y = dy * cos(c->theta), Z = dy * sin(c->theta), w = 1 - Z / c->D;
    *sx = c->origin_x + dx / w; *sy = c->origin_y + Y / w;
}

static void test_transform(void)
{
    TEST("the host's transform is the study's CSS: CATransform3D, the screen's, the homography");
    CnCam c;
    cn_cam_make(&c, 358, 576, 195, 520, 1);
    double worst = 0;
    for (double x = 0; x <= 390; x += 39) for (double y = 0; y <= 718; y += 47.86) {
        double ex, ey; float mx, my;
        css(&c, x, y, &ex, &ey);
        cn_cam_map(&c, (float)x, (float)y, &mx, &my);
        double e = fabs(mx - ex) + fabs(my - ey);
        if (e > worst) worst = e;
        /* the CA matrix about the origin, applied as a row vector, then the screen's origin added back */
        const float *m = c.ca;
        const double px = x - c.origin_x, py = y - c.origin_y;
        const double X = px * m[0] + py * m[4] + m[12], Y = px * m[1] + py * m[5] + m[13], W = px * m[3] + py * m[7] + m[15];
        CHECK(near(c.origin_x + X / W, ex, 1e-3) && near(c.origin_y + Y / W, ey, 1e-3), "ca about the origin at (%g, %g)", x, y);
        const float *f = c.ca_screen;
        const double FX = x * f[0] + y * f[4] + f[12], FY = x * f[1] + y * f[5] + f[13], FW = x * f[3] + y * f[7] + f[15];
        CHECK(near(FX / FW, ex, 1e-3) && near(FY / FW, ey, 1e-3), "ca_screen at (%g, %g)", x, y);
    }
    CHECK(worst < 1e-3, "the homography is the CSS map everywhere on the screen (worst %g)", worst);
    float ox, oy;
    cn_cam_map(&c, 195, 520, &ox, &oy);
    CHECK(near(ox, 195, 1e-4) && near(oy, 520, 1e-4), "the origin stays put");
    CHECK(near(c.ca[11], -1 / c.D * cos(c.theta), 1e-9) && c.ca[0] == c.zoom && near(c.ca[6], c.zoom * sin(c.theta), 1e-7), "m34 carries -1/D through the turn; m11 the zoom");
    /* the study's toScreen puts the zoom outside the perspective divide, the painted transform inside it;
     * 400 points above my cup on a 390 by 718 screen they part by three and a half points (cn_cam.h) */
    double ex, ey; float sc;
    css(&c, 195, 120, &ex, &ey);
    const float ys = cn_cam_to_screen(&c, -400, &sc);
    CHECK(fabs(ys - ey) > 3 && fabs(ys - ey) < 4, "the two maps part by %.2f points at 400 above", fabs(ys - ey));
    /* fromScreen undoes toScreen */
    for (float dy = -500; dy <= 100; dy += 37) CHECK(near(cn_cam_from_screen(&c, cn_cam_to_screen(&c, dy, 0)), dy, 1e-3), "fromScreen(toScreen(%g))", dy);
}

static void test_project(void)
{
    TEST("the eye: the table is drawn 1:1, an upright thing leans away from the eye's foot");
    CnCam c;
    cn_cam_make(&c, 358, 576, 195, 520, 1);
    float px, py;
    cn_cam_project(&c, 40, 100, 0, &px, &py);
    CHECK(px == 40 && py == 100, "a point on the table stays where the layout put it");
    cn_cam_project(&c, 40, 100, 280, &px, &py);   /* half way to the eye: twice as far from its foot */
    CHECK(near(px, c.eye_x + (40 - c.eye_x) * 2, 1e-3) && near(py, c.eye_y + (100 - c.eye_y) * 2, 1e-3), "half way up, twice as far out (%g, %g)", px, py);
    CHECK(py < 100, "a far cup's crown leans up the screen");
}

static void test_peek(void)
{
    TEST("the peek: the hinge, the hand's draw, the least tip that shows every die, the tween");
    CnPeek p = cn_cam_peek_tilt(58.9f, .5f, 1.0f);
    CHECK(p.angle == .5f && p.hinge_y == -58.9f && near(p.back, 58.9 * .2 * .5, 1e-5) && near(p.lift, 58.9 * .12 * .5, 1e-5), "half the tip, half the draw");
    p = cn_cam_peek_tilt(40, 0, 0);
    CHECK(p.angle == 0 && p.back == 0 && p.lift == 0, "shut");
    /* the tip found for the 390 by 718 board: the near rim's shadow clears the farthest die by 4 at
     * the tip less the extra, and does not a degree before */
    CnCam c;
    cn_cam_make(&c, 358, 576, 195, 520, 1);
    const float R = 58.906553f, mcy = 490, dy[2] = { 455, 470 }, dd[2] = { 24, 24 };
    float a = cn_cam_peek_angle(&c, R, mcy, dy, dd, 2);
    const double base = a - CN_PEEK_EXTRA_DEG * CN_PI / 180, far = 455 - 24 * .6 - 4;
    CHECK(near(base * 180 / CN_PI, floor(base * 180 / CN_PI + .5), 1e-4), "whole degrees (%g)", base * 180 / CN_PI);
    for (int k = 0; k < 2; k++) {
        const double t = base - k * CN_PI / 180, yr = mcy - R - .2 * R + 2 * R * cos(t), zr = 2 * R * sin(t) + .12 * R;
        const double yp = c.eye_y + (yr - c.eye_y) * c.eye_z / (c.eye_z - zr);
        CHECK(k == 0 ? yp <= far : yp > far, "at %g degrees the rim's shadow is at %g, the far die's edge %g", t * 180 / CN_PI, yp, far);
    }
    float none = cn_cam_peek_angle(&c, R, mcy, dy, dd, 0);
    CHECK(near(none, 10 * CN_PI / 180 + CN_PEEK_EXTRA_DEG * CN_PI / 180, 1e-6), "no dice: the least tip");
    CHECK(cn_cam_peek_ease(0) == 0 && near(cn_cam_peek_ease(1), 1, 1e-7), "the tween starts at 0 and lands on 1");
    float last = 0; int mono = 1;
    for (int i = 1; i <= 100; i++) { float e = cn_cam_peek_ease(i / 100.f); if (e < last) mono = 0; last = e; }
    CHECK(mono, "and never goes back");
    CHECK(near(cn_cam_peek_ease(.5f), 1 - .125 * (1 - .12), 1e-6), "half way: 1 - (1/2)^3 (1 - .12)");
}

int main(void)
{
    test_golden();
    test_transform();
    test_project();
    test_peek();
    return report("cn_cam_test");
}
