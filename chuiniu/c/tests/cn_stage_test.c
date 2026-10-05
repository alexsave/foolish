/* The stage (src/cn_stage.c): the table and a clock in, pixels and the HUD out.
 *
 * Run with the texture pack's path (make tex writes build/cn_tex.pack):
 *     ./build/cn_stage_test build/cn_tex.pack
 * Each test is mutation-checked; tests/MUTATIONS.md and docs_pkgD.md list
 * which mutation turned which assertion red. */
#include "../src/cn_stage.h"
#include "../src/cn_beats.h"
#include "../src/cn_scene.h"
#include "cn_check.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define ARENA ((size_t)48 << 20)

static uint8_t *PACK;
static size_t PACK_N;
static CnStage ST;           /* about 700 KB: static, not on the stack */

static uint32_t fnv(const uint8_t *b, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static void *arena_new(size_t n)
{
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "no memory for the arena\n"); exit(2); }
    return p;
}

/* the study's table: Alex, Bo, Cy, Dee, Eve, Fay; at six seats Fay is out */
static CnStageIn table_in(int W, int H, int n, int mine, int me, int kind)
{
    static const uint8_t COUNTS[6] = { 5, 4, 5, 3, 5, 2 };
    static const uint8_t FACES[6][5] = { { 1, 1, 3, 5, 6 }, { 2, 3, 4, 4 }, { 1, 2, 3, 3, 6 }, { 3, 5, 6 }, { 2, 2, 3, 4, 5 }, { 1, 3 } };
    CnStageIn in;
    memset(&in, 0, sizeof in);
    in.kind = (uint8_t)kind; in.seats = (uint8_t)n; in.me = (uint8_t)me; in.turn = (uint8_t)(mine ? me : (me + 1) % n);
    for (int v = 0; v < n; v++) {
        int s = (me + v) % n;
        in.dice[s] = COUNTS[v];
        memcpy(&in.faces[s * CN_STAGE_DICE], FACES[v], COUNTS[v]);
    }
    if (n == 6) { int s = (me + 5) % n; in.out_mask = (uint8_t)(1 << s); in.dice[s] = 0; memset(&in.faces[s * CN_STAGE_DICE], 0, 5); }
    in.known_mask = kind == CN_STAGE_REVEAL ? (uint8_t)((1 << n) - 1) : (uint8_t)(1 << me);
    in.w = (float)W; in.h = (float)H; in.scale = 1; in.seed = 7; in.roll_at_ms = CN_STAGE_NO_ROLL;
    return in;
}

/* ---- the layout through the stage is the study's ---------------------------------------- */

typedef struct { int W, H, mine; double topM, boardH, R, pad, padX; double seat[5][2], name[5][2]; } Gold;
/* from tests/cn_lay_test.c's GOLD (headless Chromium over docs/UI.html), six seats, Fay out */
static const Gold GOLD[] = {
    { 390, 340, 1, 8, 220, 30.2, 40, 40,
      { { 42.2, 66 }, { 110.6, 66 }, { 179, 66 }, { 247.4, 66 }, { 315.8, 66 } },
      { { 42.2, 110.2 }, { 110.6, 110.2 }, { 179, 110.2 }, { 247.4, 110.2 }, { 315.8, 110.2 } } },
    { 390, 340, 0, 8, 220, 30.2, 40, 40,   /* the same short board on their turn (package U) */
      { { 42.2, 66 }, { 110.6, 66 }, { 179, 66 }, { 247.4, 66 }, { 315.8, 66 } },
      { { 42.2, 110.2 }, { 110.6, 110.2 }, { 179, 110.2 }, { 247.4, 110.2 }, { 315.8, 110.2 } } },
    { 390, 718, 1, 30, 576, 42.9066, 258, 49,
      { { 45.1246, 384.393 }, { 45.1246, 173.179 }, { 179, 67.572 }, { 312.8754, 173.179 }, { 273.7327, 384.393 } },
      { { 45.1246, 441.2996 }, { 45.1246, 230.0856 }, { 179, 124.4786 }, { 312.8754, 230.0856 }, { 273.7327, 441.2996 } } },
    { 390, 718, 0, 30, 676, 50.9066, 467, 63,
      { { 48.2931, 442.1471 }, { 48.2931, 146.4412 }, { 179, -1.4118 }, { 309.7069, 146.4412 }, { 262.1747, 442.1471 } },
      { { 48.2931, 507.0536 }, { 48.2931, 211.3477 }, { 179, 63.4948 }, { 309.7069, 211.3477 }, { 262.1747, 507.0536 } } },
    { 375, 541, 1, 30, 399, 30.9066, 87, 33,
      { { 36.5349, 260.1859 }, { 36.5349, 154.5576 }, { 171.5, 101.7435 }, { 306.4651, 154.5576 }, { 278.4806, 260.1859 } },
      { { 36.5349, 305.0924 }, { 59.7148, 189.7376 }, { 171.5, 146.6501 }, { 245.5585, 154.5576 }, { 278.4806, 305.0924 } } },
    { 375, 541, 0, 30, 499, 38.9066, 161, 43,
      { { 43.1079, 334.152 }, { 43.1079, 176.456 }, { 171.5, 97.608 }, { 299.8921, 176.456 }, { 264.6218, 334.152 } },
      { { 43.1079, 387.0585 }, { 43.1079, 229.3625 }, { 171.5, 150.5145 }, { 270.7122, 217.6359 }, { 264.6218, 387.0585 } } },
    { 430, 830, 1, 30, 688, 52.9066, 501, 63,
      { { 52.2507, 449.3985 }, { 52.2507, 144.1955 }, { 199, -8.406 }, { 345.7493, 144.1955 }, { 297.301, 449.3985 } },
      { { 52.2507, 516.3051 }, { 52.2507, 211.1021 }, { 199, 58.5006 }, { 345.7493, 211.1021 }, { 297.301, 516.3051 } } },
    { 430, 830, 0, 30, 788, 56.9066, 903, 76,   /* the side seats drawn in 2.6 points: the study's crowns left the glass (package V2) */
      { { 47.5488, 524.1192 }, { 47.5488, 168.3576 }, { 199, -9.5232 }, { 350.4512, 168.3576 }, { 293.0299, 524.1192 } },
      { { 47.5488, 595.0257 }, { 47.5488, 239.2641 }, { 199, 61.3833 }, { 350.4512, 239.2641 }, { 293.0299, 595.0257 } } },
};
static int near(double a, double b, double eps) { return fabs(a - b) <= eps; }

static void test_layout(void)
{
    TEST("the layout through the stage is the study's at four sizes, my turn and theirs");
    void *A = arena_new(ARENA);
    CHECK(cn_stage_init(&ST, PACK, PACK_N) == 0 && cn_stage_attach(&ST, A, ARENA) == 0, "the stage opens the pack, then takes the arena");
    for (size_t i = 0; i < sizeof GOLD / sizeof GOLD[0]; i++) {
        const Gold *g = &GOLD[i];
        CnStageIn in = table_in(g->W, g->H, 6, g->mine, 0, CN_STAGE_TABLE);
        const CnStageHud *h = cn_stage_begin(&ST, &in);
        CHECK(h && h->ok, "%dx%d mine %d: begun", g->W, g->H, g->mine);
        if (!h) continue;
        { int w, hh; in.scale = 2; cn_stage_begin(&ST, &in); cn_stage_frame(&ST, 0, 0, 0, &w, &hh);
          printf("  %dx%d mine %d: canvas %.0fx%.0f (pad %.0f of the study's %.0f), still at %.1fx: %dx%d\n", g->W, g->H, g->mine,
                 h->canvas[2], h->canvas[3], h->board[1] - h->canvas[1], g->pad, ST.shot.scale, w, hh); in.scale = 1; h = cn_stage_begin(&ST, &in); }
        const double bx = h->board[0], by = h->board[1];
        CHECK(near(by, g->topM, .01) && near(h->board[3], g->boardH, .01), "%dx%d mine %d: the board %.2f %.2f, the study %.2f %.2f", g->W, g->H, g->mine, by, h->board[3], g->topM, g->boardH);
        CHECK(near(h->cup_r, g->R, .01), "%dx%d mine %d: R %.4f, the study %.4f", g->W, g->H, g->mine, h->cup_r, g->R);
        CHECK(near(h->pad, g->pad, .01) && near(h->pad_x, g->padX, .01), "%dx%d mine %d: pad %.1f padX %.1f, the study %.0f %.0f", g->W, g->H, g->mine, h->pad, h->pad_x, g->pad, g->padX);
        int seats_ok = 1, names_ok = 1;
        for (int s = 1; s < 6; s++) {
            seats_ok &= near(h->cup_x[s] - bx, g->seat[s - 1][0], .01) && near(h->cup_y[s] - by, g->seat[s - 1][1], .01);
            names_ok &= near(h->name_x[s] - bx, g->name[s - 1][0], .01) && near(h->name_y[s] - by, g->name[s - 1][1], .01);
        }
        CHECK(seats_ok, "%dx%d mine %d: every seat where the study puts it", g->W, g->H, g->mine);
        CHECK(names_ok, "%dx%d mine %d: every name where the study puts it", g->W, g->H, g->mine);
        /* the reveal's brass rings: each seat's die side on the glass and its ring, the layout's (package U) */
        int brass_ok = 1;
        for (int s = 0; s < 6; s++) brass_ok &= h->die_d[s] > 0 && h->die_d[s] == ST.lay.die_g[s] && h->brass_r[s] == ST.lay.brass_r[s];
        CHECK(brass_ok && h->die_d[0] > h->die_d[1], "%dx%d mine %d: a die's side and its brass ring at every seat (%.2f, %.2f)", g->W, g->H, g->mine, h->die_d[0], h->brass_r[0]);
        /* the canvas is the board less pad above, pad_x either side, pad_below under it */
        CHECK(near(h->canvas[0], bx - g->padX, .01) && h->canvas[1] >= by - g->pad && h->canvas[1] <= by
              && near(h->canvas[1] + h->canvas[3], by + g->boardH + CN_LAY_PAD_BELOW, .01), "%dx%d mine %d: the canvas (top %.0f above the board, the study %.0f)",
              g->W, g->H, g->mine, by - h->canvas[1], g->pad);
        /* the hit ellipse is round my cup on the glass: its centre maps from near my cup's flat place */
        CHECK(h->hit[2] > h->my_r * .8 && h->hit[3] > h->my_r * .8 && near(h->hit[0], h->origin_x, 1), "%dx%d mine %d: the hit ellipse (%.1f %.1f %.1f %.1f)",
              g->W, g->H, g->mine, h->hit[0], h->hit[1], h->hit[2], h->hit[3]);
        /* ...and on the glass: the crown's top edge, where a finger sees it after the turn, is
         * inside it, and a point a quarter radius past it is not */
        {
            const CnCam *c = &ST.lay.cam;
            float px, py, gx, gy, gx2, gy2;
            const float mx = h->cup_x[0] - (float)bx, my = h->cup_y[0] - (float)by, R = h->my_r;
            cn_cam_project(c, mx, my - R * (float)CN_CUP_RC, R * CN_CUP_TALL, &px, &py);
            cn_cam_map(c, (float)bx + px, (float)by + py, &gx, &gy);
            cn_cam_project(c, mx, my - R * (float)CN_CUP_RC - R * .25f, R * CN_CUP_TALL, &px, &py);
            cn_cam_map(c, (float)bx + px, (float)by + py, &gx2, &gy2);
            const double in1 = pow((gx - h->hit[0]) / h->hit[2], 2) + pow((gy - h->hit[1]) / h->hit[3], 2);
            const double in2 = pow((gx2 - h->hit[0]) / h->hit[2], 2) + pow((gy2 - h->hit[1]) / h->hit[3], 2);
            CHECK(in1 <= 1.02 && in2 > 1, "%dx%d mine %d: the crown's top is on the ellipse's edge on the glass (%.3f, past it %.3f)", g->W, g->H, g->mine, in1, in2);
        }
    }
    cn_stage_purge(&ST);
    free(A);
}

/* ---- a frame, its crowns, its table ------------------------------------------------------ */

/* the picture's pixel under a board point, up z, as the eye sees it */
static const uint8_t *px_at(const CnStage *st, const uint8_t *fb, int w, int h, float x, float y, float z)
{
    float px, py;
    cn_cam_project(&st->lay.cam, x, y, z, &px, &py);
    const float s = st->shot.scale;
    int ix = (int)((px + st->pad_x) * s), iy = (int)((py + st->pad) * s);
    if (ix < 0 || iy < 0 || ix >= w || iy >= h) return 0;
    return fb + ((size_t)iy * w + ix) * 4;
}

static void test_frame(void)
{
    TEST("a still frame draws every standing cup's crown where the HUD puts the cup, and table round them");
    void *A = arena_new(ARENA);
    cn_stage_init(&ST, PACK, PACK_N); cn_stage_attach(&ST, A, ARENA);
    CnStageIn in = table_in(390, 718, 6, 1, 0, CN_STAGE_TABLE);
    const CnStageHud *h = cn_stage_begin(&ST, &in);
    int w, hh;
    const uint8_t *fb = cn_stage_frame(&ST, 0, 0, 0, &w, &hh);
    CHECK(fb && w == 390 - 32 + 2 * 49 && hh == (int)(h->canvas[3] + .5f) && h->canvas[3] < 576 + 258 + 70, "a 1x frame of the canvas (%dx%d)", w, hh);
    if (!fb) { free(A); return; }
    long body = 0;
    for (long i = 0; i < (long)w * hh; i++) body += fb[i * 4 + 3] == 255;
    CHECK(body > 20000, "bodies drawn (%ld opaque pixels)", body);
    for (int s = 0; s < 5; s++) {
        const float x = h->cup_x[s] - h->board[0], y = h->cup_y[s] - h->board[1], R = s ? h->cup_r : h->my_r;
        const uint8_t *c = px_at(&ST, fb, w, hh, x, y, R * CN_CUP_TALL);
        CHECK(c && c[3] == 255, "seat %d: its crown is drawn over the HUD's cup (%d)", s, c ? c[3] : -1);
    }
    /* the table between my cup and the ring: no body, a shadow's alpha at most */
    const uint8_t *t = px_at(&ST, fb, w, hh, h->cup_x[0] - h->board[0] + 110, h->cup_y[0] - h->board[1] - 40, 0);
    CHECK(t && t[3] < 255, "the table beside my cup is not a body (%d)", t ? t[3] : -1);
    cn_stage_purge(&ST);
    free(A);
}

/* ---- the hand ----------------------------------------------------------------------------- */

/* the value on a die's face that points up, read from its pose: the local axis nearest world +z */
static int up_value(const CnObj *o)
{
    int best = 2, sign = 1;
    float bz = -2;
    for (int a = 0; a < 3; a++) {
        float z = o->has_rot ? o->rot[a * 3 + 2] : (a == 2 ? 1.f : 0.f);
        if (z > bz) { bz = z; best = a; sign = 1; }
        if (-z > bz) { bz = -z; best = a; sign = -1; }
    }
    return cn_die_value(o->cells, best, sign);
}

static void test_hand(void)
{
    TEST("the dealt dice are painted on the faces the throw left up, on a standard die (I19)");
    void *A = arena_new(ARENA);
    cn_stage_init(&ST, PACK, PACK_N); cn_stage_attach(&ST, A, ARENA);
    int bad = 0, opp = 0, tried = 0, nonstd = 0;
    for (int seed = 1; seed <= 40; seed++) {
        CnStageIn in = table_in(390, 718, 2 + seed % 5, seed & 1, 0, seed % 3 ? CN_STAGE_TABLE : CN_STAGE_REVEAL);
        in.seed = (uint32_t)(seed * 7919);
        in.roll_at_ms = in.kind == CN_STAGE_TABLE ? 0 : CN_STAGE_NO_ROLL;
        const CnStageHud *h = cn_stage_begin(&ST, &in);
        if (!h) { bad++; continue; }
        int n;
        const CnObj *o = cn_stage_objects(&ST, h->rolls ? h->total_ms : 0, 0, 0, &n);
        int k[6] = { 0 };
        for (int i = 0; i < n; i++) {
            if (o[i].kind != CN_OBJ_DIE) continue;
            const int s = o[i].seat, idx = k[s]++;
            if (!(in.known_mask >> s & 1)) continue;
            tried++;
            if (up_value(&o[i]) != in.faces[s * 5 + idx]) bad++;
            for (int a = 0; a < 3; a++) if (cn_die_value(o[i].cells, a, 1) + cn_die_value(o[i].cells, a, -1) != 7) opp++;
            uint8_t ref[6];
            cn_die_cells(2, 1, 1, ref);
            int seen[7] = { 0 };
            for (int f = 0; f < 6; f++) seen[o[i].cells[f]]++;
            for (int v = 1; v <= 6; v++) nonstd += seen[v] != 1;
        }
    }
    CHECK(tried > 300, "dice checked (%d)", tried);
    CHECK(bad == 0, "every known die at rest shows its dealt value up (%d wrong)", bad);
    CHECK(opp == 0, "opposite faces sum to seven (%d pairs wrong)", opp);
    CHECK(nonstd == 0, "each face 1 to 6 once (%d wrong)", nonstd);
    cn_stage_purge(&ST);
    free(A);
}

/* ---- memory -------------------------------------------------------------------------------- */

static void test_memory(void)
{
    TEST("a small arena fails cleanly, a frame that does not fit steps its scale down, purge and attach rebuild the same bytes");
    /* NO ARENA AT ALL: the pack opens, a table begins and lays out (the HUD is the whole
     * layout), and only a frame needs the arena; what began before one is attached is what
     * begins after */
    CHECK(cn_stage_init(&ST, PACK, PACK_N) == 0 && ST.arena == 0, "the stage opens the pack and takes no arena");
    CnStageIn in = table_in(390, 718, 6, 1, 0, CN_STAGE_TABLE);
    in.scale = 2; in.roll_at_ms = 0;
    const CnStageHud *h0 = cn_stage_begin(&ST, &in);
    CnStageHud bare;
    memset(&bare, 0, sizeof bare);
    if (h0) bare = *h0;
    int w = -1, h = -1;
    CHECK(h0 && h0->ok && h0->rolls && h0->total_ms > 0, "begin with no arena: the layout and the throws (total %u ms)", h0 ? h0->total_ms : 0);
    CHECK(cn_stage_frame(&ST, 0, 0, 0, &w, &h) == 0 && w == 0 && h == 0 && !cn_stage_prepare(&ST, 0, 0, 0), "and no frame");
    {
        void *A0 = arena_new(ARENA);
        CHECK(cn_stage_attach(&ST, A0, ARENA) == 0 && cn_stage_frame(&ST, 0, 0, 0, &w, &h) != 0, "an arena attached: the begun table draws");
        const CnStageHud *h1 = cn_stage_begin(&ST, &in);
        CHECK(h1 && memcmp(h1, &bare, sizeof bare) == 0, "begun with an arena: the very HUD begun without one");
        cn_stage_purge(&ST);
        free(A0);
    }
    in.roll_at_ms = CN_STAGE_NO_ROLL;

    /* an arena that holds no texture set: nothing is drawn, nothing breaks */
    void *small = arena_new((size_t)4 << 20);
    CHECK(cn_stage_attach(&ST, small, (size_t)4 << 20) == 0, "a 4 MB arena is taken");
    CHECK(cn_stage_begin(&ST, &in) != 0, "begin needs no arena");
    CHECK(cn_stage_frame(&ST, 0, 0, 0, &w, &h) == 0 && w == 0 && h == 0, "no frame in 4 MB");
    CHECK(!cn_stage_shot(&ST)->ok && cn_stage_finish(&ST) == 0, "the shot says nothing was drawn");
    cn_stage_band(&ST, 1, 0, 1);    /* a band after a failed prepare does nothing */
    CHECK(cn_stage_attach(&ST, 0, 0) == CN_STAGE_E_ARENA, "a null arena is refused");
    free(small);

    /* the full arena: a 2x still frame of the six-seat table on every drawer. The frame keeps whole only its picture
     * and each pixel's surface and face (8 bytes a pixel; package V1): 390 by 718 was 36 MB at 2x and drawn at 1.5,
     * and 430 by 830 on their turn (its far cups lean 361 points above the board) was drawn at 1 */
    void *A = arena_new(ARENA);
    CHECK(cn_stage_attach(&ST, A, ARENA) == 0, "a 48 MB arena is taken");
    const uint8_t *fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    CHECK(fb && cn_stage_shot(&ST)->scale == 2, "390 by 718 at 2x fits 48 MB (scale %.1f)", cn_stage_shot(&ST)->scale);
    for (int mine = 0; mine < 2; mine++) {
        CnStageIn tall = table_in(430, 830, 6, mine, 0, CN_STAGE_TABLE);
        tall.scale = 2;
        cn_stage_begin(&ST, &tall);
        fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
        CHECK(fb && cn_stage_shot(&ST)->scale == 2, "430 by 830 on %s turn at 2x fits 48 MB (scale %.1f, %d by %d, %d above)", mine ? "my" : "their",
              cn_stage_shot(&ST)->scale, w, h, ST.pad);
        printf("  2x six seats 430x830, %s turn: %dx%d pixels, textures %.1f MB, the frame %.1f MB, of %.0f MB\n", mine ? "my" : "their", w, h,
               (ARENA - cn_scene_room()) / 1048576.0, cn_scene_frame_bytes(ST.W, ST.H, ST.pad, 2, CN_STAGE_SHADOW_RES, 25344, 12864) / 1048576.0, ARENA / 1048576.0);
    }
    in = table_in(375, 541, 6, 1, 0, CN_STAGE_TABLE);
    in.scale = 2;
    cn_stage_begin(&ST, &in);
    fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    CHECK(fb && cn_stage_shot(&ST)->scale == 2, "375 by 541 at 2x fits 48 MB (scale %.1f)", cn_stage_shot(&ST)->scale);
    const uint32_t one = fb ? fnv(fb, (size_t)w * h * 4) : 0;
    printf("  2x six seats 375x541: %dx%d pixels, textures and their copies %.1f MB, the frame %.1f MB, of %.0f MB\n", w, h,
           (ARENA - cn_scene_room()) / 1048576.0, cn_scene_frame_bytes(ST.W, ST.H, ST.pad, 2, CN_STAGE_SHADOW_RES, 0, 0) / 1048576.0, ARENA / 1048576.0);

    /* purge: the arena goes, the host frees it; a new one, at another address, draws the same bytes */
    cn_stage_purge(&ST);
    CHECK(cn_stage_frame(&ST, 0, 0, 0, &w, &h) == 0, "purged: no frame");
    void *B = arena_new(ARENA + 4096);
    free(A);
    CHECK(cn_stage_attach(&ST, (uint8_t *)B + 48, ARENA) == 0, "attached again");
    fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    CHECK(fb && fnv(fb, (size_t)w * h * 4) == one, "the rebuilt textures draw the same bytes (%08x %08x)", fb ? fnv(fb, (size_t)w * h * 4) : 0, one);

    /* A FRAME DOES NOT DEPEND ON THE FRAMES BEFORE IT: on 390 by 718 a still frame drawn
     * first, and the same still frame drawn after a throw frame, are the same bytes (the
     * textures' half-size copies are made before any frame, never by whichever came first) */
    in = table_in(390, 718, 6, 1, 0, CN_STAGE_TABLE);
    in.scale = 2; in.roll_at_ms = 0;
    cn_stage_purge(&ST);
    cn_stage_attach(&ST, B, ARENA);
    const CnStageHud *hr = cn_stage_begin(&ST, &in);
    fb = cn_stage_frame(&ST, hr->total_ms, 0, 0, &w, &h);
    const uint32_t first = fb ? fnv(fb, (size_t)w * h * 4) : 0;
    cn_stage_purge(&ST);
    cn_stage_attach(&ST, B, ARENA);
    cn_stage_frame(&ST, 1000, 0, 0, &w, &h);
    fb = cn_stage_frame(&ST, hr->total_ms, 0, 0, &w, &h);
    CHECK(fb && first && fnv(fb, (size_t)w * h * 4) == first, "a still frame is the same after a throw frame as first (%08x %08x)", fb ? fnv(fb, (size_t)w * h * 4) : 0, first);

    /* an arena with room for the textures and a 1x frame but not 2x: the frame steps down */
    cn_stage_purge(&ST);
    CHECK(cn_stage_attach(&ST, B, (size_t)30 << 20) == 0, "a 30 MB arena");
    fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    CHECK(fb && cn_stage_shot(&ST)->scale < 2, "a 2x frame that does not fit is drawn smaller (scale %.1f)", cn_stage_shot(&ST)->scale);
    cn_stage_purge(&ST);
    free(B);
}

/* ---- bands, determinism, the bubble, the out seat, the clock -------------------------------- */

static uint32_t banded(CnStage *st, uint32_t t, float peek, int nb, int reverse)
{
    if (!cn_stage_prepare(st, t, peek, 0)) return 0;
    for (int pass = 0; pass < CN_STAGE_PASSES; pass++)
        for (int b = 0; b < nb; b++) cn_stage_band(st, pass, reverse ? nb - 1 - b : b, nb);
    const uint8_t *fb = cn_stage_finish(st);
    return fb ? fnv(fb, (size_t)st->shot.w * st->shot.h * 4) : 0;
}

static void test_bands_and_determinism(void)
{
    TEST("16 bands draw the single thread's bytes; the same input, the same bytes (pinned)");
    void *A = arena_new(ARENA);
    cn_stage_init(&ST, PACK, PACK_N); cn_stage_attach(&ST, A, ARENA);
    CnStageIn in = table_in(375, 541, 6, 1, 0, CN_STAGE_TABLE);
    in.roll_at_ms = 0; in.scale = 3;
    const CnStageHud *h = cn_stage_begin(&ST, &in);
    const uint32_t mid = 1200, still = h->total_ms;
    const uint32_t one_mid = banded(&ST, mid, 0, 1, 0), one_still = banded(&ST, still, .6f, 1, 0);
    CHECK(one_mid && banded(&ST, mid, 0, CN_STAGE_BANDS, 1) == one_mid, "a throw frame in 16 bands");
    CHECK(one_still && banded(&ST, still, .6f, CN_STAGE_BANDS, 1) == one_still && banded(&ST, still, .6f, 7, 0) == one_still, "a still frame in 16 and 7 bands");
    banded(&ST, mid, 0, 1, 0);
    CHECK(ST.shot.rolling && ST.shot.scale == 1.5f, "a throw frame is drawn at 1.5 (%.2f)", ST.shot.scale);
    banded(&ST, still, .6f, 1, 0);
    CHECK(!ST.shot.rolling && ST.shot.scale == 2 && ST.shot.done, "a still frame at 2, never 3 (%.2f)", ST.shot.scale);
    /* again from a fresh begin: the same bytes */
    cn_stage_begin(&ST, &in);
    CHECK(banded(&ST, mid, 0, 4, 0) == one_mid && banded(&ST, still, .6f, 4, 0) == one_still, "begun again: the same frames");
    printf("  frames: throw at 1.2 s %08x, still peeking %08x\n", one_mid, one_still);
    /* moved in package V2: the lift starts over the table, and at 375 by 541 the far seats' cups stay down
     * (their held cups left the drawer), so their dice lie at their stations (docs_pkgV2.md); re-pinned for
     * package V1: the edges pass draws each pixel on a surface's edge from four samples (every other pixel is
     * the old picture's byte for byte: cn_scene_test holds that), and every body numbers its faces; the pair is
     * the merge of both, the same under clang, gcc 16 and ASan/UBSan */
    CHECK(one_mid == 0x563a3970u, "the throw frame's golden");
    CHECK(one_still == 0x50980f05u, "the still frame's golden");
    cn_stage_purge(&ST);
    free(A);
}

static void test_bubble(void)
{
    TEST("the bubble is 300 by 195 points exactly, its cups in a row, the out cup lying");
    void *A = arena_new(ARENA);
    cn_stage_init(&ST, PACK, PACK_N); cn_stage_attach(&ST, A, ARENA);
    for (int n = 2; n <= 6; n++) {
        CnStageIn in = table_in(390, 718, n, 0, 1 % n, CN_STAGE_BUBBLE);
        in.scale = 3;
        const CnStageHud *h = cn_stage_begin(&ST, &in);
        int w, hh;
        const uint8_t *fb = cn_stage_frame(&ST, 0, 0, 0, &w, &hh);
        CHECK(h && fb && w == 600 && hh == 390, "%d seats: 600 by 390 pixels at 2x (%dx%d)", n, w, hh);
        CHECK(h && h->w == 300 && h->h == 195 && h->canvas[2] == 300 && h->canvas[3] == 195, "%d seats: the HUD's bubble", n);
        const float step = n > 4 ? 46 : 60;
        int row = 1;
        for (int i = 1; h && i < n; i++) row &= near(h->cup_x[i] - h->cup_x[i - 1], step, 1e-3) && h->cup_y[i] == 46;
        CHECK(row, "%d seats: the cups in a row %.0f apart", n, step);
    }
    cn_stage_purge(&ST);
    free(A);
}

static void test_out(void)
{
    TEST("a seat with no dice: its cup lies dim on its side, no dice, no throw");
    void *A = arena_new(ARENA);
    cn_stage_init(&ST, PACK, PACK_N); cn_stage_attach(&ST, A, ARENA);
    CnStageIn in = table_in(390, 718, 6, 0, 0, CN_STAGE_TABLE);
    in.roll_at_ms = 0;
    const CnStageHud *h = cn_stage_begin(&ST, &in);
    int n, dice5 = 0, cups5 = 0;
    const CnObj *o = cn_stage_objects(&ST, 900, 0, 0, &n);
    const CnObj *cup = 0;
    for (int i = 0; i < n; i++) if (o[i].seat == 5) { if (o[i].kind == CN_OBJ_DIE) dice5++; else { cups5++; cup = &o[i]; } }
    CHECK(h && cups5 == 1 && dice5 == 0, "one cup, no dice");
    CHECK(cup && cup->out && cup->has_rot && fabsf(cup->rot[8]) < .3f, "lying on its side, its axis near level (%.2f)", cup ? cup->rot[8] : 9);
    CHECK(cup && cup->kmul[0] < 1, "dim");
    int thrown = 0;
    for (int j = 0; j < ST.nthrow; j++) thrown |= ST.thr[j].t.seat == 5;
    int mask = 0;
    for (int s = 0; s < 6; s++) mask += ST.lay.throw_mask >> s & 1;
    CHECK(!thrown && !(ST.lay.throw_mask >> 5 & 1) && ST.nthrow == mask, "a throw a seat the layout lets throw (%d), none of them seat 5's", ST.nthrow);
    /* drawn: the lying cup's body is on the picture */
    int w, hh;
    const uint8_t *fb = cn_stage_frame(&ST, 900, 0, 0, &w, &hh);
    const uint8_t *c = fb && cup ? px_at(&ST, fb, w, hh, cup->x, cup->y, cup->lift) : 0;
    CHECK(c && c[3] == 255, "the lying cup is drawn (%d)", c ? c[3] : -1);
    cn_stage_purge(&ST);
    free(A);
}

static void test_clock(void)
{
    TEST("the roll starts at roll_at, outlasts the SHAKE beat, and my dice rest before everything does (I21)");
    void *A = arena_new(ARENA);
    cn_stage_init(&ST, PACK, PACK_N); cn_stage_attach(&ST, A, ARENA);
    CnStageIn in = table_in(390, 718, 4, 1, 0, CN_STAGE_TABLE);
    in.roll_at_ms = 1500;
    const CnStageHud *h = cn_stage_begin(&ST, &in);
    CHECK(h && h->rolls && h->roll_at_ms == 1500 && h->total_ms >= 1500 + CN_T_SHAKE, "total %u is past the SHAKE beat", h ? h->total_ms : 0);
    CHECK(h && h->rest_ms > 1500 + CN_T_SHAKE && h->rest_ms <= h->total_ms, "my dice rest at %u, inside the roll", h ? h->rest_ms : 0);
    CHECK(h && cn_stage_total_ms(&ST) == h->total_ms, "cn_stage_total_ms");
    CHECK(h && !cn_stage_done(&ST, h->total_ms - 1) && cn_stage_done(&ST, h->total_ms), "done exactly at the total");
    /* before the roll starts the cups wait low with their dice; nothing moves between two such instants */
    int n;
    float y0, y1;
    const CnObj *o = cn_stage_objects(&ST, 0, 0, 0, &n);
    y0 = o[n - 1].lift;
    o = cn_stage_objects(&ST, 1400, 0, 0, &n);
    y1 = o[n - 1].lift;
    CHECK(y0 == y1, "nothing moves before roll_at");
    o = cn_stage_objects(&ST, 1500 + 400, 0, 0, &n);
    CHECK(o[n - 1].lift != y1, "my cup moves once the roll is on");
    /* no roll: the dice at rest from the first instant, done at once */
    in.roll_at_ms = CN_STAGE_NO_ROLL;
    h = cn_stage_begin(&ST, &in);
    CHECK(h && !h->rolls && h->roll_at_ms == 0 && h->total_ms == 0 && cn_stage_done(&ST, 0) && cn_stage_total_ms(&ST) == 0, "no roll: done at 0");
    /* the refusals */
    CnStageIn bad = in;
    bad.seats = 7;
    CHECK(!cn_stage_begin(&ST, &bad) && !cn_stage_frame(&ST, 0, 0, 0, 0, 0), "seven seats refused, and nothing drawn after");
    bad = in; bad.faces[0] = 7;
    CHECK(!cn_stage_begin(&ST, &bad), "a seven on my die refused");
    bad = in; bad.kind = 9;
    CHECK(!cn_stage_begin(&ST, &bad), "an unknown kind refused");
    cn_stage_purge(&ST);
    free(A);
}

/* ---- the reveal and the HUD stay clear (package V2) --------------------------------------------- */

/* a body as the stage draws it: every corner of its mesh placed (cn_geom_emit), seen from the eye
 * (cn_cam_project), on the board's place in the drawer, through the turn (cn_cam_map) */
static float VB[CN_MESH_MAX_CORNER * CN_GEOM_VF], FB[CN_MESH_MAX_CORNER * CN_GEOM_FF];
typedef struct { int n; double x[CN_MESH_MAX_CORNER], y[CN_MESH_MAX_CORNER]; } Pic;
static void picture(const CnStage *st, int i, const CnObj *o, Pic *p)
{
    static const int tex[CN_TEX_SLOTS] = { 0 };
    const CnMesh *m = &st->mesh[st->obj_mesh[i]];
    cn_geom_emit(m, o, 0, tex, VB, 0, FB, 0);
    p->n = 0;
    for (int v = 0; v < m->ncorner; v++) {
        float px, py, gx, gy;
        cn_cam_project(&st->lay.cam, VB[v * CN_GEOM_VF], VB[v * CN_GEOM_VF + 1], VB[v * CN_GEOM_VF + 2], &px, &py);
        cn_cam_map(&st->lay.cam, st->lay.board_x + px, st->lay.board_y + py, &gx, &gy);
        p->x[p->n] = gx; p->y[p->n] = gy; p->n++;
    }
}
/* the convex hull of a picture (monotone chain), in place */
static int cmp_pt(const void *a, const void *b)
{
    const double *p = a, *q = b;
    return p[0] < q[0] ? -1 : p[0] > q[0] ? 1 : p[1] < q[1] ? -1 : p[1] > q[1];
}
static double PTS[CN_MESH_MAX_CORNER][2];
static void hull(Pic *p)
{
    const int n = p->n;
    for (int i = 0; i < n; i++) { PTS[i][0] = p->x[i]; PTS[i][1] = p->y[i]; }
    qsort(PTS, (size_t)n, sizeof PTS[0], cmp_pt);
    static int st[2 * CN_MESH_MAX_CORNER];
    int k = 0;
    #define CR(o, a, b) ((PTS[a][0] - PTS[o][0]) * (PTS[b][1] - PTS[o][1]) - (PTS[a][1] - PTS[o][1]) * (PTS[b][0] - PTS[o][0]))
    for (int i = 0; i < n; i++) { while (k >= 2 && CR(st[k - 2], st[k - 1], i) <= 0) k--; st[k++] = i; }
    for (int i = n - 2, t = k + 1; i >= 0; i--) { while (k >= t && CR(st[k - 2], st[k - 1], i) <= 0) k--; st[k++] = i; }
    #undef CR
    p->n = k - 1;
    for (int i = 0; i < p->n; i++) { p->x[i] = PTS[st[i]][0]; p->y[i] = PTS[st[i]][1]; }
}
/* a convex picture and a box (x0 y0 x1 y1) overlap: no separating axis among the box's two and the hull's edges' normals */
static int meets(const Pic *h, const double b[4])
{
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    for (int i = 0; i < h->n; i++) { x0 = fmin(x0, h->x[i]); x1 = fmax(x1, h->x[i]); y0 = fmin(y0, h->y[i]); y1 = fmax(y1, h->y[i]); }
    if (x1 <= b[0] || x0 >= b[2] || y1 <= b[1] || y0 >= b[3]) return 0;
    for (int i = 0; i < h->n; i++) {
        const int j = (i + 1) % h->n;
        const double nx = h->y[j] - h->y[i], ny = h->x[i] - h->x[j];
        double hmin = 1e30, hmax = -1e30, bmin = 1e30, bmax = -1e30;
        for (int q = 0; q < h->n; q++) { const double v = nx * h->x[q] + ny * h->y[q]; hmin = fmin(hmin, v); hmax = fmax(hmax, v); }
        for (int q = 0; q < 4; q++) { const double v = nx * b[q & 1 ? 2 : 0] + ny * b[q & 2 ? 3 : 1]; bmin = fmin(bmin, v); bmax = fmax(bmax, v); }
        if (bmin >= hmax || bmax <= hmin) return 0;
    }
    return 1;
}
static int boxes_meet(const double a[4], const double b[4]) { return a[0] < b[2] && a[2] > b[0] && a[1] < b[3] && a[3] > b[1]; }
/* a name's box on the glass (flat, turned with the planks): w by h placed on its anchor by how (cn_lay.h's CN_NAME_*) */
static void name_rect(const CnStage *st, int s, double w, double h, double out[4])
{
    const CnStageHud *H = &st->hud;
    const double x = H->name_x[s], y = H->name_y[s];
    double l, t;
    switch (H->name_how[s]) {
    case CN_NAME_FOOT: l = x - w / 2; t = y - h; break;
    case CN_NAME_LEFT: l = x; t = y - h / 2; break;
    default:           l = x - w / 2; t = y - CN_LAY_NAME_UP; break;
    }
    out[0] = out[1] = 1e30; out[2] = out[3] = -1e30;
    for (int c = 0; c < 4; c++) {
        float gx, gy;
        cn_cam_map(&st->lay.cam, (float)(l + (c & 1) * w), (float)(t + (c >> 1) * h), &gx, &gy);
        out[0] = fmin(out[0], gx); out[1] = fmin(out[1], gy); out[2] = fmax(out[2], gx); out[3] = fmax(out[3], gy);
    }
}
static void rect_box(const float r[4], double out[4]) { out[0] = r[0]; out[1] = r[1]; out[2] = r[0] + r[2]; out[3] = r[1] + r[3]; }

static const int SIZES_W[] = { 375, 390, 430 };
static const int SIZES_H[] = { 281, 290, 300, 310, 323, 328, 334, 340, 360, 399, 400, 401, 410, 421, 422, 450, 480, 541, 584, 650, 718, 760, 830, 900 };
#define NW ((int)(sizeof SIZES_W / sizeof SIZES_W[0]))
#define NH ((int)(sizeof SIZES_H / sizeof SIZES_H[0]))

static void test_reveal_inside(void)
{
    TEST("the reveal: every frame of the lift inside the drawer, 281 to 900, 2 to 6 seats; the compact row lifts every cup in full");
    cn_stage_init(&ST, PACK, PACK_N);
    double worst = 1e9;
    int frames = 0, cups = 0, full = 0, short_full = 1;
    for (int wi = 0; wi < NW; wi++) for (int hi = 0; hi < NH; hi++) for (int n = 2; n <= 6; n++) {
        const int W = SIZES_W[wi], H = SIZES_H[hi];
        CnStageIn in = table_in(W, H, n, 0, 0, CN_STAGE_REVEAL);
        const CnStageHud *h = cn_stage_begin(&ST, &in);
        if (!h) { CHECK(0, "%dx%d n %d: the reveal begins", W, H, n); continue; }
        double b[4] = { 1e30, 1e30, -1e30, -1e30 };
        for (int k = 0; k <= 24; k++) {   /* the lift's every 24th: it is a tip that only grows */
            int no;
            const CnObj *o = cn_stage_objects(&ST, CN_STAGE_NO_ROLL - 1, 0, k / 24.0f, &no);
            for (int i = 0; i < no; i++) {
                Pic p; picture(&ST, i, &o[i], &p);
                for (int v = 0; v < p.n; v++) { b[0] = fmin(b[0], p.x[v]); b[1] = fmin(b[1], p.y[v]); b[2] = fmax(b[2], p.x[v]); b[3] = fmax(b[3], p.y[v]); }
            }
            frames++;
        }
        worst = fmin(worst, fmin(fmin(b[0], b[1]), fmin(W - b[2], H - b[3])));
        /* CN_LAY_EDGE inside, as the layout fits it (half a point for the float projection) */
        const double e = CN_LAY_EDGE - .5;
        CHECK(b[0] >= e && b[1] >= e && b[2] <= W - e && b[3] <= H, "%dx%d n %d: every frame of the lift CN_LAY_EDGE inside (x %.1f..%.1f y %.1f..%.1f)", W, H, n, b[0], b[2], b[1], b[3]);
        /* each standing cup's tip against the least that shows its dice */
        int no;
        const CnObj *o = cn_stage_objects(&ST, CN_STAGE_NO_ROLL - 1, 0, 0, &no);
        for (int i = 0; i < no; i++) {
            if (o[i].kind != CN_OBJ_CUP || o[i].out || !in.dice[o[i].seat]) continue;
            float dy[5], dd[5];
            int nd = 0;
            for (int j = 0; j < no; j++) if (o[j].kind == CN_OBJ_DIE && o[j].seat == o[i].seat) { dy[nd] = o[j].y; dd[nd] = o[j].d; nd++; }
            const float least = cn_cam_peek_angle(&ST.lay.cam, o[i].R, o[i].home_y, dy, dd, nd);
            const int whole = ST.lift_angle[o[i].seat] >= least - 1e-6f;
            cups++; full += whole;
            if (h->short_board) short_full &= whole;
        }
    }
    CHECK(short_full, "on a short board every cup lifts as far as shows its dice");
    CHECK(full * 100 >= cups * 90, "nine cups in ten lift in full anywhere (%d of %d)", full, cups);
    printf("  %d reveal frames: the nearest any body comes to the drawer's edge is %.2f points; %d of %d cups lift in full\n", frames, worst, full, cups);
}

static void test_hud_clear(void)
{
    TEST("the HUD clears every cup and name, and on a short board no cup covers a name, 281 to 900, 2 to 6 seats, every screen");
    cn_stage_init(&ST, PACK, PACK_N);
    int checked = 0, tall_cover = 0, tall_cover_rest = 0;
    for (int wi = 0; wi < NW; wi++) for (int hi = 0; hi < NH; hi++) for (int n = 2; n <= 6; n++) for (int screen = 0; screen < 3; screen++) {
        const int W = SIZES_W[wi], H = SIZES_H[hi];
        CnStageIn in = table_in(W, H, n, screen == 0, 0, screen == 2 ? CN_STAGE_REVEAL : CN_STAGE_TABLE);
        const CnStageHud *h = cn_stage_begin(&ST, &in);
        if (!h) { CHECK(0, "%dx%d n %d: begins", W, H, n); continue; }
        const char *what = screen == 0 ? "mine" : screen == 1 ? "theirs" : "reveal";
        double hud[2][4];
        const char *hud_name[2];
        int nh = 0;
        if (h->has_plate) { rect_box(h->plate, hud[nh]); hud_name[nh++] = "plate"; }
        if (h->has_shelf) { rect_box(h->shelf, hud[nh]); hud_name[nh++] = "shelf"; }
        /* every name's letters: a short name's width, its line and the turn's glow bar under it (CN_LAY_NAME_H) */
        double names[CN_STAGE_SEATS][4], letters[CN_STAGE_SEATS][4];
        for (int s = 0; s < n; s++) {
            name_rect(&ST, s, CN_LAY_NAME_TEXT_W, CN_LAY_NAME_H, names[s]);
            name_rect(&ST, s, CN_LAY_NAME_TEXT_W, CN_LAY_NAME_TEXT_H, letters[s]);
        }
        /* the cups as they stand (my cup shut), and at the reveal tipped in full */
        int no;
        const CnObj *o = cn_stage_objects(&ST, CN_STAGE_NO_ROLL - 1, 0, screen == 2 ? 1 : 0, &no);
        for (int i = 0; i < no; i++) {
            if (o[i].kind != CN_OBJ_CUP) continue;
            Pic p; picture(&ST, i, &o[i], &p); hull(&p);
            for (int k = 0; k < nh; k++)
                CHECK(!meets(&p, hud[k]), "%dx%d n %d %s: seat %d's cup clear of the %s", W, H, n, what, o[i].seat, hud_name[k]);
            for (int s = 0; s < n; s++) {
                const int covers = meets(&p, letters[s]);
                if (h->short_board) CHECK(!covers, "%dx%d n %d %s: seat %d's cup clear of seat %d's name", W, H, n, what, o[i].seat, s);
                else { tall_cover += covers; tall_cover_rest += covers && screen != 2; }
            }
            checked++;
        }
        for (int s = 0; s < n; s++)
            for (int k = 0; k < nh; k++) CHECK(!boxes_meet(names[s], hud[k]), "%dx%d n %d %s: seat %d's name clear of the %s", W, H, n, what, s, hud_name[k]);
    }
    printf("  %d cups against the plate, the shelf and every name; on tall boards a cup covers a name %d times (%d of them standing: the ring's names are the study's, docs_pkgV2.md)\n", checked, tall_cover, tall_cover_rest);
}

/* ---- the names on the table (package N) --------------------------------------------------------- */

/* A NAME'S BITMAP as a host would hand it over, 3 texels a point: the block is the letters (blocks of ink five points
 * wide, two apart, over a row of 17 points; a seat's name is 40 + 8 seat points wide) and, when bright, the turn's bar
 * under them (36 points, 2 tall, 3 below) with a soft glow round it; the halo empty. Premultiplied RGBA. The texels,
 * and its size in points in *wpt, *hpt. */
static uint8_t NAME_PX[CN_STAGE_NAME_W_MAX * CN_STAGE_NAME_H_MAX * 4];
static void name_bitmap(int seat, int bright, int *w, int *h, float *wpt, float *hpt)
{
    const int halo = CN_STAGE_NAME_HALO, tw = 40 + 8 * seat, th = 17, bw = (tw + 2 * halo), bh = (th + 5 + 2 * halo);
    *wpt = (float)bw; *hpt = (float)bh; *w = bw * 3; *h = bh * 3;
    memset(NAME_PX, 0, (size_t)*w * *h * 4);
    const uint8_t ink[3] = { bright ? 230 : 128, bright ? 240 : 152, bright ? 235 : 144 }, glow[3] = { 143, 251, 224 };
    for (int y = 0; y < *h; y++) for (int x = 0; x < *w; x++) {
        const float px = (x + .5f) / 3 - halo, py = (y + .5f) / 3 - halo;   /* points in the block */
        uint8_t *o = &NAME_PX[((size_t)y * *w + x) * 4];
        if (px >= 4 && px < tw - 4 && py >= 3 && py < th - 3 && (int)(px - 4) % 7 < 5) { o[0] = ink[0]; o[1] = ink[1]; o[2] = ink[2]; o[3] = 255; continue; }
        if (!bright) continue;
        const float bx = px < tw / 2.f - 18 ? tw / 2.f - 18 - px : px > tw / 2.f + 18 ? px - tw / 2.f - 18 : 0;
        const float by = py < th + 3 ? th + 3 - py : py > th + 5 ? py - th - 5 : 0, d = bx > by ? bx : by;
        const float a = d <= 0 ? 1 : d < 5 ? .6f * (1 - d / 5) : 0;
        for (int c = 0; c < 3; c++) o[c] = (uint8_t)(glow[c] * a + .5f);
        o[3] = (uint8_t)(255 * a + .5f);
    }
}
/* every seat's name, the turn's bright; the bitmaps of the last call are NAME_PX's no more (each is copied in) */
static int give_names(CnStage *st, int n, int turn)
{
    int changed = 0;
    for (int s = 0; s < n; s++) {
        int w, h; float wpt, hpt;
        name_bitmap(s, s == turn, &w, &h, &wpt, &hpt);
        changed += cn_stage_name(st, s, NAME_PX, w, h, wpt, hpt) == 1;
    }
    return changed;
}
/* two frames to compare, as large as the largest asked for */
static uint8_t *FB_NAMED, *FB_BARE;
static size_t FB_CAP;
static void fb_room(size_t n)
{
    if (n <= FB_CAP) return;
    free(FB_NAMED); free(FB_BARE);
    FB_NAMED = arena_new(n); FB_BARE = arena_new(n); FB_CAP = n;
}

/* name s's rect on the frame, inset by `in` points, in pixels: x0 y0 x1 y1 */
static int name_px_box(const CnStage *st, int s, float in, int box[4])
{
    float r[4];
    if (!cn_stage_name_rect(st, s, r)) return 0;
    const float sc = st->shot.scale;
    box[0] = (int)((r[0] + in) * sc); box[2] = (int)((r[2] - in) * sc);
    box[1] = (int)((r[1] + in + st->pad) * sc); box[3] = (int)((r[3] - in + st->pad) * sc);
    if (box[0] < 0) box[0] = 0;
    if (box[1] < 0) box[1] = 0;
    if (box[2] > st->shot.w) box[2] = st->shot.w;
    if (box[3] > st->shot.h) box[3] = st->shot.h;
    return 1;
}

static void test_names(void)
{
    TEST("the names lie on the table: a cup in front hides one, the frame without them is the old one, and they fit 48 MB");
    void *A = arena_new(ARENA);
    cn_stage_init(&ST, PACK, PACK_N); cn_stage_attach(&ST, A, ARENA);

    /* THE WORST MEMORY: 430 by 830 on their turn at 2x, six names (the turn's with its bar) */
    CnStageIn in = table_in(430, 830, 6, 0, 0, CN_STAGE_TABLE);
    in.scale = 2;
    cn_stage_begin(&ST, &in);
    int w, h;
    const uint8_t *fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    const uint32_t bare = fb ? fnv(fb, (size_t)w * h * 4) : 0;
    const size_t tex_bare = ARENA - cn_scene_room();
    CHECK(give_names(&ST, 6, in.turn) == 6, "six names given");
    fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    int drawn = 0;
    for (int s = 0; s < 6; s++) drawn += ST.name[s].id >= 0;
    const size_t tex_named = ARENA - cn_scene_room();
    CHECK(fb && ST.shot.scale == 2 && drawn == 6, "430 by 830, their turn: every name drawn and the frame still 2x (%d names, scale %.1f)", drawn, ST.shot.scale);
    printf("  430x830 their turn at 2x: the set %.2f MB, six names %.2f MB with their copies, the frame %.2f MB: %.2f of %.0f MB\n", tex_bare / 1048576.0,
           (tex_named - tex_bare) / 1048576.0, cn_scene_frame_bytes(ST.W, ST.H, ST.pad, 2, CN_STAGE_SHADOW_RES, 25344 + 24, 12864 + 12) / 1048576.0,
           (tex_named + cn_scene_frame_bytes(ST.W, ST.H, ST.pad, 2, CN_STAGE_SHADOW_RES, 25344 + 24, 12864 + 12)) / 1048576.0, ARENA / 1048576.0);
    CHECK(tex_named - tex_bare < (size_t)1 << 20, "six names and their copies are under 1 MB (%zu bytes)", tex_named - tex_bare);
    const uint32_t named = fb ? fnv(fb, (size_t)w * h * 4) : 0;
    CHECK(named != bare, "the names are in the picture");
    /* the same names again change nothing and upload nothing; taking them all away is the bare frame, byte for byte */
    CHECK(give_names(&ST, 6, in.turn) == 0 && ST.names_up, "the same names again: nothing changed");
    for (int s = 0; s < 6; s++) cn_stage_name(&ST, s, 0, 0, 0, 0, 0);
    fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    CHECK(fb && fnv(fb, (size_t)w * h * 4) == bare, "names taken away: the frame drawn before any name (%08x %08x)", fb ? fnv(fb, (size_t)w * h * 4) : 0, bare);
    CHECK(ARENA - cn_scene_room() == tex_bare, "and their room given back");
    give_names(&ST, 6, in.turn);
    fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    CHECK(fb && fnv(fb, (size_t)w * h * 4) == named, "given again: the named frame again");
    /* refused: past the maxima, a seat past six, no size */
    CHECK(cn_stage_name(&ST, 6, NAME_PX, 4, 4, 10, 10) == -1 && cn_stage_name(&ST, 0, NAME_PX, CN_STAGE_NAME_W_MAX + 1, 4, 10, 10) == -1 &&
          cn_stage_name(&ST, 0, NAME_PX, 4, CN_STAGE_NAME_H_MAX + 1, 10, 10) == -1 && cn_stage_name(&ST, 0, NAME_PX, 4, 4, 0, 10) == -1, "a name past the maxima is refused");

    /* A CUP IN FRONT HIDES A NAME. Over tall drawers, both turns and the reveal (every cup up): at every pixel of a name's
     * rect where the bare frame shows a cup (opaque, and opaque all round: inside it, not at its edge), the named frame
     * is the bare one's, byte for byte; and somewhere a name is half hidden: some of its pixels a cup's, some its own */
    static const int SW[] = { 390, 430, 375 }, SH[] = { 718, 830, 541 };
    long covered = 0, wrong = 0, half = 0;
    for (int z = 0; z < 3; z++) for (int kind = CN_STAGE_TABLE; kind <= CN_STAGE_REVEAL; kind++) for (int mine = 0; mine < 2; mine++) for (int n = 4; n <= 6; n++) {
        if (kind == CN_STAGE_REVEAL && mine) continue;
        CnStageIn t = table_in(SW[z], SH[z], n, mine, 0, kind);
        t.scale = 2;
        cn_stage_begin(&ST, &t);
        for (int s = 0; s < 6; s++) cn_stage_name(&ST, s, 0, 0, 0, 0, 0);
        fb = cn_stage_frame(&ST, 0, 0, 1, &w, &h);
        if (!fb) { CHECK(0, "%dx%d: a frame", SW[z], SH[z]); continue; }
        fb_room((size_t)w * h * 4);
        memcpy(FB_BARE, fb, (size_t)w * h * 4);
        give_names(&ST, n, t.turn);
        const int w0 = w, h0 = h;
        fb = cn_stage_frame(&ST, 0, 0, 1, &w, &h);
        if (!fb || w != w0 || h != h0) { CHECK(0, "%dx%d: the named frame, the bare one's size", SW[z], SH[z]); continue; }
        memcpy(FB_NAMED, fb, (size_t)w * h * 4);
        for (int s = 0; s < n; s++) {
            int b[4];
            if (!name_px_box(&ST, s, 2, b)) continue;
            long cov = 0, own = 0;
            for (int y = b[1] + 1; y < b[3] - 1; y++) for (int x = b[0] + 1; x < b[2] - 1; x++) {
                const size_t i = ((size_t)y * w + x) * 4;
                int body = 1;
                for (int dy = -1; dy <= 1 && body; dy++) for (int dx = -1; dx <= 1; dx++) body &= FB_BARE[i + ((long)dy * w + dx) * 4 + 3] == 255;
                if (body) { cov++; wrong += memcmp(&FB_BARE[i], &FB_NAMED[i], 4) != 0; }
                else own += memcmp(&FB_BARE[i], &FB_NAMED[i], 4) != 0;
            }
            covered += cov;
            if (cov > 50 && own > 50) {
                if (!half) printf("  %dx%d %s, %s turn, %d seats: seat %d's name half under a cup (%ld pixels the cup's, %ld its own)\n", SW[z], SH[z],
                                  kind == CN_STAGE_REVEAL ? "the reveal" : "the table", mine ? "my" : "their", n, s, cov, own);
                half++;
            }
        }
    }
    CHECK(covered > 1000 && wrong == 0, "every pixel of a name under a cup is the cup's (%ld of %ld differ)", wrong, covered);
    CHECK(half > 0, "and %ld names are half hidden, half shown", half);

    /* THE ARENA'S FAILURE PATH: an arena that holds the set and the still frame but not the names draws the frame at
     * its scale and skips the names; with room for two, the first two seats' */
    in = table_in(430, 830, 6, 0, 0, CN_STAGE_TABLE);
    in.scale = 2;
    cn_stage_begin(&ST, &in);
    for (int s = 0; s < 6; s++) cn_stage_name(&ST, s, 0, 0, 0, 0, 0);
    fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    int nv = 4 * 6, nf = 2 * 6;   /* the bodies' and six names' */
    for (int i = 0; i < ST.nobj; i++) { nv += ST.mesh[ST.obj_mesh[i]].ncorner; nf += ST.mesh[ST.obj_mesh[i]].ntri; }
    const size_t need = ARENA - cn_scene_room() + cn_scene_frame_bytes(ST.W, ST.H, ST.name_pad, 2, CN_STAGE_SHADOW_RES, nv, nf);
    for (int room = 0; room < 2; room++) {
        cn_stage_purge(&ST);
        const size_t bytes = need + (room ? (size_t)300 << 10 : 0);
        CHECK(cn_stage_attach(&ST, A, bytes) == 0, "an arena of %.2f MB", bytes / 1048576.0);
        give_names(&ST, 6, in.turn);
        fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
        int k = 0, first = 0;
        for (int s = 0; s < 6; s++) { k += ST.name[s].id >= 0; first += s < 2 && ST.name[s].id >= 0; }
        if (!room) CHECK(fb && ST.shot.scale == 2 && fnv(fb, (size_t)w * h * 4) == bare && k == 0, "no room for a name: none drawn, the bare frame at 2x (%d names, scale %.1f)", k, ST.shot.scale);
        else CHECK(fb && ST.shot.scale == 2 && k == 2 && first == 2, "room for two: seats 0 and 1 drawn, the frame at 2x (%d names, scale %.1f)", k, ST.shot.scale);
    }
    /* the reserve is the table's: a small table in the same arena holds all six, and the tall one begun again two */
    CnStageIn small = table_in(375, 541, 6, 0, 0, CN_STAGE_TABLE);
    small.scale = 2;
    cn_stage_begin(&ST, &small);
    fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    int k6 = 0;
    for (int s = 0; s < 6; s++) k6 += ST.name[s].id >= 0;
    cn_stage_begin(&ST, &in);
    fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    int k2 = 0;
    for (int s = 0; s < 6; s++) k2 += ST.name[s].id >= 0;
    CHECK(k6 == 6 && fb && ST.shot.scale == 2 && k2 == 2, "375 by 541 in it: six names; 430 by 830 begun again: two, and the frame at 2x (%d, %d, scale %.1f)", k6, k2, ST.shot.scale);
    /* a purge and a new arena: the names come back from the stage's own copy, the same bytes */
    cn_stage_purge(&ST);
    cn_stage_attach(&ST, A, ARENA);
    fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    CHECK(fb && fnv(fb, (size_t)w * h * 4) == named, "purged and attached again: the named frame, nothing asked of the host (%08x %08x)", fb ? fnv(fb, (size_t)w * h * 4) : 0, named);

    /* bands, and the pin: the six-seat table of the bands test (375 by 541, a throw and a still frame peeking) with
     * names; and the bubble draws none */
    in = table_in(375, 541, 6, 1, 0, CN_STAGE_TABLE);
    in.roll_at_ms = 0; in.scale = 3;
    const CnStageHud *hh = cn_stage_begin(&ST, &in);
    give_names(&ST, 6, in.turn);
    const uint32_t mid = banded(&ST, 1200, 0, 1, 0), still = banded(&ST, hh->total_ms, .6f, 1, 0);
    CHECK(mid && banded(&ST, 1200, 0, CN_STAGE_BANDS, 1) == mid && still && banded(&ST, hh->total_ms, .6f, 7, 0) == still, "with names: 16 and 7 bands draw the one thread's bytes");
    printf("  frames with names: throw at 1.2 s %08x, still peeking %08x\n", mid, still);
    /* pinned (package N): the same under clang, gcc 16 and ASan/UBSan */
    CHECK(mid == 0xa48f41f0u, "the throw frame with names' golden");
    CHECK(still == 0x44078389u, "the still frame with names' golden");
    CnStageIn bub = table_in(300, 195, 4, 0, 0, CN_STAGE_BUBBLE);
    cn_stage_begin(&ST, &bub);
    fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    const uint32_t with = fb ? fnv(fb, (size_t)w * h * 4) : 0;
    for (int s = 0; s < 6; s++) cn_stage_name(&ST, s, 0, 0, 0, 0, 0);
    fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    CHECK(with && fb && fnv(fb, (size_t)w * h * 4) == with, "the bubble draws no names");
    cn_stage_purge(&ST);
    free(A);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/cn_tex.pack";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cn_stage_test: no texture pack at %s (make tex)\n", path); return 2; }
    fseek(f, 0, SEEK_END);
    PACK_N = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    PACK = malloc(PACK_N);
    if (!PACK || fread(PACK, 1, PACK_N, f) != PACK_N) { fprintf(stderr, "cn_stage_test: could not read %s\n", path); return 2; }
    fclose(f);
    test_layout();
    test_frame();
    test_hand();
    test_memory();
    test_bands_and_determinism();
    test_bubble();
    test_out();
    test_clock();
    test_reveal_inside();
    test_hud_clear();
    test_names();
    free(PACK);
    return report("cn_stage_test");
}
