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
    { 430, 830, 0, 30, 788, 56.9066, 903, 79,
      { { 44.9507, 524.1192 }, { 44.9507, 168.3576 }, { 199, -9.5232 }, { 353.0493, 168.3576 }, { 293.0299, 524.1192 } },
      { { 44.9507, 595.0257 }, { 44.9507, 239.2641 }, { 199, 61.3833 }, { 353.0493, 239.2641 }, { 293.0299, 595.0257 } } },
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

    /* the full arena: a 2x still frame of the six-seat table on 375 by 541; on 390 by 718
     * the frame alone is 36 MB at 2x and it is drawn at 1.5 (docs_pkgD.md has the table) */
    void *A = arena_new(ARENA);
    CHECK(cn_stage_attach(&ST, A, ARENA) == 0, "a 48 MB arena is taken");
    const uint8_t *fb = cn_stage_frame(&ST, 0, 0, 0, &w, &h);
    CHECK(fb && cn_stage_shot(&ST)->scale == 1.5f, "390 by 718 at 2x does not fit 48 MB: 1.5 (scale %.1f)", cn_stage_shot(&ST)->scale);
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
    CHECK(one_mid == 0x18318e6bu, "the throw frame's golden");
    CHECK(one_still == 0xe41ce178u, "the still frame's golden");
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
    CHECK(!thrown && ST.nthrow == 5, "five throws, none of them seat 5's (%d)", ST.nthrow);
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
    free(PACK);
    return report("cn_stage_test");
}
