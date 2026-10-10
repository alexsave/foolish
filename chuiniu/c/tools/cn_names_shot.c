/* cn_names_shot: the names on the table, offline (package N). A six-seat table through the stage (cn_stage), each
 * seat's name handed over as the iOS host hands it (cn_stage_name: the name in IM Fell English SC at 14, tracked
 * .14em, three texels a point, the turn's bar under it, premultiplied), drawn at 2x and laid over the planks
 * (cn_texgen's tile, flat: the host's turn of the layer is left out). It looks for a table where a cup stands in
 * front of a name and writes that one: the picture, and a close-up of the half-hidden name with the same table
 * drawn without its names beside it. And the cost: a still frame at 2x with and without the six names.
 *
 *   make -C chuiniu/c names-shot          build/names_shot.png, build/names_shot_zoom.png
 *
 * The letters here are this tool's (cn_texgen's TrueType reader); in the product they are the host's (CoreText,
 * NameDecal.swift), which is what lets a name be any script. Not a test. */
#define main cn_texgen_main
#include "cn_texgen.c"
#undef main
#include "../src/cn_stage.h"
#include <math.h>
#include <time.h>

/* the table's six names, the study's */
static const char *const NAMES[6] = { "Alex", "Bo", "Cy", "Dee", "Eve", "Fay" };

/* a line of the small caps into a coverage mask (w by h texels), its pen starting at x0 on the baseline, px a point
 * at size pt, tracked .14em; the advance in points */
static double text_line(const Font *f, const char *s, double pt, double px, uint8_t *mask, int w, int h, double x0, double base, int draw)
{
    const double upem = be16(f->b + f->head + 18), sc = pt * px / upem, track = .14 * pt * px;
    double pen = x0 * px;
    static Edge e[8192];
    for (const char *c = s; *c; c++) {
        const int gid = font_gid(f, (unsigned)(unsigned char)*c);
        if (gid <= 0) continue;
        int bbox[4];
        const int ne = draw ? font_outline(f, gid, e, 8192, bbox) : -1;
        if (ne > 0) {
            const int gx = (int)floor(pen + bbox[0] * sc) - 1, gy = (int)floor(base * px - bbox[3] * sc) - 1;
            const int gw = (int)ceil((bbox[2] - bbox[0]) * sc) + 3, gh = (int)ceil((bbox[3] - bbox[1]) * sc) + 3;
            for (int i = 0; i < ne; i++) {
                e[i].x0 = pen + e[i].x0 * sc - gx; e[i].x1 = pen + e[i].x1 * sc - gx;
                e[i].y0 = base * px - e[i].y0 * sc - gy; e[i].y1 = base * px - e[i].y1 * sc - gy;
            }
            uint8_t *g = calloc((size_t)gw * gh, 1);
            raster(e, ne, gw, gh, g);
            for (int y = 0; y < gh; y++) for (int x = 0; x < gw; x++) {
                const int X = gx + x, Y = gy + y;
                if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
                const int v = mask[Y * w + X] + g[y * gw + x];
                mask[Y * w + X] = (uint8_t)(v > 255 ? 255 : v);
            }
            free(g);
        }
        pen += font_advance(f, gid) * sc + track;
    }
    return pen / px - x0;
}
/* a mask blurred by a box of r texels, twice (near enough the layers' Gaussian) */
static void blur(const uint8_t *in, uint8_t *out, int w, int h, int r)
{
    int *t = malloc(sizeof(int) * (size_t)w * h);
    uint8_t *a = malloc((size_t)w * h);
    memcpy(a, in, (size_t)w * h);
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int s = 0, n = 0; for (int k = -r; k <= r; k++) if (x + k >= 0 && x + k < w) { s += a[y * w + x + k]; n++; } t[y * w + x] = s / n; }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int s = 0, n = 0; for (int k = -r; k <= r; k++) if (y + k >= 0 && y + k < h) { s += t[(y + k) * w + x]; n++; } a[y * w + x] = (uint8_t)(s / n); }
    }
    memcpy(out, a, (size_t)w * h);
    free(t); free(a);
}
/* "over" in premultiplied texels: o = c + o (1 - a), the colour rgb at coverage cov times alpha */
static void over(uint8_t *o, const uint8_t rgb[3], double a)
{
    for (int c = 0; c < 3; c++) o[c] = (uint8_t)(rgb[c] * a + o[c] * (1 - a) + .5);
    o[3] = (uint8_t)(255 * a + o[3] * (1 - a) + .5);
}

/* A NAME'S BITMAP, as NameDecal.render draws it: the block (the letters' box, 8 more than they are wide; the line;
 * 3 apart; the 2-point bar, 36 wide under a far seat's, 44 under mine) with CN_STAGE_NAME_HALO round it */
static void name_bitmap(const Font *f, const char *name, int bright, int box, int alive, uint8_t *px, int *w, int *h, float *wpt, float *hpt)
{
    const double pt = 14, halo = CN_STAGE_NAME_HALO, k = 3;
    const double asc = bes16(f->b + f->hhea + 4), desc = -bes16(f->b + f->hhea + 6), upem = be16(f->b + f->head + 18);
    const double lineH = ceil((asc + desc) / upem * pt), textW = fmin(150, ceil(text_line(f, name, pt, k, 0, 0, 0, 0, 0, 0)) + 8);
    *wpt = (float)(textW + 2 * halo); *hpt = (float)(lineH + 5 + 2 * halo);
    *w = (int)ceil(*wpt * k); *h = (int)ceil(*hpt * k);
    const int W = *w, H = *h;
    uint8_t *m = calloc((size_t)W * H, 1), *s = calloc((size_t)W * H, 1), *sh = calloc((size_t)W * H, 1);
    const double width = text_line(f, name, pt, k, 0, 0, 0, 0, 0, 0), x0 = halo + (textW - width) / 2, base = halo + asc / upem * pt;
    text_line(f, name, pt, k, m, W, H, x0, base, 1);
    /* the shadow: black, a point down, blurred a point */
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) s[y * W + x] = y >= 3 ? m[(y - 3) * W + x] : 0;
    blur(s, sh, W, H, 1);
    memset(px, 0, (size_t)W * H * 4);
    static const uint8_t black[3] = { 0, 0, 0 }, ink[3] = { 0xE2, 0xE7, 0xD4 }, dim[3] = { 0x8E, 0xA3, 0x9A }, glow[3] = { 0x8F, 0xFB, 0xE0 };
    const double a_ink = alive ? 1 : .6;
    if (bright) {   /* the bar and its glow */
        const double bw = box ? 36 : 44, bx0 = halo + textW / 2 - bw / 2, by0 = halo + lineH + 3;
        uint8_t *bar = calloc((size_t)W * H, 1), *gl = calloc((size_t)W * H, 1);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            const double X = (x + .5) / k, Y = (y + .5) / k;
            bar[y * W + x] = X >= bx0 && X < bx0 + bw && Y >= by0 && Y < by0 + 2 ? 255 : 0;
        }
        blur(bar, gl, W, H, 6);
        for (int i = 0; i < W * H; i++) { over(&px[i * 4], glow, gl[i] / 255. * .9); over(&px[i * 4], glow, bar[i] / 255.); }
        free(bar); free(gl);
    }
    for (int i = 0; i < W * H; i++) { over(&px[i * 4], black, sh[i] / 255. * a_ink); over(&px[i * 4], bright ? ink : dim, m[i] / 255. * a_ink); }
    free(m); free(s); free(sh);
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static CnStage ST;
static uint8_t NAME_PX[CN_STAGE_NAME_W_MAX * CN_STAGE_NAME_H_MAX * 4];

static CnStageIn table(int W, int H, int n, int mine, int kind)
{
    static const uint8_t COUNTS[6] = { 5, 4, 5, 3, 5, 2 };
    CnStageIn in;
    memset(&in, 0, sizeof in);
    in.kind = (uint8_t)kind; in.seats = (uint8_t)n; in.me = 0; in.turn = (uint8_t)(mine ? 0 : 1);
    for (int s = 0; s < n; s++) { in.dice[s] = COUNTS[s]; for (int d = 0; d < COUNTS[s]; d++) in.faces[s * 5 + d] = (uint8_t)(1 + (s + d * 2) % 6); }
    if (n == 6) { in.out_mask = 1 << 5; in.dice[5] = 0; memset(&in.faces[25], 0, 5); }
    in.known_mask = kind == CN_STAGE_REVEAL ? (uint8_t)((1 << n) - 1) : 1;
    in.w = (float)W; in.h = (float)H; in.scale = 2; in.seed = 7; in.roll_at_ms = CN_STAGE_NO_ROLL;
    return in;
}
static void give(const Font *f, int n, int turn, int out_mask)
{
    for (int s = 0; s < n; s++) {
        int w, h; float wpt, hpt;
        name_bitmap(f, NAMES[s], s == turn, ST.lay.name_how[s] == CN_NAME_BOX, !(out_mask >> s & 1), NAME_PX, &w, &h, &wpt, &hpt);
        cn_stage_name(&ST, s, NAME_PX, w, h, wpt, hpt);
    }
}
static void clear(void) { for (int s = 0; s < 6; s++) cn_stage_name(&ST, s, 0, 0, 0, 0, 0); }

/* the frame over the planks (tiled from the canvas's top left), RGB */
static uint8_t *over_planks(const uint8_t *fb, int w, int h, const uint8_t *pl, int pw, int ph)
{
    uint8_t *o = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const uint8_t *c = &fb[((size_t)y * w + x) * 4], *p = &pl[((size_t)(y % ph) * pw + x % pw) * 3];
        for (int k = 0; k < 3; k++) o[((size_t)y * w + x) * 3 + k] = (uint8_t)((c[k] * c[3] + p[k] * (255 - c[3]) + 127) / 255);
    }
    return o;
}

int main(int argc, char **argv)
{
    const char *packp = argc > 1 ? argv[1] : "build/cn_tex.pack", *fontp = argc > 2 ? argv[2] : "tools/fonts/IMFeENsc28P.ttf";
    const char *outp = argc > 3 ? argv[3] : "build/names_shot.png", *zoomp = argc > 4 ? argv[4] : "build/names_shot_zoom.png";
    size_t pn, fnb;
    uint8_t *pack = read_file(packp, &pn), *fontb = read_file(fontp, &fnb);
    Font f;
    if (!pack || !fontb || !font_open(&f, fontb, fnb)) { fprintf(stderr, "cn_names_shot: the pack (%s) and the small caps (%s)\n", packp, fontp); return 2; }
    void *arena = malloc(CN_STAGE_ARENA);
    if (cn_stage_init(&ST, pack, pn) || cn_stage_attach(&ST, arena, CN_STAGE_ARENA)) { fprintf(stderr, "cn_names_shot: the stage\n"); return 1; }
    enum { S = CN_TEXGEN_PLANK_SCALE };
    const int pw = TG_TILE_W * S, ph = TG_TILE_H * S;
    uint8_t *pl = malloc((size_t)pw * ph * 3);
    cn_texgen_planks(S, 1, pl);

    /* THE COST: a still frame of six seats at 2x, with and without the names, the least of 15 */
    static const int CW[2] = { 390, 430 }, CH[2] = { 718, 830 };
    for (int c = 0; c < 2; c++) for (int named = 0; named < 2; named++) {
        CnStageIn in = table(CW[c], CH[c], 6, 0, CN_STAGE_TABLE);
        cn_stage_begin(&ST, &in);
        clear();
        if (named) give(&f, 6, in.turn, in.out_mask);
        int w, h;
        cn_stage_frame(&ST, 0, 0, 0, &w, &h);   /* the textures, once */
        double best = 1e9;
        for (int r = 0; r < 15; r++) { const double t0 = now_ms(); cn_stage_frame(&ST, 0, 0, 0, &w, &h); const double t = now_ms() - t0; if (t < best) best = t; }
        printf("%dx%d six seats, still, 2x (%d by %d), %s: %.2f ms on one thread, %.2f MB of textures\n", CW[c], CH[c], w, h,
               named ? "six names" : "no names", best, (CN_STAGE_ARENA - cn_scene_room()) / 1048576.0);
    }

    /* THE TABLE: a far seat's name a cup stands in front of, on a tall board (their turn, then mine; the reveal) */
    static const int SW[] = { 390, 430, 375 }, SH[] = { 718, 830, 541 };
    int best_cov = 0, bz = 0, bkind = 0, bmine = 0, bn = 0, bs = -1;
    for (int z = 0; z < 3; z++) for (int kind = CN_STAGE_TABLE; kind <= CN_STAGE_REVEAL; kind++) for (int mine = 0; mine < 2; mine++) for (int n = 4; n <= 6; n++) {
        CnStageIn in = table(SW[z], SH[z], n, mine, kind);
        cn_stage_begin(&ST, &in);
        clear();
        int w, h;
        const uint8_t *fb = cn_stage_frame(&ST, 0, 0, 1, &w, &h);
        if (!fb) continue;
        uint8_t *bare = malloc((size_t)w * h * 4);
        memcpy(bare, fb, (size_t)w * h * 4);
        give(&f, n, in.turn, in.out_mask);
        fb = cn_stage_frame(&ST, 0, 0, 1, &w, &h);
        for (int s = 0; s < n; s++) {
            float r[4];
            if (s == in.me || !cn_stage_name_rect(&ST, s, r)) continue;
            /* the name's letters' texels where the bare frame has a body: hidden; and where it is shown */
            int cov = 0, own = 0;
            for (int y = (int)((r[1] + ST.pad + 8) * 2); y < (int)((r[3] + ST.pad - 8) * 2); y++) for (int x = (int)((r[0] + 8) * 2); x < (int)((r[2] - 8) * 2); x++) {
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                const size_t i = ((size_t)y * w + x) * 4;
                if (bare[i + 3] == 255) cov++; else own += memcmp(&bare[i], &fb[i], 4) != 0;
            }
            if (own > 200 && cov > 100 && cov > best_cov) { best_cov = cov; bz = z; bkind = kind; bmine = mine; bn = n; bs = s; }
        }
        free(bare);
    }
    if (bs < 0) { fprintf(stderr, "cn_names_shot: no far name with a cup in front of it\n"); return 1; }
    CnStageIn in = table(SW[bz], SH[bz], bn, bmine, bkind);
    cn_stage_begin(&ST, &in);
    clear();
    int w, h;
    const uint8_t *fb = cn_stage_frame(&ST, 0, 0, 1, &w, &h);
    uint8_t *bare = over_planks(fb, w, h, pl, pw, ph);
    give(&f, bn, in.turn, in.out_mask);
    fb = cn_stage_frame(&ST, 0, 0, 1, &w, &h);
    uint8_t *named = over_planks(fb, w, h, pl, pw, ph);
    if (!write_png(outp, named, w, h)) return 1;
    printf("%dx%d %s, %s turn, %d seats: %s's name half behind a cup (%d of its texels a cup's): wrote %s (%dx%d)\n", SW[bz], SH[bz],
           bkind == CN_STAGE_REVEAL ? "the reveal" : "the table", bmine ? "my" : "their", bn, NAMES[bs], best_cov, outp, w, h);
    /* the close-up: the name's rect and a cup's height round it, with the names and without, side by side, 3 times */
    float r[4];
    cn_stage_name_rect(&ST, bs, r);
    const int zx = (int)fmax(0, (r[0] - 30) * 2), zy = (int)fmax(0, (r[1] + ST.pad - 90) * 2);
    const int zw = (int)fmin(w - zx, (r[2] - r[0] + 60) * 2), zh = (int)fmin(h - zy, (r[3] - r[1] + 120) * 2), Z = 3, gapw = 8;
    const int ow = (2 * zw + gapw) * Z, oh = zh * Z;
    uint8_t *zo = calloc((size_t)ow * oh * 3, 1);
    for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
        const int px = x / Z, side = px >= zw + gapw ? 1 : px < zw ? 0 : -1;
        if (side < 0) continue;
        const int sx = zx + (side ? px - zw - gapw : px), sy = zy + y / Z;
        memcpy(&zo[((size_t)y * ow + x) * 3], &(side ? bare : named)[((size_t)sy * w + sx) * 3], 3);
    }
    if (!write_png(zoomp, zo, ow, oh)) return 1;
    printf("wrote %s: the name behind the cup (left), the same table without names (right)\n", zoomp);
    free(zo); free(bare); free(named); free(pl); free(arena); free(pack); free(fontb);
    return 0;
}
