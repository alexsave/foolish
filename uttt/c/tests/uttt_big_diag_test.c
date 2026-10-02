/* The 243 diagnostics (src/uttt_big_diag.h): the JPEG facts, the greys, the
 * history ring and the report.
 *
 *     make -C uttt/c run        (./build/uttt_big_diag_test; and asan)
 *
 * THE JPEGS ARE REAL FILES (tests/fixtures), each with a known truth:
 *   messages_sim_q050_243.jpeg  what Messages itself wrote for a bubble's
 *       picture on the iOS 27 simulator (the layout probe, 2026-10-01): the
 *       sender's extension's own encoder, ImageIO at 0.50, 4:2:0, 243 x 243.
 *   imageio_*.jpg  written by ImageIO on macOS 27 (CGImageDestination, the
 *       call UIImageJPEGRepresentation makes) from one 96 x 99 picture at
 *       0.50, 0.89, 0.915 and 1.00 (ImageIO's 1.00 is the one 4:4:4 it
 *       writes), progressive at 0.50, and a one-component grey at 0.50.
 *   cjpeg_*.jpg  libjpeg-turbo's cjpeg (Homebrew) from that picture:
 *       -quality 50 -sample 1x1 (4:4:4), -quality 75 -sample 2x1 (4:2:2),
 *       -quality 90 -progressive (4:2:0): an encoder that is not ImageIO.
 *   make_fixtures.swift beside them says how each was made.
 * A real transport's files (quality 0.89 from the phone's transcoder) were
 * not available to this test; tests/uttt_big_diag_imageio.c writes ImageIO
 * JPEGs at every step on a Mac and holds the parser and the tables to them.
 *
 * THE GREYS are held to the kit's own bd_sample (linked here): the same
 * cells, the same classes, the same risky count and smallest margin, on
 * painted boards with noise and at sizes the picture could come back at.
 */
#include "../src/uttt_big_diag.h"
#include "../src/uttt_big_msg.h"
#include "../../../shared/c/test/check.h"
#include "../../../shared/swift/BubbleDataKit/Sources/CBubbleData/include/bubble_data.h"
#include <math.h>
#include <stdint.h>

#ifndef FIXTURES
#define FIXTURES "tests/fixtures/"
#endif

static uint8_t *slurp(const char *name, long *n)
{
    char path[512];
    snprintf(path, sizeof path, "%s%s", FIXTURES, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { fprintf(stderr, "short read %s\n", path); exit(2); }
    fclose(f);
    return b;
}

static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ------------------------------------------------------------- the JPEGs */

typedef struct {
    const char *file;
    int w, h, comps, sof;
    const char *chroma;
    int exact, q_lo, q_hi;     /* exact: the ImageIO steps, thousandths       */
    int ijg_lo, ijg_hi;        /* the libjpeg scaling's quality, tenths        */
} Truth;

static const Truth truths[] = {
    { "messages_sim_q050_243.jpeg", 243, 243, 3, 0xC0, "4:2:0", 1, 500, 500, -1, -1 },
    { "imageio_q050_420.jpg",        96,  99, 3, 0xC0, "4:2:0", 1, 500, 500, -1, -1 },
    { "imageio_q089_420.jpg",        96,  99, 3, 0xC0, "4:2:0", 1, 890, 910, -1, -1 },
    { "imageio_q100_444.jpg",        96,  99, 3, 0xC0, "4:4:4", 1, 1000, 1000, -1, -1 },
    { "imageio_q050_progressive.jpg", 96, 99, 3, 0xC2, "4:2:0", 1, 500, 500, -1, -1 },
    { "imageio_q050_grey.jpg",       96,  99, 1, 0xC0, "grey",  1, 500, 500, -1, -1 },
    { "cjpeg_q50_444.jpg",           96,  99, 3, 0xC0, "4:4:4", 0, -1, -1, 500, 500 },
    { "cjpeg_q75_422.jpg",           96,  99, 3, 0xC0, "4:2:2", 0, -1, -1, 745, 755 },
    { "cjpeg_q90_420_progressive.jpg", 96, 99, 3, 0xC2, "4:2:0", 0, -1, -1, 895, 905 },
};

static void test_fixtures(void)
{
    TEST("jpeg: real files read to their known truth");
    for (size_t i = 0; i < sizeof truths / sizeof truths[0]; i++) {
        const Truth *t = &truths[i];
        long n;
        uint8_t *b = slurp(t->file, &n);
        UbdJpeg j;
        int r = ubd_jpeg_parse(b, n, &j);
        CHECK(r == UBD_JPEG_OK, "%s: status %d", t->file, r);
        CHECK(j.width == t->w && j.height == t->h, "%s: %d x %d", t->file, j.width, j.height);
        CHECK(j.components == t->comps, "%s: %d components", t->file, j.components);
        CHECK(j.sof == t->sof, "%s: sof %x", t->file, j.sof);
        CHECK(j.precision == 8, "%s: precision %d", t->file, j.precision);
        CHECK(!strcmp(ubd_jpeg_chroma(&j), t->chroma), "%s: chroma %s", t->file, ubd_jpeg_chroma(&j));
        CHECK(j.has_luma_table && j.luma_bits == 8, "%s: luma table", t->file);
        TEST("jpeg: an ImageIO file's quality is exact and its range right");
        CHECK(j.q_exact == t->exact, "%s: exact %d", t->file, j.q_exact);
        if (t->exact) {
            CHECK(j.q_lo == t->q_lo && j.q_hi == t->q_hi && j.q_est == t->q_lo,
                  "%s: q %d..%d est %d", t->file, j.q_lo, j.q_hi, j.q_est);
        } else {
            TEST("jpeg: a libjpeg file's quality by the scaling");
            CHECK(j.q_ijg >= t->ijg_lo && j.q_ijg <= t->ijg_hi, "%s: ijg %d", t->file, j.q_ijg);
        }
        TEST("jpeg: real files read to their known truth");
        free(b);
    }

    /* 0.915 writes a table no step does (ImageIO takes the quality as a
     * continuous number): no exact match, and the interpolation lands near. */
    TEST("jpeg: a quality between ImageIO's steps interpolates near it");
    {
        long n;
        uint8_t *b = slurp("imageio_q0915_420.jpg", &n);
        UbdJpeg j;
        CHECK(ubd_jpeg_parse(b, n, &j) == UBD_JPEG_OK, "parse");
        CHECK(!j.q_exact && j.q_est >= 910 && j.q_est <= 920, "exact %d lo %d hi %d est %d",
              j.q_exact, j.q_lo, j.q_hi, j.q_est);
        free(b);
    }
}

/* A table between two steps reads between them; a table at a step reads
 * exact; the ends clamp. */
static void test_estimate(void)
{
    TEST("jpeg: ImageIO's sums never rise with the quality");
    for (int p = 0; p < 100; p++) {
        int a = 0, b = 0;
        for (int k = 0; k < 64; k++) { a += ubd_imageio_luma(p)[k]; b += ubd_imageio_luma(p + 1)[k]; }
        CHECK(b <= a, "step %d sum %d then %d", p, a, b);
    }
    CHECK(ubd_imageio_luma(-1) == NULL && ubd_imageio_luma(101) == NULL, "out of range");

    /* Build a minimal JPEG around a table and parse it. */
    TEST("jpeg: a table between 0.60 and 0.61 interpolates between them");
    {
        uint8_t f[2 + 4 + 65 + 2 + 17 + 4];
        int at = 0;
        f[at++] = 0xFF; f[at++] = 0xD8;
        f[at++] = 0xFF; f[at++] = 0xDB; f[at++] = 0; f[at++] = 67; f[at++] = 0x00;
        const uint8_t *a = ubd_imageio_luma(60), *b = ubd_imageio_luma(61);
        for (int k = 0; k < 64; k++) f[at++] = (uint8_t)(k < 32 ? a[k] : b[k]);
        f[at++] = 0xFF; f[at++] = 0xC0; f[at++] = 0; f[at++] = 17;
        f[at++] = 8; f[at++] = 0; f[at++] = 16; f[at++] = 0; f[at++] = 16; f[at++] = 3;
        f[at++] = 1; f[at++] = 0x22; f[at++] = 0; f[at++] = 2; f[at++] = 0x11; f[at++] = 1;
        f[at++] = 3; f[at++] = 0x11; f[at++] = 1;
        f[at++] = 0xFF; f[at++] = 0xD9;
        UbdJpeg j;
        CHECK(ubd_jpeg_parse(f, at, &j) == UBD_JPEG_OK, "parse");
        CHECK(!j.q_exact && j.q_est > 600 && j.q_est < 610, "est %d", j.q_est);
        CHECK(!strcmp(ubd_jpeg_chroma(&j), "4:2:0"), "chroma");

        TEST("jpeg: a table past either end clamps");
        for (int k = 0; k < 64; k++) f[7 + k] = 255;
        CHECK(ubd_jpeg_parse(f, at, &j) == UBD_JPEG_OK && j.q_est == 0 && !j.q_exact, "est %d", j.q_est);
        for (int k = 0; k < 64; k++) f[7 + k] = k ? 1 : 0;   /* a sum of 63, under 1.00's 64 */
        CHECK(ubd_jpeg_parse(f, at, &j) == UBD_JPEG_OK && j.q_est == 1000, "est %d", j.q_est);

        TEST("jpeg: a frame with no table for luma has no quality");
        f[6] = 0x01;                                          /* the table is id 1 now */
        CHECK(ubd_jpeg_parse(f, at, &j) == UBD_JPEG_OK && !j.has_luma_table && j.q_est == -1, "luma");

        TEST("jpeg: no frame before the scan is refused");
        uint8_t g[] = { 0xFF, 0xD8, 0xFF, 0xDA, 0, 2, 0xFF, 0xD9 };
        CHECK(ubd_jpeg_parse(g, sizeof g, &j) == UBD_JPEG_NO_FRAME, "status %d", j.status);
        TEST("jpeg: not a JPEG");
        uint8_t png[] = { 0x89, 'P', 'N', 'G', 0, 0 };
        CHECK(ubd_jpeg_parse(png, sizeof png, &j) == UBD_JPEG_NOT_JPEG, "png");
        CHECK(ubd_jpeg_parse(NULL, 10, &j) == UBD_JPEG_NOT_JPEG, "null");
    }
}

/* Every prefix and every single-bit flip of a real file's head: the parser
 * never reads outside the bytes (asan) and never calls a cut head whole. */
static void test_hostile(void)
{
    TEST("jpeg: every truncation of the head is refused, never misread");
    long n;
    uint8_t *b = slurp("messages_sim_q050_243.jpeg", &n);
    UbdJpeg whole;
    ubd_jpeg_parse(b, n, &whole);
    /* where the scan starts: the head is everything before it */
    long sos = 2;
    while (sos + 4 <= n && !(b[sos] == 0xFF && b[sos + 1] == 0xDA)) sos += 2 + ((b[sos + 2] << 8) | b[sos + 3]);
    CHECK(sos > 100 && sos < n, "sos at %ld", sos);
    for (long k = 0; k < sos + 4; k++) {
        uint8_t *cut = malloc((size_t)(k ? k : 1));
        memcpy(cut, b, (size_t)k);
        UbdJpeg j;
        int r = ubd_jpeg_parse(cut, k, &j);
        CHECK(r != UBD_JPEG_OK || k > sos, "a cut at %ld read whole", k);
        free(cut);
    }
    TEST("jpeg: every single-bit flip in the head is survived");
    int changed = 0;
    for (long k = 0; k < sos; k++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t *c = malloc((size_t)n);
            memcpy(c, b, (size_t)n);
            c[k] ^= (uint8_t)(1u << bit);
            UbdJpeg j;
            int r = ubd_jpeg_parse(c, n, &j);
            if (r == UBD_JPEG_OK && (j.width != whole.width || j.q_est != whole.q_est || j.h[0] != whole.h[0])) changed++;
            CHECK(r <= 0 && r >= UBD_JPEG_BAD_FRAME, "status %d", r);
            free(c);
        }
    }
    CHECK(changed > 0, "no flip changed a fact: the test reads nothing");
    free(b);
}

/* ------------------------------------------------------------- the greys */

static uint8_t sym[UTB_CELLS];
static uint8_t cells[243 * 244];
static uint8_t paint[729 * 732 * 4];

/* A random board with `density` per mille marks, painted at 3 px a cell. */
static void painted(int density)
{
    for (int i = 0; i < UTB_CELLS; i++) {
        uint32_t r = rnd() % 1000;
        sym[i] = r < (uint32_t)density ? (uint8_t)(1 + (rnd() & 1)) : 0;
    }
    if (bd_frame(sym, UTB_CELLS, BD_KIND_SYMBOLS, 0, 243, cells) != BD_EOK) { fprintf(stderr, "frame\n"); exit(2); }
    if (bd_paint(cells, 243, 3, paint) != BD_EOK) { fprintf(stderr, "paint\n"); exit(2); }
}

/* `src` (sw x sh) scaled to dw x dh by nearest pixel, with noise of +/- amp
 * on every channel of every pixel. */
static uint8_t *resized(const uint8_t *src, int sw, int sh, int dw, int dh, int amp)
{
    uint8_t *d = malloc((size_t)dw * dh * 4);
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            const uint8_t *s = src + (((long)y * sh / dh) * sw + (long)x * sw / dw) * 4;
            uint8_t *o = d + ((long)y * dw + x) * 4;
            for (int c = 0; c < 3; c++) {
                int v = s[c] + (amp ? (int)(rnd() % (2 * amp + 1)) - amp : 0);
                o[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
            o[3] = 255;
        }
    return d;
}

static void test_greys_match_the_kit(void)
{
    TEST("greys: the classes, risky and margin are the kit's own");
    static const int sizes[][2] = { {729, 732}, {729, 729}, {700, 705}, {1200, 1200}, {486, 488}, {243, 244} };
    static const int amps[] = { 0, 40, 70, 127 };
    uint8_t grid[243 * 244];
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
        for (size_t a = 0; a < sizeof amps / sizeof amps[0]; a++) {
            painted(a == 3 ? 500 : 40);
            int w = sizes[s][0], h = sizes[s][1];
            uint8_t *pic = resized(paint, 729, 732, w, h, amps[a]);
            BdReading kit;
            CHECK(bd_sample(pic, w, h, 243, grid, &kit) == BD_EOK, "kit sample");
            UbdLuma l;
            CHECK(ubd_luma(pic, w, h, 243, &l) == 0, "ubd_luma");
            int count[3] = { 0, 0, 0 };
            for (int i = 0; i < 243 * 244; i++) count[grid[i]]++;
            CHECK(l.cells == kit.cells && l.risky == kit.risky && l.min_margin == kit.min_margin,
                  "%dx%d amp %d: cells %d/%d risky %d/%d margin %d/%d", w, h, amps[a],
                  l.cells, kit.cells, l.risky, kit.risky, l.min_margin, kit.min_margin);
            CHECK(l.cls[UBD_CLASS_EMPTY].n == count[0] && l.cls[UBD_CLASS_X].n == count[1] &&
                  l.cls[UBD_CLASS_O].n == count[2], "%dx%d amp %d: classes %d %d %d against %d %d %d",
                  w, h, amps[a], l.cls[0].n, l.cls[1].n, l.cls[2].n, count[0], count[1], count[2]);
            int hist = 0;
            for (int b = 0; b < UBD_BUCKETS; b++) hist += l.hist[b];
            CHECK(hist == l.cells, "histogram holds %d of %d", hist, l.cells);
            free(pic);
        }

    TEST("greys: a size the kit refuses is refused");
    UbdLuma l;
    CHECK(ubd_luma(paint, 242, 244, 243, &l) == -1, "narrow");
    CHECK(ubd_luma(paint, 243, 243, 243, &l) == -1, "short");
    CHECK(ubd_luma(paint, 729, 732, 63, &l) == -1, "side 63");
    CHECK(ubd_luma(NULL, 729, 732, 243, &l) == -1, "null");
}

static void test_greys_numbers(void)
{
    TEST("greys: a picture as painted is exactly its levels");
    painted(40);
    UbdLuma l;
    CHECK(ubd_luma(paint, 729, 732, 243, &l) == 0, "luma");
    CHECK(l.cls[0].mean == 255 && l.cls[1].mean == 128 && l.cls[2].mean == 0, "means %f %f %f",
          l.cls[0].mean, l.cls[1].mean, l.cls[2].mean);
    CHECK(l.cls[0].sd == 0 && l.cls[1].sd == 0 && l.cls[2].sd == 0, "sd");
    CHECK(l.cls[0].min == 255 && l.cls[0].max == 255 && l.cls[1].min == 128 && l.cls[1].max == 128 &&
          l.cls[2].min == 0 && l.cls[2].max == 0, "min/max");
    CHECK(l.worst_dev3 == 0 && l.risky == 0 && l.min_margin == 63, "worst %d risky %d margin %d",
          l.worst_dev3, l.risky, l.min_margin);
    CHECK(l.hist[15] == l.cls[0].n && l.hist[8] == l.cls[1].n && l.hist[0] == l.cls[2].n, "buckets");

    TEST("greys: a shift of one class moves its numbers and the worst deviation");
    /* every X pixel 128 -> 133 on all channels, and one O cell's centre to 20 */
    for (int i = 0; i < 729 * 732; i++)
        if (paint[4 * i] == 128) paint[4 * i] = paint[4 * i + 1] = paint[4 * i + 2] = 133;
    int o_cell = -1;
    for (int i = 0; i < UTB_CELLS && o_cell < 0; i++) if (sym[i] == 2) o_cell = i;
    int row = o_cell / 243 + 1, col = o_cell % 243;
    long y = ((2L * row + 1) * 732) / (2L * 244), x = ((2L * col + 1) * 729) / (2L * 243);
    uint8_t *px = paint + (y * 729 + x) * 4;
    px[0] = 20; px[1] = 21; px[2] = 22;                     /* a sum of 63, a mean of 21 */
    CHECK(ubd_luma(paint, 729, 732, 243, &l) == 0, "luma");
    CHECK(l.cls[1].mean == 133 && l.cls[1].min == 133 && l.cls[1].max == 133, "X %f", l.cls[1].mean);
    CHECK(l.cls[2].max == 21 && l.cls[2].min == 0 && l.cls[2].sd > 0, "O max %d", l.cls[2].max);
    CHECK(l.worst_dev3 == 63, "worst %d (21 from 0)", l.worst_dev3);
    double n = l.cls[2].n, mean = 21.0 / n, var = (21.0 * 21.0) / n - mean * mean;
    CHECK(l.cls[2].sd > 0.999 * sqrt(var) && l.cls[2].sd < 1.001 * sqrt(var), "sd %f against %f", l.cls[2].sd, sqrt(var));
    CHECK(l.hist[133 / 16] == l.cls[1].n && l.hist[1] == 1, "buckets");
}

/* --------------------------------------------------------------- the ring */

static void test_ring(void)
{
    TEST("ring: the layout, byte for byte");
    UbdEvent e = { 1790001000, UBD_ROLE_OPENED, 729, 732, 459812, 890, 0, 46, UBD_R_OK,
                   UBD_EV_CRC_KNOWN | UBD_EV_CRC_SAME | UBD_EV_Q_EXACT };
    uint8_t a[UBD_RING_BYTES], b[UBD_RING_BYTES];
    int n = ubd_ring_push(NULL, 0, &e, a, sizeof a);
    static const uint8_t golden[24] = {
        'U', 'R', 1, 1,
        0x68, 0x3F, 0xB1, 0x6A,                 /* 1790001000 = 0x6AB13F68 */
        UBD_ROLE_OPENED, 89, 0xD9, 0x02, 0xDC, 0x02,
        0x24, 0x04, 0x07, 0x00, 0x00, 0x00, 46, UBD_R_OK, 7, 0 };
    CHECK(n == 24 && !memcmp(a, golden, 24), "n %d", n);

    TEST("ring: newest first, capped, every field back");
    int len = n;
    for (int i = 1; i < 25; i++) {
        UbdEvent k = e;
        k.when = e.when + i;
        k.role = i & 1 ? UBD_ROLE_SENT : UBD_ROLE_OPENED;
        k.risky = i & 1 ? -1 : i;
        k.min_margin = i & 1 ? -1 : 40 - i;
        k.q = i & 1 ? -1 : 500;
        k.result = i % UBD_R_COUNT;
        uint8_t *from = i & 1 ? a : b, *to = i & 1 ? b : a;
        len = ubd_ring_push(from, len, &k, to, UBD_RING_BYTES);
        CHECK(len == 4 + (i + 1 < UBD_RING_MAX ? i + 1 : UBD_RING_MAX) * UBD_REC_LEN, "len %d at %d", len, i);
    }
    const uint8_t *ring = a;                       /* the 24th push, even, wrote to a */
    CHECK(ubd_ring_count(ring, len) == UBD_RING_MAX, "count %d", ubd_ring_count(ring, len));
    for (int k = 0; k < UBD_RING_MAX; k++) {
        UbdEvent g;
        CHECK(ubd_ring_get(ring, len, k, &g), "get %d", k);
        int i = 24 - k;
        CHECK(g.when == e.when + i && g.role == (i & 1 ? UBD_ROLE_SENT : UBD_ROLE_OPENED) &&
              g.risky == (i & 1 ? -1 : i) && g.min_margin == (i & 1 ? -1 : 40 - i) &&
              g.q == (i & 1 ? -1 : 500) && g.result == i % UBD_R_COUNT && g.width == 729 && g.bytes == 459812,
              "record %d", k);
    }
    UbdEvent g;
    CHECK(!ubd_ring_get(ring, len, UBD_RING_MAX, &g) && !ubd_ring_get(ring, len, -1, &g), "past the end");

    TEST("ring: anything not the layout reads as empty and is started again");
    uint8_t junk[30] = { 'U', 'R', 2, 1 };
    CHECK(ubd_ring_count(junk, 24) == 0, "version");
    junk[2] = 1;
    CHECK(ubd_ring_count(junk, 23) == 0 && ubd_ring_count(junk, 25) == 0 && ubd_ring_count(junk, 24) == 1, "length");
    junk[3] = 21;
    CHECK(ubd_ring_count(junk, 4 + 21 * UBD_REC_LEN) == 0, "over the cap");
    CHECK(ubd_ring_push(junk, 24, &e, b, sizeof b) == 24 && ubd_ring_count(b, 24) == 1, "restart");
    CHECK(ubd_ring_push(a, len, &e, b, 23) == -1, "short cap");

    TEST("ring: values past a field clamp, never wrap");
    UbdEvent big = { 1790001000, UBD_ROLE_SENT, 70000, -5, 5000000000L, 2000, 70000, 300, 99, 0 };
    n = ubd_ring_push(NULL, 0, &big, a, sizeof a);
    CHECK(ubd_ring_get(a, n, 0, &g) && g.width == 0xFFFF && g.height == 0 && g.bytes == 0xFFFFFFFFL &&
          g.q == 1000 && g.risky == 0xFFFE && g.min_margin == 254 && g.result == UBD_R_NOT_READ,
          "w %d h %d bytes %ld q %d risky %d m %d r %d", g.width, g.height, g.bytes, g.q, g.risky, g.min_margin, g.result);
}

/* ------------------------------------------------------------- the report */

static int lines_ok(const char *s, int *count)
{
    int ok = 1, w = 0;
    *count = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '\n') { (*count)++; w = 0; continue; }
        if (++w > UBD_LINE_MAX) ok = 0;
        if ((unsigned char)*p >= 0x80 || *p == '{' || *p == '"') ok = 0;   /* plain ASCII, no JSON */
    }
    return ok;
}

/* The line holding `what`, or NULL. */
static const char *line_with(const char *s, const char *what, char *out, int cap)
{
    const char *p = strstr(s, what);
    if (!p) return NULL;
    while (p > s && p[-1] != '\n') p--;
    int k = 0;
    while (p[k] && p[k] != '\n' && k < cap - 1) { out[k] = p[k]; k++; }
    out[k] = 0;
    return out;
}

static char text[16384];

static void test_report(void)
{
    /* A received big bubble: a real link, its board, the picture as painted,
     * and a JPEG head that says 729 x 732 at ImageIO's 0.89. */
    UtbMsg m;
    uint8_t me[UTM_TAG_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 }, you[UTM_TAG_LEN] = { 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    utb_msg_open(&m, 1790001000, 7, me);
    CHECK(utb_msg_play(&m, you, 29527), "a first move");
    char url[160] = "data:,?";
    utb_msg_text_encode(&m, url + 6, (int)sizeof url - 6);
    memcpy(sym, m.game.cell, UTB_CELLS);
    bd_frame(sym, UTB_CELLS, BD_KIND_SYMBOLS, 0, 243, cells);
    bd_paint(cells, 243, 3, paint);

    long n89;
    uint8_t *j89 = slurp("imageio_q089_420.jpg", &n89);
    /* the SOF's height and width to 732 x 729: a head that says what a real
     * received picture is */
    for (long i = 2; i + 9 < n89; ) {
        if (j89[i + 1] == 0xC0) { j89[i + 5] = 732 >> 8; j89[i + 6] = 732 & 255; j89[i + 7] = 729 >> 8; j89[i + 8] = 729 & 255; break; }
        i += 2 + ((j89[i + 2] << 8) | j89[i + 3]);
    }
    uint8_t ring[UBD_RING_BYTES];
    UbdEvent sent = { 1790001100, UBD_ROLE_SENT, 729, 732, 255312, 500, -1, -1, UBD_R_NOT_READ, UBD_EV_Q_EXACT };
    int rn = ubd_ring_push(NULL, 0, &sent, ring, sizeof ring);

    UbdFacts f;
    memset(&f, 0, sizeof f);
    f.app_version = "1.1"; f.app_build = "16"; f.os_version = "27.0"; f.model = "iPhone16,2";
    f.install = "TestFlight"; f.now = 1790001200; f.utc_offset = 3600; f.from = UBD_FROM_SELECTED;
    f.who = UBD_WHO_OTHER; f.pending = 0; f.session = "8C2A";
    f.url = url; f.seat = "O by record";
    f.layout = "MSMessageTemplateLayout"; f.caption_len = 9; f.subcaption_len = -1; f.summary_len = 9;
    f.has_image = 1; f.image_w = 729; f.image_h = 732; f.image_scale_pct = 100;
    f.has_file = 1; f.file_ext = "jpeg"; f.file_bytes = n89; f.file = j89; f.file_n = n89;
    f.rgba = paint; f.rgba_w = 729; f.rgba_h = 732;
    f.read_result = UBD_R_OK; f.read_cells = 243 * 244; f.read_risky = 0; f.read_min_margin = 63;
    f.symbols = sym; f.read_us = 87400;
    f.ring = ring; f.ring_n = rn;

    TEST("report: every line is short, plain and ASCII");
    int len = ubd_report(&f, text, sizeof text), count = 0;
    CHECK(len > 0 && (int)strlen(text) == len, "len %d", len);
    CHECK(lines_ok(text, &count), "a line over %d, or not plain", UBD_LINE_MAX);
    CHECK(count > 40, "lines %d", count);
    if (getenv("UBD_SHOW")) fputs(text, stdout);              /* the report, to read */

    TEST("report: a received picture as expected has no mismatch mark");
    char l[128];
    CHECK(!strstr(text, "\n!"), "a '!' line in a report that matches:\n%s", text);
    CHECK(line_with(text, "size 729 x 732 (expect 729 x 732)", l, sizeof l) && l[0] == ' ', "size line");
    CHECK(line_with(text, "quality 0.89-0.91 (expect 0.89)", l, sizeof l) && l[0] == ' ', "quality line");
    CHECK(line_with(text, "an exact ImageIO table, luma sum 330", l, sizeof l), "method line");
    CHECK(line_with(text, "chroma 4:2:0, luma 2x2", l, sizeof l), "chroma line");
    CHECK(line_with(text, "header and check UTM_EOK", l, sizeof l), "check line");
    CHECK(line_with(text, "plies 1, last 29527", l, sizeof l), "plies line");
    CHECK(line_with(text, "= link", l, sizeof l) && l[0] == ' ', "board crc line");
    CHECK(line_with(text, "marks X 1, O 0, empty 59048", l, sizeof l), "marks line");
    CHECK(line_with(text, "result BD_EOK", l, sizeof l), "result line");
    CHECK(line_with(text, "read 87.4 ms", l, sizeof l), "read time");
    CHECK(line_with(text, "sent by the other person", l, sizeof l), "who");
    CHECK(line_with(text, "at 2026-09-21 ", l, sizeof l), "clock");

    TEST("report: the greys per class, the worst deviation and the histogram");
    /* every sampled cell, the header row's included */
    int lv[3] = { 0, 0, 0 };
    for (int i = 0; i < 243 * 244; i++) lv[cells[i]]++;
    char want[64];
    snprintf(want, sizeof want, "empty (painted 255): %d cells", lv[0]);
    CHECK(line_with(text, want, l, sizeof l), "%s", want);
    snprintf(want, sizeof want, "X (painted 128): %d cells", lv[1]);
    CHECK(line_with(text, want, l, sizeof l), "%s", want);
    snprintf(want, sizeof want, "O (painted 0): %d cells", lv[2]);
    CHECK(line_with(text, want, l, sizeof l), "%s", want);
    CHECK(line_with(text, "mean 128.0 sd 0.00 min 128 max 128", l, sizeof l), "X stats");
    CHECK(line_with(text, "worst deviation from nominal: 0.0", l, sizeof l), "worst");
    CHECK(line_with(text, "histogram, 16 levels a bucket", l, sizeof l), "histogram");

    TEST("report: the history, newest first, with the send");
    CHECK(line_with(text, "== history, newest first (1)", l, sizeof l), "history count");
    CHECK(line_with(text, "21 15:31:40 sent 729x732 255.3K q=0.50 -", l, sizeof l) &&
          !strcmp(l, "21 15:31:40 sent 729x732 255.3K q=0.50 -"), "the send: %s", text);

    TEST("report: what the transport was not expected to do is marked");
    f.who = UBD_WHO_OTHER;
    long n50;
    uint8_t *j50 = slurp("imageio_q050_420.jpg", &n50);
    f.file = j50; f.file_n = n50;                         /* a received copy at 0.50, 96 x 99 */
    f.read_risky = 3;
    CHECK(ubd_report(&f, text, sizeof text) > 0, "report");
    CHECK(line_with(text, "quality 0.50 (expect 0.89)", l, sizeof l) && l[0] == '!', "quality");
    CHECK(line_with(text, "size 96 x 99", l, sizeof l) && l[0] == '!', "size");
    CHECK(line_with(text, "risky 3", l, sizeof l) && l[0] == '!', "risky");
    f.who = UBD_WHO_ME;                                   /* my own copy may be 0.50 */
    CHECK(ubd_report(&f, text, sizeof text) > 0 &&
          line_with(text, "quality 0.50 (expect 0.50 or 0.89)", l, sizeof l) && l[0] == ' ', "mine at 0.50");

    TEST("report: a board that is not the link's is marked");
    sym[100] = 2;
    CHECK(ubd_report(&f, text, sizeof text) > 0 && line_with(text, "!= link", l, sizeof l) && l[0] == '!',
          "crc");
    sym[100] = 0;

    TEST("report: a cap too small cuts on a whole line");
    char small[300];
    CHECK(ubd_report(&f, small, sizeof small) == -1, "cut");
    CHECK(strlen(small) > 0 && small[strlen(small) - 1] == '\n', "ends on a line");

    TEST("report: no message is a short report and the history");
    UbdFacts none;
    memset(&none, 0, sizeof none);
    none.from = UBD_FROM_NONE;
    none.ring = ring; none.ring_n = rn;
    CHECK(ubd_report(&none, text, sizeof text) > 0 && strstr(text, "no message (+ menu)") &&
          !strstr(text, "== the link") && strstr(text, "history, newest first (1)"), "none");

    TEST("report: the event a reading adds");
    f.who = UBD_WHO_OTHER; f.file = j89; f.file_n = n89; f.read_risky = 0;
    UbdEvent ev = ubd_event_of(&f, UBD_ROLE_OPENED);
    CHECK(ev.width == 729 && ev.height == 732 && ev.q == 890 && (ev.flags & UBD_EV_Q_EXACT) &&
          (ev.flags & UBD_EV_CRC_KNOWN) && (ev.flags & UBD_EV_CRC_SAME) && ev.risky == 0 &&
          ev.min_margin == 63 && ev.bytes == n89 && ev.result == UBD_R_OK, "event");
    sym[5] = 1;
    ev = ubd_event_of(&f, UBD_ROLE_OPENED);
    CHECK((ev.flags & UBD_EV_CRC_KNOWN) && !(ev.flags & UBD_EV_CRC_SAME), "a changed board");
    sym[5] = 0;
    free(j89);
    free(j50);
}

int main(void)
{
    test_fixtures();
    test_estimate();
    test_hostile();
    test_greys_match_the_kit();
    test_greys_numbers();
    test_ring();
    test_report();
    return report("uttt_big_diag_test");
}
