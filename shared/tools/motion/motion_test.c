/* motion_test - the finder and the scorer against frames and takes whose
 * answers are known: every square, bar and clock bit painted at a known place
 * from the shared palette, and every metric of a synthetic take worked out by
 * hand. */
#include "motion.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { W = 660, H = 1434 };        /* a 220x478pt frame at 3x */
static const double S = 3.0;
static uint8_t img[W * H * 3];

static void fill(double x0, double y0, double x1, double y1, int32_t r, int32_t g, int32_t b) {
    for (int32_t y = (int32_t)(y0 * S); y < (int32_t)(y1 * S); y++)
        for (int32_t x = (int32_t)(x0 * S); x < (int32_t)(x1 * S); x++) {
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            uint8_t *p = img + 3 * ((size_t)y * W + x);
            p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
        }
}

/* In the palette's own colour: 0, 0.5, 1 -> 0, 128, 255. */
static void ink(int32_t k, double x0, double y0, double x1, double y1) {
    int32_t c[3];
    for (int32_t i = 0; i < 3; i++) c[i] = mr_ink_half(k, i) == 2 ? 255 : mr_ink_half(k, i) * 128;
    fill(x0, y0, x1, y1, c[0], c[1], c[2]);
}

static void square(int32_t k, double cx, double cy) {
    ink(k, cx - MR_SIDE_PT / 2, cy - MR_SIDE_PT / 2, cx + MR_SIDE_PT / 2, cy + MR_SIDE_PT / 2);
}

static void clock_at(double red, int32_t v) {
    for (int32_t i = 0; i < MR_CLOCK_BITS; i++) {
        int32_t on = (v >> (MR_CLOCK_BITS - 1 - i)) & 1;
        double x = 24 + MR_CLOCK_CELL_PT * i, y = red + MR_EDGE_PT;   /* under the bar */
        fill(x, y, x + MR_CLOCK_CELL_PT, y + MR_CLOCK_CELL_PT, on ? 255 : 0, on ? 255 : 0, on ? 255 : 0);
    }
}

static int32_t M(const char *name) { return mt_mark_by_name(name); }

static void test_find(void) {
    memset(img, 0xf0, sizeof img);                          /* paper */
    ink(MR_INK_RED, 0, 100, 220, 104);                      /* red bar rows 300..311 */
    ink(MR_INK_GREEN, 0, 400, 220, 404);
    clock_at(100, 0x2A5B);
    square(MR_INK_MAGENTA, 110, 250);
    square(MR_INK_CYAN, 60, 200); square(MR_INK_CYAN, 160, 200);
    square(MR_INK_CYAN, 60, 300); square(MR_INK_CYAN, 160, 300);
    square(MR_INK_ORANGE, 40, 130);
    square(MR_INK_BLUE, 190, 380);
    square(MR_INK_VIOLET, 120, 330);
    square(MR_INK_PINK, 90, 230);
    /* a lime blob far too big to be a square, and a yellow sliver */
    ink(MR_INK_LIME, 100, 150, 160, 190);
    ink(MR_INK_YELLOW, 150, 120, 200, 122);
    /* a square in the banded strip is the strip, not a mark */
    square(MR_INK_YELLOW, 8, 360);
    MtRow r;
    r.t = 1.25;
    mt_find(img, W, H, S, &r);
    /* half resolution: pixel rows 150..155 of the half image are the red bar,
     * centre 152.5 -> 101.67pt; a 12pt square spans 18 half pixels, centre
     * (start + 8.5) / 1.5 */
    CHECK(fabs(r.red - 101.6667) < 0.01, "red %.3f", r.red);
    CHECK(fabs(r.green - 401.6667) < 0.01, "green %.3f", r.green);
    CHECK(r.clock == 0x2A5B, "clock %d", r.clock);
    double off = 8.5 / 1.5 - 6;                             /* the half grid's bias */
    CHECK(fabs(r.x[M("magenta")] - (110 + off)) < 0.01 && fabs(r.y[M("magenta")] - (250 + off)) < 0.01,
          "magenta %.2f,%.2f", r.x[M("magenta")], r.y[M("magenta")]);
    CHECK(fabs(r.y[M("cyan_tl")] - (200 + off)) < 0.01 && fabs(r.x[M("cyan_tl")] - (60 + off)) < 0.01, "cyan_tl");
    CHECK(fabs(r.x[M("cyan_tr")] - (160 + off)) < 0.01, "cyan_tr");
    CHECK(fabs(r.y[M("cyan_bl")] - (300 + off)) < 0.01, "cyan_bl");
    CHECK(fabs(r.x[M("cyan_br")] - (160 + off)) < 0.01 && fabs(r.y[M("cyan_br")] - (300 + off)) < 0.01, "cyan_br");
    CHECK(r.y[M("cyan")] == MT_NONE, "four cyans are four corners, not one cyan");
    CHECK(fabs(r.y[M("orange")] - (130 + off)) < 0.01, "orange %.2f", r.y[M("orange")]);
    CHECK(fabs(r.y[M("blue")] - (380 + off)) < 0.01, "blue %.2f", r.y[M("blue")]);
    CHECK(fabs(r.y[M("violet")] - (330 + off)) < 0.01, "violet %.2f", r.y[M("violet")]);
    CHECK(fabs(r.y[M("pink")] - (230 + off)) < 0.01, "pink %.2f", r.y[M("pink")]);
    CHECK(r.y[M("lime")] == MT_NONE, "a big blob is not a square");
    CHECK(r.y[M("yellow")] == MT_NONE, "a sliver and the strip are not squares");
    /* a square scaled 1.4x (a rider through an auto-collapse) is still one */
    memset(img, 0xf0, sizeof img);
    ink(MR_INK_MAGENTA, 100, 100, 100 + 1.4 * MR_SIDE_PT, 100 + 1.4 * MR_SIDE_PT);
    mt_find(img, W, H, S, &r);
    CHECK(r.y[M("magenta")] != MT_NONE, "a 1.4x square");
    CHECK(r.red == MT_NONE && r.clock == -1, "no bar, no clock");
    /* a 1.7x one fits the box but not the area: a filled patch, not a mark */
    memset(img, 0xf0, sizeof img);
    ink(MR_INK_MAGENTA, 100, 100, 100 + 1.7 * MR_SIDE_PT, 100 + 1.7 * MR_SIDE_PT);
    mt_find(img, W, H, S, &r);
    CHECK(r.y[M("magenta")] == MT_NONE, "a 1.7x patch is not a square");
    /* paper, grey ink and a half-saturated red are no ink */
    memset(img, 0xf0, sizeof img);
    fill(0, 100, 220, 104, 255, 140, 140);
    mt_find(img, W, H, S, &r);
    CHECK(r.red == MT_NONE, "a pale red is not the bar");
    /* a bar the frame's edge cuts is not read: its rows in the frame are
     * not its centre; one whole bar a row inside the edge still is */
    memset(img, 0xf0, sizeof img);
    ink(MR_INK_RED, 0, 100, 220, 104);
    ink(MR_INK_GREEN, 0, H / S - 2, 220, H / S);
    mt_find(img, W, H, S, &r);
    CHECK(r.green == MT_NONE, "a green bar cut by the bottom edge (%.2f)", r.green);
    CHECK(fabs(r.red - 101.6667) < 0.01, "the red bar still reads (%.3f)", r.red);
    memset(img, 0xf0, sizeof img);
    ink(MR_INK_RED, 0, 0, 220, 2);
    ink(MR_INK_GREEN, 0, H / S - 5, 220, H / S - 1);
    mt_find(img, W, H, S, &r);
    CHECK(r.red == MT_NONE, "a red bar cut by the top edge (%.2f)", r.red);
    CHECK(r.green != MT_NONE, "a whole green bar just inside the bottom edge reads");
}

/* A synthetic take: the drawer collapses from 840 to 290 on the host spring,
 * `n` frames at 60Hz, each mark placed as a function of the drawer. */
static void take(MtRow *rows, int32_t n, double jump_at, double jump) {
    for (int32_t i = 0; i < n; i++) {
        double t = i / 60.0, p = mt_host_progress(t - 0.1, 0.338);
        MtRow *r = &rows[i];
        memset(r, 0, sizeof *r);
        for (int32_t m = 0; m < MT_MARKS; m++) r->x[m] = r->y[m] = MT_NONE;
        r->t = t; r->clock = -1;
        r->green = 920;
        r->red = 80 + (840 - 290) * p;
        double mid = (r->red + r->green) / 2;
        r->y[M("magenta")] = mid + (t >= jump_at ? jump : 0);       /* the board's centre */
        r->y[M("orange")] = r->red + 40;                              /* the header         */
        r->y[M("blue")] = r->green - 30;                              /* a door             */
    }
}

static void test_score(void) {
    enum { N = 90 };
    static MtRow rows[N];
    MtScoreOpts o;
    mt_default_opts(&o);
    MtScore s[MT_MARKS];

    take(rows, N, 99, 0);
    CHECK(mt_score(rows, N, &o, s), "a take that moves scores");
    for (const char *const *m = (const char *const[]){"magenta", "orange", "blue", NULL}; *m; m++) {
        MtScore *x = &s[M(*m)];
        CHECK(x->seen && x->snaps == 0 && x->maxstep < 1e-9, "%s rides its anchor (step %.3f)", *m, x->maxstep);
        CHECK(x->miss == 0, "%s never missing", *m);
    }
    /* the header rides the host spring exactly: its jerk is the floor, but
     * for the frame or two before the first move is seen (bars.py anchors
     * the floor there, not at the spring's real start) */
    MtScore *h = &s[M("orange")];
    CHECK(fabs(h->jerk / h->floor - 1) < 0.1, "header jerk/floor %.3f", h->jerk / h->floor);
    CHECK(s[M("blue")].jerk == 0 && s[M("blue")].travel == 0, "the door never moves");
    CHECK(s[M("magenta")].stray > 0, "a spring is not a straight line");

    /* one 10pt teleport at 0.3s: one snap of 10, while the drawer still moves */
    take(rows, N, 0.3, 10);
    mt_score(rows, N, &o, s);
    MtScore *b = &s[M("magenta")];
    CHECK(b->snaps == 1 && fabs(b->maxsnap - 10) < 1e-6 && fabs(b->maxstep - 10) < 1e-6,
          "a 10pt jump: snaps %d max %.3f", b->snaps, b->maxsnap);
    CHECK(b->late == 0, "while the drawer moves it is not late");
    /* the same jump after the drawer has stopped is late */
    take(rows, N, 1.2, 10);
    o.span = 1.4;
    mt_score(rows, N, &o, s);
    CHECK(s[M("magenta")].late == 1, "a jump after the drawer stopped is late (%d)", s[M("magenta")].late);
    o.span = 1.2;
    /* a 3pt jump is under the snap: a step, not a snap */
    take(rows, N, 0.3, 3);
    mt_score(rows, N, &o, s);
    CHECK(s[M("magenta")].snaps == 0 && fabs(s[M("magenta")].maxstep - 3) < 1e-6, "3pt is a step");
    /* a frame missing inside the sightings is a miss, and the diff across it
     * is not taken */
    take(rows, N, 99, 0);
    rows[20].y[M("magenta")] = MT_NONE;
    mt_score(rows, N, &o, s);
    CHECK(s[M("magenta")].miss == 1 && s[M("magenta")].snaps == 0, "a missing frame is a miss, not a snap");
    /* nothing moves: no score, unless the whole take is asked for */
    for (int32_t i = 0; i < N; i++) rows[i].red = 80;
    CHECK(!mt_score(rows, N, &o, s), "a still take has no first move");
    o.whole = 1;
    CHECK(mt_score(rows, N, &o, s), "the whole of a still take");

    /* A CORNER SCALES WITH THE BOARD: side(h) = h / 2 here, and a corner
     * sits a quarter of the side above the centre. Rigid, it steps every
     * frame; scored against the side it rides. */
    static double side[1001];
    for (int32_t h2 = 0; h2 <= 1000; h2++) side[h2] = h2 / 2.0;
    take(rows, N, 99, 0);
    for (int32_t i = 0; i < N; i++)
        rows[i].y[M("cyan_tl")] = (rows[i].red + rows[i].green) / 2 - (rows[i].green - rows[i].red + 7) / 8;
    o.whole = 0;
    mt_score(rows, N, &o, s);
    CHECK(s[M("cyan_tl")].snaps > 0, "a scaling corner scored rigid steps (%d)", s[M("cyan_tl")].snaps);
    o.side = side; o.side_h0 = 0; o.side_h1 = 1000;
    mt_score(rows, N, &o, s);
    CHECK(s[M("cyan_tl")].maxstep < 0.01, "against the side it rides (step %.3f)", s[M("cyan_tl")].maxstep);
    o.side = NULL;

    /* THE TABLE round-trips */
    take(rows, N, 99, 0);
    const char *p = "build/motion_test.tbl";
    FILE *f = fopen(p, "w");
    mt_write_header(f);
    for (int32_t i = 0; i < N; i++) mt_write_row(f, &rows[i]);
    fclose(f);
    static MtRow back[N];
    int32_t n = mt_read_table(p, back, N);
    CHECK(n == N, "read %d rows", n);
    CHECK(fabs(back[37].red - rows[37].red) < 0.006 && back[37].y[M("cyan_tl")] == MT_NONE
          && fabs(back[37].y[M("orange")] - rows[37].y[M("orange")]) < 0.006, "a row reads back");
}

/* THE BOARD'S SIZE: four corner squares on a board whose side is half the
 * drawer (side(h) = h / 2), centred on the drawer, the squares 6pt inside. */
static void board_take(MtRow *rows, int32_t n, double jump_at, double jump, double wobble_at) {
    take(rows, n, 99, 0);
    for (int32_t i = 0; i < n; i++) {
        MtRow *r = &rows[i];
        double h = r->green - r->red, side = h / 2 + (r->t >= jump_at ? jump : 0);
        if (r->t >= wobble_at && r->t < wobble_at + 0.05) side += 3;     /* grows back 3pt for 3 frames */
        double cx = 110, cy = (r->red + r->green) / 2, e = side / 2 - 6;
        r->x[M("cyan_tl")] = cx - e; r->y[M("cyan_tl")] = cy - e;
        r->x[M("cyan_tr")] = cx + e; r->y[M("cyan_tr")] = cy - e;
        r->x[M("cyan_bl")] = cx - e; r->y[M("cyan_bl")] = cy + e;
        r->x[M("cyan_br")] = cx + e; r->y[M("cyan_br")] = cy + e;
    }
}

static void test_board(void) {
    enum { N = 90 };
    static MtRow rows[N];
    static double side[1001];
    for (int32_t h2 = 0; h2 <= 1000; h2++) side[h2] = h2 / 2.0;
    MtScoreOpts o;
    mt_default_opts(&o);
    MtBoard b;

    /* reversals, with the hysteresis */
    double v1[] = {0, 5, 10, 9.5, 12}, v2[] = {0, 5, 10, 8, 12}, v3[] = {0, MT_NONE, 5, 3, MT_NONE, 1};
    CHECK(mt_reversals(v1, 5, MT_REV_TOL) == 0, "a half-point dip is not a reversal");
    CHECK(mt_reversals(v2, 5, MT_REV_TOL) == 2, "down 2 and up again is two (%d)", mt_reversals(v2, 5, MT_REV_TOL));
    CHECK(mt_reversals(v3, 6, MT_REV_TOL) == 1, "gaps are skipped (%d)", mt_reversals(v3, 6, MT_REV_TOL));

    /* the size follows the drawer: it shrinks the whole way, once */
    board_take(rows, N, 99, 0, 99);
    CHECK(fabs(mt_board_w(&rows[0]) - (rows[0].green - rows[0].red) / 2 + 12) < 1e-9, "width from the corners %.3f",
          mt_board_w(&rows[0]));
    CHECK(fabs(mt_board_h(&rows[50]) - mt_board_w(&rows[50])) < 1e-9, "a square board");
    rows[30].x[M("cyan_tl")] = rows[30].y[M("cyan_tl")] = MT_NONE;   /* one corner hidden: the other pair */
    CHECK(fabs(mt_board_w(&rows[30]) - (rows[30].x[M("cyan_br")] - rows[30].x[M("cyan_bl")])) < 1e-9,
          "one pair is enough");
    CHECK(mt_board(rows, N, &o, &b), "a take with a board scores");
    CHECK(b.w_rev == 0 && b.h_rev == 0 && b.drawer_rev == 0, "no reversals (%d %d %d)", b.w_rev, b.h_rev, b.drawer_rev);
    double follow = b.w_maxstep;
    CHECK(follow > 4 && follow < 40, "a spring's step is the drawer's half (%.2f)", follow);
    o.side = side; o.side_h0 = 0; o.side_h1 = 1000;
    mt_board(rows, N, &o, &b);
    CHECK(b.w_res_step < 0.01 && b.h_res_step < 0.01 && b.w_res_max < 0.01,
          "against the side it follows (%.3f %.3f)", b.w_res_step, b.w_res_max);

    /* a 10pt jump of the size while the drawer moves: a step the side did
     * not ask for */
    board_take(rows, N, 0.3, 10, 99);
    mt_board(rows, N, &o, &b);
    CHECK(fabs(b.w_res_step - 10) < 0.5 && fabs(b.h_res_step - 10) < 0.5, "a 10pt jump (%.3f %.3f)",
          b.w_res_step, b.h_res_step);

    /* a wobble: the size grows back 3pt for three frames as the drawer
     * settles, while it only shrinks - two reversals the drawer did not make */
    board_take(rows, N, 99, 0, 1.0);
    mt_board(rows, N, &o, &b);
    CHECK(b.w_rev == 2 && b.h_rev == 2 && b.drawer_rev == 0, "a wobble reverses twice (%d %d %d)",
          b.w_rev, b.h_rev, b.drawer_rev);
    CHECK(b.sq_rev == 2 && b.sq_drawer_rev == 0 && b.sq_skipped == 0,
          "a square wobble is ours: two on the square frames (%d %d, %d left out)",
          b.sq_rev, b.sq_drawer_rev, b.sq_skipped);
    /* THE HOST STRETCHING the layer it holds: three frames 3pt narrower and
     * 3pt taller, a square laid out - its width reverses twice, but no
     * square frame does */
    board_take(rows, N, 99, 0, 99);
    for (int32_t i = 30; i < 33; i++) {
        rows[i].x[M("cyan_tl")] += 1.5; rows[i].x[M("cyan_bl")] += 1.5;
        rows[i].x[M("cyan_tr")] -= 1.5; rows[i].x[M("cyan_br")] -= 1.5;
        rows[i].y[M("cyan_tl")] -= 1.5; rows[i].y[M("cyan_tr")] -= 1.5;
        rows[i].y[M("cyan_bl")] += 1.5; rows[i].y[M("cyan_br")] += 1.5;
    }
    mt_board(rows, N, &o, &b);
    CHECK(b.h_rev == 2 && b.w_rev >= 1, "the stretch reverses the height and the width (%d %d)", b.h_rev, b.w_rev);
    CHECK(b.sq_rev == 0 && b.sq_skipped == 3, "no square frame reverses (%d), three left out (%d)",
          b.sq_rev, b.sq_skipped);
    o.side = NULL;

    /* OFF THE DRAWER: a mark below the green bar for five frames */
    MtScore s[MT_MARKS];
    take(rows, N, 99, 0);
    for (int32_t i = 40; i < 45; i++) rows[i].y[M("blue")] = rows[i].green + 10;
    mt_score(rows, N, &o, s);
    CHECK(s[M("blue")].off == 5 && s[M("orange")].off == 0, "five frames off (%d)", s[M("blue")].off);
    /* the bottom that the content painted is not the drawer's */
    take(rows, N, 99, 0);
    for (int32_t i = 0; i < N; i++) { rows[i].green -= 300; rows[i].y[M("blue")] -= 300; }   /* all jumped up */
    mt_score(rows, N, &o, s);
    CHECK(s[M("blue")].maxstep < 1e-9, "riding the painted bar looks still");
    for (int32_t i = 45; i < N; i++) { rows[i].green += 300; rows[i].y[M("blue")] += 300; }  /* back at 0.75s */
    mt_fix_bottom(rows, N, 920, 0);
    CHECK(rows[10].green == 920 && rows[60].green == 920, "the bottom is the screen's");
    mt_score(rows, N, &o, s);
    CHECK(fabs(s[M("blue")].maxsnap - 300) < 1e-6, "a door that jumped 300pt, against the real bottom (%.2f)",
          s[M("blue")].maxsnap);
    /* PAST COMPACT the card slides whole: its top 100pt below where a
     * 290pt drawer's is, the door riding the card - not off the drawer */
    take(rows, N, 99, 0);
    for (int32_t i = 60; i < 70; i++) { rows[i].red += 100; rows[i].y[M("blue")] += 100; rows[i].green += 100; }
    mt_fix_bottom(rows, N, 920, 0);
    mt_score(rows, N, &o, s);
    CHECK(s[M("blue")].off == 10, "without the compact height the sliding card looks off (%d)", s[M("blue")].off);
    take(rows, N, 99, 0);
    for (int32_t i = 60; i < 70; i++) { rows[i].red += 100; rows[i].y[M("blue")] += 100; rows[i].green += 100; }
    mt_fix_bottom(rows, N, 920, 290);
    CHECK(fabs(rows[65].green - (rows[65].red + 290)) < 1e-9 && rows[20].green == 920, "the card's bottom past compact");
    mt_score(rows, N, &o, s);
    CHECK(s[M("blue")].off == 0, "the door rides the sliding card (%d off)", s[M("blue")].off);
}

/* GRID: a device frame with no ruler - a dark wallpaper above the drawer, the
 * paper from 100pt, the four heavy lines, a coloured stroke as long as a line
 * (a win line) and the recording's dark last rows, which are not the board. */
static void test_grid(void) {
    fill(0, 0, W / S, H / S, 30, 32, 40);                  /* wallpaper       */
    fill(0, 100, W / S, H / S, 244, 241, 238);             /* the drawer      */
    fill(0, H / S - 4, W / S, H / S, 0, 0, 0);             /* the last rows   */
    /* a board of side 150 centred at (110, 260): lines at 1/3 and 2/3 */
    double x0 = 35, y0 = 185, sd = 150;
    for (int32_t k = 1; k <= 2; k++) {
        fill(x0, y0 + sd * k / 3 - 1, x0 + sd, y0 + sd * k / 3 + 1, 40, 40, 44);
        fill(x0 + sd * k / 3 - 1, y0, x0 + sd * k / 3 + 1, y0 + sd, 40, 40, 44);
    }
    fill(x0, y0 + 20, x0 + sd, y0 + 24, 180, 50, 40);      /* a red win line  */
    fill(x0, y0 + 120, x0 + sd, y0 + 123, 30, 40, 100);    /* a navy stroke, dark and as long: only its colour tells */
    MtGrid g;
    mt_grid(img, W, H, S, &g);
    CHECK(fabs(g.top - 100) < 0.5, "grid: the drawer's top (%.2f)", g.top);
    CHECK(fabs(g.h1 - (y0 + sd / 3)) < 0.5 && fabs(g.h2 - (y0 + 2 * sd / 3)) < 0.5, "grid: the heavy rows (%.2f %.2f)",
          g.h1, g.h2);
    CHECK(fabs(g.v1 - (x0 + sd / 3)) < 0.5 && fabs(g.v2 - (x0 + 2 * sd / 3)) < 0.5, "grid: the heavy columns (%.2f %.2f)",
          g.v1, g.v2);
    double cx, cy, side;
    CHECK(mt_grid_board(&g, &cx, &cy, &side) && fabs(cx - 110) < 0.5 && fabs(cy - 260) < 0.5 && fabs(side - sd) < 1,
          "grid: the board (%.2f %.2f %.2f)", cx, cy, side);
    /* a thin white highlight across the wallpaper is not the drawer's top */
    fill(0, 60, W / S, 62, 250, 250, 250);
    mt_grid(img, W, H, S, &g);
    CHECK(fabs(g.top - 100) < 0.5, "grid: a highlight is not the drawer (%.2f)", g.top);
}

/* PACE: a stroke laid in ten 60 Hz frames against the same stroke in three
 * frames 120 ms apart - what the owner saw as "choppy". A ruler square in
 * the box is not ink. */
static uint8_t before[W * H * 3];
static void pace_take(int32_t steps, double dt, double *t, int32_t *inkn, int32_t *chg, int32_t *n) {
    MtBox b = { 30, 30, 300, 60 };            /* pixels: 100x20 pt at 3x      */
    fill(0, 0, W / S, H / S, 240, 238, 230);  /* paper                        */
    square(MR_INK_VIOLET, 60, 20);            /* a ruler square in the box    */
    *n = 0;
    for (int32_t k = 0; k < 3; k++) {         /* the still frames before      */
        /* the codec's shimmer: one level up and down, which is not a change */
        for (int32_t x = 40; x < 60; x++) img[3 * (40 * W + x)] = (uint8_t)(240 + (k & 1));
        /* and a codec block edge: a few pixels jump, which is not a change */
        for (int32_t x = 70; x < 73; x++) img[3 * (50 * W + x)] = (uint8_t)(k == 2 ? 180 : 240);
        t[*n] = k * dt; inkn[*n] = mt_box_ink(img, W, H, b, 150);
        chg[*n] = k > 0 && mt_box_diff(before, img, W, H, b, MT_PACE_TOL) >= MT_PACE_MIN;
        memcpy(before, img, sizeof img); (*n)++;
    }
    for (int32_t s = 1; s <= steps; s++) {
        fill(10, 12, 10 + 100.0 * s / steps, 28, 40, 40, 60);   /* the stroke grows */
        t[*n] = (2 + s) * dt; inkn[*n] = mt_box_ink(img, W, H, b, 150);
        chg[*n] = mt_box_diff(before, img, W, H, b, MT_PACE_TOL) >= MT_PACE_MIN;
        memcpy(before, img, sizeof img); (*n)++;
    }
}

static void test_pace(void) {
    double t[64]; int32_t inkn[64], chg[64]; int32_t n;
    MtPace smooth, choppy;
    pace_take(10, 1.0 / 60, t, inkn, chg, &n);
    CHECK(inkn[0] == 0, "pace: a ruler square is not ink (%d)", inkn[0]);
    CHECK(!chg[1] && !chg[2], "pace: the codec's shimmer is not a change");
    mt_pace(t, inkn, chg, n, &smooth);
    CHECK(smooth.frames == 10, "pace: ten changed frames (%d)", smooth.frames);
    CHECK(fabs(smooth.fps - 60) < 1, "pace: 60 fps (%.2f)", smooth.fps);
    CHECK(fabs(smooth.maxgap - 1.0 / 60) < 1e-6, "pace: gap one frame (%.4f)", smooth.maxgap);
    CHECK(fabs(smooth.maxstep - .1) < .02, "pace: a tenth a frame (%.3f)", smooth.maxstep);
    pace_take(3, .120, t, inkn, chg, &n);
    mt_pace(t, inkn, chg, n, &choppy);
    CHECK(choppy.frames == 3 && fabs(choppy.fps - 1 / .120) < .1, "pace: 3 frames 120 ms apart (%d, %.2f)",
          choppy.frames, choppy.fps);
    CHECK(fabs(choppy.maxgap - .120) < 1e-6 && choppy.maxstep > .3, "pace: the gap and the step (%.3f %.3f)",
          choppy.maxgap, choppy.maxstep);
    CHECK(choppy.rough > 3 * smooth.rough, "pace: judder is worse when choppy (%.2f vs %.2f)",
          choppy.rough, smooth.rough);
}

int main(void) {
    test_find();
    test_score();
    test_board();
    test_grid();
    test_pace();
    printf("motion: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
