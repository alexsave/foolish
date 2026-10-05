/* The texture pack and the textures derived from it (src/cn_tex.c, tools/cn_texgen.c).
 *
 *   ./build/cn_tex_test [font.ttf] [pack]   (from chuiniu/c; `make tex-test`)
 *
 * The baker is compiled in (its main left out), so the test bakes the pack in
 * memory, holds it to the file `make tex` wrote, and pins both to goldens. */
#define CN_TEXGEN_NO_MAIN
#include "../tools/cn_texgen.c"
#include "../../../shared/c/test/check.h"

static uint32_t le32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint32_t le16(const uint8_t *b) { return (uint32_t)(b[0] | b[1] << 8); }
/* a texture or a pack as one number */
static uint32_t fnv(const void *b, size_t n) { return cn_tex_fnv1a((const uint8_t *)b, (uint32_t)n); }

/* THE GOLDENS. A change to any generator, offset, gradient, glyph or the
 * layout moves one of these; that is a new look, so re-pin it on purpose. */
#define GOLD_PACK_LEN    344944u
#define GOLD_PACK        0xacf5f72au
#define GOLD_SIDE7       0xd26ecd2du
#define GOLD_CROWN7      0x65aa86fdu
#define GOLD_DIE7        0x7a477160u
#define GOLD_DIE7_BUMP   0x455713c1u

/* the iMessage extension's budget for the pack on disk */
#define PACK_BUDGET (4u << 20)

static int dark(const uint8_t *t) { return t[0] + t[1] + t[2] < 3 * 60; }

/* dark clusters in one cell of a die atlas: 4-connected, at least `min` texels */
static int clusters(const uint8_t *atlas, int cell, int min)
{
    enum { S = CN_TEX_DIE_CELL };
    static uint8_t seen[S * S]; static int stack[S * S];
    memset(seen, 0, sizeof seen);
    int n = 0;
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        if (seen[y * S + x] || !dark(atlas + ((size_t)y * CN_TEX_DIE_W + cell * S + x) * 4)) continue;
        int sp = 0, size = 0; stack[sp++] = y * S + x; seen[y * S + x] = 1;
        while (sp) {
            int i = stack[--sp], cx = i % S, cy = i / S; size++;
            const int nb[4][2] = { { cx - 1, cy }, { cx + 1, cy }, { cx, cy - 1 }, { cx, cy + 1 } };
            for (int k = 0; k < 4; k++) {
                int X = nb[k][0], Y = nb[k][1];
                if (X < 0 || Y < 0 || X >= S || Y >= S || seen[Y * S + X]) continue;
                if (!dark(atlas + ((size_t)Y * CN_TEX_DIE_W + cell * S + X) * 4)) continue;
                seen[Y * S + X] = 1; stack[sp++] = Y * S + X;
            }
        }
        if (size >= min) n++;
    }
    return n;
}

/* a sink over malloc, as the scene's scene_tex_new would be */
typedef struct { int n, fail; uint8_t *rgba[8]; int8_t *bump[8]; } Mock;
static int mock_alloc(void *ctx, int w, int h, int has_bump, CnTexImage *img)
{
    Mock *m = ctx;
    if (m->fail || m->n >= 8) return -1;
    img->w = w; img->h = h;
    img->rgba = m->rgba[m->n] = malloc((size_t)w * h * 4);
    img->bump = m->bump[m->n] = has_bump ? malloc((size_t)w * h * 2) : 0;
    return m->n++;
}

int main(int argc, char **argv)
{
    const char *font = argc > 1 ? argv[1] : "tools/fonts/IMFeENrm28P.ttf";
    const char *file = argc > 2 ? argv[2] : "build/cn_tex.pack";
    static uint8_t A[CN_TEX_SIDE_W * CN_TEX_SIDE_H * 4], B[CN_TEX_SIDE_W * CN_TEX_SIDE_H * 4];
    static int8_t BA[CN_TEX_SIDE_W * CN_TEX_SIDE_H * 2], BB[CN_TEX_SIDE_W * CN_TEX_SIDE_H * 2];

    TEST("hash is the study's");
    /* TEX.hash's numerators, from the JavaScript run in node */
    CHECK(cn_tex_hash(0, 0, 0) == 0u, "%u", cn_tex_hash(0, 0, 0));
    CHECK(cn_tex_hash(7, 91, 977) == 1420941503u, "%u", cn_tex_hash(7, 91, 977));
    CHECK(cn_tex_hash(42, 96, 977) == 2074240742u, "%u", cn_tex_hash(42, 96, 977));
    CHECK(cn_tex_hash(3, 4, 911) == 568668281u, "%u", cn_tex_hash(3, 4, 911));
    CHECK(cn_tex_hash(123456, 654321, 37) == 3057492463u, "%u", cn_tex_hash(123456, 654321, 37));

    TEST("bake is deterministic");
    uint32_t len = 0, len2 = 0;
    uint8_t *pk = cn_texgen_pack(font, &len), *pk2 = cn_texgen_pack(font, &len2);
    CHECK(pk && pk2, "the baker read no font at %s", font);
    if (!pk || !pk2) return report("cn_tex_test");
    CHECK(len == len2 && !memcmp(pk, pk2, len), "two bakes differ");
    CHECK(len == GOLD_PACK_LEN, "pack length %u", len);
    CHECK(cn_tex_fnv1a(pk, len) == GOLD_PACK, "pack golden %08x", cn_tex_fnv1a(pk, len));

    TEST("size constants");
    CHECK(len < PACK_BUDGET, "pack %u B over the %u B budget", len, PACK_BUDGET);
    CHECK(CN_TEX_DIE_W == 6 * CN_TEX_DIE_CELL && CN_TEX_SIDE_W == 4 * CN_TEX_VERD && CN_TEX_SIDE_H == 2 * CN_TEX_VERD, "sizes");
    CHECK(CN_TEX_CROWN == CN_TEX_VERD, "the crown is one tile");
    int w, h;
    cn_tex_size(CN_TEX_SIDE, &w, &h); CHECK(w == 1024 && h == 512, "side %dx%d", w, h);
    cn_tex_size(CN_TEX_CROWN_T, &w, &h); CHECK(w == 256 && h == 256, "crown %dx%d", w, h);
    cn_tex_size(CN_TEX_DIE, &w, &h); CHECK(w == 768 && h == 128, "die %dx%d", w, h);

    TEST("pack round trip");
    CnTexPack p;
    CHECK(cn_tex_pack_open(&p, pk, len) == CN_TEX_OK, "open");
    CHECK(le32(pk) == CN_TEX_MAGIC && le16(pk + 6) == CN_TEX_ENTRIES && le32(pk + 8) == len, "header");
    /* the layout is what the header says: the records, then 4-aligned blocks, nothing after the last */
    uint32_t expect = CN_TEX_HEAD_BYTES + CN_TEX_ENTRIES * CN_TEX_ENTRY_BYTES;
    expect = ((expect + 3) & ~3u) + CN_TEX_VERD * CN_TEX_VERD * 3; expect = ((expect + 3) & ~3u) + CN_TEX_BONE * CN_TEX_BONE * 3;
    for (int s = 0; s < CN_TEX_GLYPH_SIZES; s++) for (int d = 0; d < CN_TEX_DIGITS; d++) expect = ((expect + 3) & ~3u) + (uint32_t)(p.glyph[s][d].w * p.glyph[s][d].h);
    CHECK(((expect + 3) & ~3u) == len, "layout %u against %u", expect, len);
    for (int s = 0; s < CN_TEX_GLYPH_SIZES; s++) for (int d = 0; d < CN_TEX_DIGITS; d++) {
        const CnTexGlyph *g = &p.glyph[s][d];
        CHECK(g->w > 10 && g->h > 10 && g->w < 160 && g->h < 200, "glyph %d/%d %dx%d", s, d, g->w, g->h);
    }
    /* the file `make tex` wrote is this bake */
    size_t fl = 0; uint8_t *fb = read_file(file, &fl);
    CHECK(fb && fl == len && !memcmp(fb, pk, len), "%s is not this bake (make tex)", file);
    free(fb);
    /* every damage is refused */
    pk[len - 5] ^= 1; CHECK(cn_tex_pack_open(&p, pk, len) == CN_TEX_E_CHECK, "a flipped texel"); pk[len - 5] ^= 1;
    pk[0] ^= 1; CHECK(cn_tex_pack_open(&p, pk, len) == CN_TEX_E_MAGIC, "magic"); pk[0] ^= 1;
    pk[4] ^= 2; CHECK(cn_tex_pack_open(&p, pk, len) == CN_TEX_E_VERSION, "version"); pk[4] ^= 2;
    CHECK(cn_tex_pack_open(&p, pk, len - 4) == CN_TEX_E_SHORT, "truncated");
    CHECK(cn_tex_pack_open(&p, pk, 8) == CN_TEX_E_SHORT, "a stub");
    CHECK(cn_tex_pack_open(&p, pk, len) == CN_TEX_OK, "reopen");

    TEST("same seed same texture");
    cn_tex_cup_side(&p, 7, A); cn_tex_cup_side(&p, 7, B);
    CHECK(!memcmp(A, B, sizeof A), "side 7 twice");
    uint32_t side7 = fnv(A, sizeof A);
    cn_tex_cup_side(&p, 8, B);
    CHECK(memcmp(A, B, sizeof A), "seeds 7 and 8 give one side");
    CHECK(side7 == GOLD_SIDE7, "side golden %08x", side7);
    cn_tex_cup_crown(&p, 7, 3, 0, CN_TEX_NUMERAL_SMALL, A);
    CHECK(fnv(A, CN_TEX_CROWN * CN_TEX_CROWN * 4) == GOLD_CROWN7, "crown golden %08x", fnv(A, CN_TEX_CROWN * CN_TEX_CROWN * 4));
    cn_tex_die_atlas(&p, 7, A); cn_tex_die_relief(A, 7, BA);
    CHECK(fnv(A, CN_TEX_DIE_W * CN_TEX_DIE_CELL * 4) == GOLD_DIE7, "die golden %08x", fnv(A, CN_TEX_DIE_W * CN_TEX_DIE_CELL * 4));
    CHECK(fnv(BA, CN_TEX_DIE_W * CN_TEX_DIE_CELL * 2) == GOLD_DIE7_BUMP, "die bump golden %08x", fnv(BA, CN_TEX_DIE_W * CN_TEX_DIE_CELL * 2));
    /* a side is the verdigris tile at the seed's offset: every texel above the grime is the tile's */
    cn_tex_cup_side(&p, 7, A);
    {
        int ox = (int)(cn_tex_hash(7, 91, 977) >> 24), oy = (int)(cn_tex_hash(7, 92, 977) >> 24), bad = 0;
        for (int y = 0; y < 460; y++) for (int x = 0; x < CN_TEX_SIDE_W; x++)
            bad += memcmp(A + ((size_t)y * CN_TEX_SIDE_W + x) * 4, p.verd + (((y + oy) & 255) * 256 + ((x + ox) & 255)) * 3, 3) != 0;
        CHECK(bad == 0, "%d texels are not the tile's", bad);
    }

    TEST("numeral lands inside the crown");
    for (int s = 0; s < CN_TEX_GLYPH_SIZES; s++) for (int d = 0; d < CN_TEX_DIGITS; d++) {
        int px = s ? CN_TEX_NUMERAL_LARGE : CN_TEX_NUMERAL_SMALL, changed = 0, outside = 0;
        cn_tex_cup_crown(&p, 11, -1, 0, px, A);
        cn_tex_cup_crown(&p, 11, d, 0, px, B);
        double sx = 0, sy = 0;
        for (int y = 0; y < CN_TEX_CROWN; y++) for (int x = 0; x < CN_TEX_CROWN; x++) {
            size_t i = ((size_t)y * CN_TEX_CROWN + x) * 4;
            if (!memcmp(A + i, B + i, 4)) continue;
            changed++; sx += x; sy += y;
            /* the crown is the texture's inscribed circle (the mesh's disc maps .5 + .5 cos, .5 + .5 sin); the 36-gon is inside 127.5 */
            double dx = x + .5 - 128, dy = y + .5 - 128;
            if (dx * dx + dy * dy > 120.0 * 120.0) outside++;
        }
        CHECK(outside == 0, "digit %d at %d: %d texels past the crown", d, px, outside);
        CHECK(changed > (s ? 1500 : 500), "digit %d at %d: only %d texels stamped", d, px, changed);
        if (changed) CHECK((sx / changed - 128) * (sx / changed - 128) + (sy / changed - 136) * (sy / changed - 136) < 30 * 30,
                           "digit %d at %d: centred at %.1f, %.1f", d, px, sx / changed, sy / changed);
    }
    CHECK(!cn_tex_stamp_numeral(&p, A, 256, 256, 10, CN_TEX_NUMERAL_SMALL), "digit 10");
    CHECK(!cn_tex_stamp_numeral(&p, A, 256, 256, 3, 100), "an unbaked size");
    /* out: the dark tint and no numeral, whatever the count */
    cn_tex_cup_crown(&p, 11, -1, 1, 112, A); cn_tex_cup_crown(&p, 11, 4, 1, 112, B);
    CHECK(!memcmp(A, B, CN_TEX_CROWN * CN_TEX_CROWN * 4), "an out crown carries a numeral");
    {
        cn_tex_cup_crown(&p, 11, -1, 0, 112, B); long la = 0, lb = 0;
        for (int i = 0; i < CN_TEX_CROWN * CN_TEX_CROWN; i++) { la += A[i * 4] + A[i * 4 + 1] + A[i * 4 + 2]; lb += B[i * 4] + B[i * 4 + 1] + B[i * 4 + 2]; }
        CHECK(la * 10 < lb * 6, "out is not dark: %ld against %ld", la, lb);
    }

    TEST("die cell k has k pips");
    for (uint32_t seed = 1; seed <= 40; seed++) {
        cn_tex_die_atlas(&p, seed, A);
        for (int k = 1; k <= 6; k++) {
            int n = clusters(A, k - 1, 40);
            CHECK(n == k, "seed %u face %d: %d pips", seed, k, n);
        }
    }
    /* the 1 is blood, the others black */
    cn_tex_die_atlas(&p, 3, A);
    {
        const uint8_t *one = A + ((size_t)64 * CN_TEX_DIE_W + 64) * 4;
        CHECK(one[0] > one[1] + 20, "the 1's pip is not red: %d %d %d", one[0], one[1], one[2]);
    }

    TEST("relief");
    memset(A, 90, CN_TEX_CROWN * CN_TEX_CROWN * 4);
    cn_tex_relief(A, 256, 256, CN_TEX_RELIEF_CUP, BA);
    { int nz = 0; for (int i = 0; i < 256 * 256 * 2; i++) nz += BA[i] != 0; CHECK(nz == 0, "a flat texture has %d slopes", nz); }
    /* a ramp of 16 red a texel is 8 of height a texel, 16 across the pair: -(16) / 255 * 1.2 * 20 = -1.51 -> -2;
     * at the edge the left neighbour is the texel itself, 8 across: -0.75 -> -1 */
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) A[(y * 16 + x) * 4] = (uint8_t)(x * 16);
    cn_tex_relief(A, 16, 16, CN_TEX_RELIEF_CUP, BA);
    CHECK(BA[(5 * 16 + 5) * 2] == -2 && BA[(5 * 16 + 5) * 2 + 1] == 0, "ramp slope %d %d", BA[(5 * 16 + 5) * 2], BA[(5 * 16 + 5) * 2 + 1]);
    CHECK(BA[(5 * 16 + 0) * 2] == -1, "the edge clamps: %d", BA[(5 * 16) * 2]);

    TEST("mip");
    cn_tex_cup_floor(&p, 5, A); cn_tex_relief(A, 256, 256, CN_TEX_RELIEF_CUP, BA);
    cn_tex_mip(A, BA, 256, 256, B, BB);
    {
        int bad = 0;
        for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) for (int c = 0; c < 4; c++) {
            int s = A[((2 * y) * 256 + 2 * x) * 4 + c] + A[((2 * y) * 256 + 2 * x + 1) * 4 + c] + A[((2 * y + 1) * 256 + 2 * x) * 4 + c] + A[((2 * y + 1) * 256 + 2 * x + 1) * 4 + c];
            bad += B[(y * 128 + x) * 4 + c] != (s + 2) / 4;
        }
        CHECK(bad == 0, "%d texels are not the rounded mean", bad);
        int8_t q = BB[(10 * 128 + 10) * 2], e = (int8_t)((BA[(20 * 256 + 20) * 2] + BA[(20 * 256 + 21) * 2] + BA[(21 * 256 + 20) * 2] + BA[(21 * 256 + 21) * 2]) / 4);
        CHECK(q == e, "bump mean %d against %d", q, e);
    }

    TEST("upload through a sink");
    {
        Mock m = { 0 };
        CnTexSink sink = { &m, mock_alloc };
        CnTexSpec s1 = { CN_TEX_CROWN_T, 9, 2, 0, CN_TEX_NUMERAL_LARGE, 1 }, s2 = { CN_TEX_DIE, 9, 0, 0, 0, 1 }, s3 = { CN_TEX_SIDE, 9, 0, 0, 0, 0 };
        int a = cn_tex_upload(&p, &sink, &s1), b = cn_tex_upload(&p, &sink, &s2), c = cn_tex_upload(&p, &sink, &s3);
        CHECK(a == 0 && b == 1 && c == 2, "ids %d %d %d", a, b, c);
        cn_tex_cup_crown(&p, 9, 2, 0, CN_TEX_NUMERAL_LARGE, A); cn_tex_relief(A, 256, 256, CN_TEX_RELIEF_CUP, BA);
        CHECK(!memcmp(m.rgba[0], A, 256 * 256 * 4) && !memcmp(m.bump[0], BA, 256 * 256 * 2), "crown upload");
        cn_tex_die_atlas(&p, 9, A); cn_tex_die_relief(A, 9, BA);
        CHECK(!memcmp(m.rgba[1], A, 768 * 128 * 4) && !memcmp(m.bump[1], BA, 768 * 128 * 2), "die upload");
        cn_tex_cup_side(&p, 9, A);
        CHECK(!memcmp(m.rgba[2], A, sizeof A) && !m.bump[2], "side upload");
        m.fail = 1;
        CHECK(cn_tex_upload(&p, &sink, &s1) == -1, "a full scene");
        for (int i = 0; i < m.n; i++) { free(m.rgba[i]); free(m.bump[i]); }
    }

    free(pk); free(pk2);

    /* THE PLANKS (cn_planks.jpg). No golden here: the march runs libm's cos 25 times a row through a
     * chaotic map, so two C libraries may part in the last bits; the capture comparison
     * (`cn_texgen --compare-planks`, MAD 0.38 of 255 against the study's canvas) is the oracle for the
     * look, and these pin the structure the screen relies on. */
    TEST("planks: six planks, seamless, a running bond, nails, the deep palette");
    {
        enum { S = CN_TEXGEN_PLANK_SCALE, W = TG_TILE_W * S, H = TG_TILE_H * S };
        uint8_t *pl = malloc((size_t)W * H * 3), *bare = malloc((size_t)W * H * 3), *again = malloc((size_t)W * H * 3);
        cn_texgen_planks(S, 1, pl); cn_texgen_planks(S, 0, bare); cn_texgen_planks(S, 1, again);
        CHECK(!memcmp(pl, again, (size_t)W * H * 3), "two bakes of the planks differ");
#define LUM(img, x, y) ((img)[((size_t)(y) * W + (x)) * 3] + (img)[((size_t)(y) * W + (x)) * 3 + 1] + (img)[((size_t)(y) * W + (x)) * 3 + 2])
        /* the column's mean brightness over the whole height */
        double col[W];
        for (int x = 0; x < W; x++) { long s = 0; for (int y = 0; y < H; y++) s += LUM(bare, x, y); col[x] = (double)s / H; }
        /* a gap at every plank edge, the tile's two edges included, and none down a plank's middle */
        for (int k = 0; k <= TG_PLANKS; k++) {
            int xe = k * TG_PLANK_W * S; if (xe >= W) xe = W - 1;
            double mid = col[((k < TG_PLANKS ? k : k - 1) * TG_PLANK_W + TG_PLANK_W / 2) * S];
            CHECK(col[xe] < mid - 8, "plank edge %d (x %d) is not a gap: %.1f against the middle's %.1f", k, xe, col[xe], mid);
        }
        for (int k = 0; k < TG_PLANKS; k++) {
            int xm = (k * TG_PLANK_W + TG_PLANK_W / 2) * S;
            CHECK(col[xm] > col[xm - 20 * S] - 6 && col[xm] > 30, "plank %d's middle is dark: %.1f", k, col[xm]);
        }
        /* seamless: the step across each wrap is no bigger than the steps inside the tile */
        double adjx = 0, adjy = 0, wrapx = 0, wrapy = 0;
        for (int y = 0; y < H; y++) { for (int x = 1; x < W; x++) adjx += abs(LUM(bare, x, y) - LUM(bare, x - 1, y)); wrapx += abs(LUM(bare, 0, y) - LUM(bare, W - 1, y)); }
        for (int x = 0; x < W; x++) { for (int y = 1; y < H; y++) adjy += abs(LUM(bare, x, y) - LUM(bare, x, y - 1)); wrapy += abs(LUM(bare, x, 0) - LUM(bare, x, H - 1)); }
        adjx /= (double)(W - 1) * H; adjy /= (double)(H - 1) * W; wrapx /= H; wrapy /= W;
        CHECK(wrapx < 2 * adjx + 1, "the left and right edges part: %.2f a texel against %.2f inside", wrapx, adjx);
        CHECK(wrapy < 2 * adjy + 1, "the top and bottom edges part: %.2f a texel against %.2f inside", wrapy, adjy);
        /* the running bond: an even plank ends at the tile's top, an odd one halfway down */
        for (int k = 0; k < TG_PLANKS; k++) {
            int x0 = (k * TG_PLANK_W + 20) * S, x1 = ((k + 1) * TG_PLANK_W - 20) * S, darkest = -1; double low = 1e9;
            for (int y = 0; y < H; y++) { long s = 0; for (int x = x0; x < x1; x++) s += LUM(bare, x, y); if (s < low) { low = (double)s; darkest = y; } }
            int want = k % 2 ? H / 2 : 0, d = abs(darkest - want); if (d > H / 2) d = H - d;
            CHECK(d <= 6 * S, "plank %d ends at row %d, not %d", k, darkest, want);
        }
        /* the deep palette: dark, and green-blue rather than brown (no foolish walnut) */
        {
            double r = 0, g = 0, b = 0;
            for (size_t i = 0; i < (size_t)W * H; i++) { r += bare[i * 3]; g += bare[i * 3 + 1]; b += bare[i * 3 + 2]; }
            r /= (double)W * H; g /= (double)W * H; b /= (double)W * H;
            CHECK(r + g + b > 3 * 8 && r + g + b < 3 * 32, "the planks' mean is %.1f %.1f %.1f", r, g, b);
            CHECK(r < g && r < b, "the planks lean warm: %.1f %.1f %.1f", r, g, b);
        }
        /* the nails: a dark rose head at every place the rows put one, twelve to a row */
        {
            double ys[16]; int rows = tg_nail_rows(ys, 16);
            CHECK(rows >= 7 && rows <= 9, "%d rows of nails in 830 points", rows);
            for (int i = 1; i < rows; i++) CHECK(ys[i] - ys[i - 1] >= 92 && ys[i] - ys[i - 1] <= 116, "rows %d and %d are %.1f apart", i - 1, i, ys[i] - ys[i - 1]);
            CHECK(TG_TILE_H + ys[0] - ys[rows - 1] >= 92, "the seam crowds the rows: %.1f", TG_TILE_H + ys[0] - ys[rows - 1]);
            int heads = 0, missed = 0;
            for (int ri = 0; ri < rows; ri++) for (int i = 0; i < TG_PLANKS; i++) for (int e = 0; e < 2; e++) {
                double x = e ? (i + 1) * TG_PLANK_W - 7.0 : i * TG_PLANK_W + 7.0, y = ys[ri];
                double nx = x + (tg_hh(i, (int)y, 12) - .5) * 2.5, ny = y + (tg_hh((int)y, (int)(i + x), 12) - .5) * 5;
                int cx = (int)(nx * S), cy = (int)(ny * S), changed = 0;
                for (int dy = -2 * S; dy <= 2 * S; dy++) for (int dx = -2 * S; dx <= 2 * S; dx++) {
                    int px = ((cx + dx) % W + W) % W, py = ((cy + dy) % H + H) % H;
                    changed += memcmp(pl + ((size_t)py * W + px) * 3, bare + ((size_t)py * W + px) * 3, 3) != 0;
                }
                if (changed > 8 * S * S) heads++; else missed++;
            }
            CHECK(missed == 0 && heads == rows * 12, "%d nail heads, %d missing", heads, missed);
            /* and nowhere else: far from every head the two bakes agree */
            long stray = 0;
            for (int y = 0; y < H; y++) {
                int near = 0; for (int ri = 0; ri < rows; ri++) { double d = y / (double)S - ys[ri]; if (d > -12 && d < 12) near = 1; }
                if (!near) stray += memcmp(pl + (size_t)y * W * 3, bare + (size_t)y * W * 3, (size_t)W * 3) != 0;
            }
            CHECK(stray == 0, "%ld rows away from the nails changed", stray);
        }
#undef LUM
        free(pl); free(bare); free(again);
    }
    return report("cn_tex_test");
}
