/* bubble_data_test.c - the board-as-a-picture format (bubble_data.h): the
 * CRC-32 check value, framing and unframing both layers at the sizes that
 * matter (the smallest side, the 243 board, the byte capacity edge), every
 * error a read can return reached on purpose, painting and sampling back
 * exactly, sampling a picture that came back at another size, and the
 * decision margin (+/-63 of noise reads right, +/-70 does not).
 *
 *   make -C shared/swift/BubbleDataKit test
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include "../../../../c/test/check.h"
#include "../../Sources/CBubbleData/include/bubble_data.h"

#define NMAX 243
#define CELLS_MAX (NMAX * (NMAX + 1))
#define PIX_MAX (3 * NMAX * 3 * (NMAX + 1) * 4)

static uint8_t g_sym[CELLS_MAX], g_back[CELLS_MAX], g_cells[CELLS_MAX], g_cells2[CELLS_MAX];
static uint8_t g_bytes[NMAX * NMAX], g_bytes_back[NMAX * NMAX];
static uint8_t g_rgba[PIX_MAX], g_rgba2[PIX_MAX];

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

static void random_symbols(uint8_t *s, int k) { for (int i = 0; i < k; i++) s[i] = (uint8_t)(next() % 3); }
static void random_bytes(uint8_t *b, int k) { for (int i = 0; i < k; i++) b[i] = (uint8_t)next(); }

/* Frame nsym random symbols into an n grid, unframe them, compare. */
static void symbols_round_trip(int n, int nsym)
{
    random_symbols(g_sym, nsym);
    int r = bd_frame(g_sym, nsym, BD_KIND_SYMBOLS, 0, n, g_cells);
    CHECK(r == BD_EOK, "frame n=%d nsym=%d: %d", n, nsym, r);
    int kind = -1, pad = -1;
    int got = bd_unframe(g_cells, n, g_back, CELLS_MAX, &kind, &pad);
    CHECK(got == nsym, "unframe n=%d: %d symbols, want %d", n, got, nsym);
    CHECK(kind == BD_KIND_SYMBOLS && pad == 0, "n=%d kind %d pad %d", n, kind, pad);
    CHECK(got < 0 || !memcmp(g_sym, g_back, (size_t)nsym), "n=%d symbols differ", n);
    int zeros = 1;
    for (int i = n + nsym; i < n * (n + 1); i++) zeros &= g_cells[i] == 0;
    for (int i = BD_HEADER_CELLS; i < n; i++) zeros &= g_cells[i] == 0;
    CHECK(zeros, "n=%d: the cells outside the header and payload are not zero", n);
}

static void bytes_round_trip(int n, int nbytes)
{
    random_bytes(g_bytes, nbytes);
    int r = bd_encode_bytes(g_bytes, nbytes, n, g_cells);
    CHECK(r == BD_EOK, "encode n=%d nbytes=%d: %d", n, nbytes, r);
    memset(g_bytes_back, 0xA5, sizeof g_bytes_back);
    int got = bd_decode_bytes(g_cells, n, g_bytes_back, (int)sizeof g_bytes_back);
    CHECK(got == nbytes, "decode n=%d: %d bytes, want %d", n, got, nbytes);
    CHECK(got < 0 || !memcmp(g_bytes, g_bytes_back, (size_t)nbytes), "n=%d nbytes=%d bytes differ", n, nbytes);
    CHECK(nbytes >= (int)sizeof g_bytes_back || g_bytes_back[nbytes] == 0xA5,
          "n=%d nbytes=%d: decode wrote past the payload", n, nbytes);
}

/* Header byte i of a framed grid (the 12 header bytes are 4 groups of 16 cells). */
static void header_bytes(const uint8_t *cells, uint8_t *h)
{
    for (int g = 0; g < 4; g++) {
        uint32_t v = 0;
        for (int i = 15; i >= 0; i--) v = v * 3 + cells[g * 16 + i];
        h[g * 3] = (uint8_t)v; h[g * 3 + 1] = (uint8_t)(v >> 8); h[g * 3 + 2] = (uint8_t)(v >> 16);
    }
}

/* Rewrite the header with one byte changed and the CRC made right again, so a
 * test reaches the check AFTER the magic, not the checksum. */
static void forge_header(uint8_t *cells, int n, int at, uint8_t value)
{
    uint8_t h[12];
    header_bytes(cells, h);
    h[at] = value;
    int count = h[5] | (h[6] << 8) | (h[7] << 16);
    if (count > n * n) count = n * n;
    uint32_t crc = bd_crc32(bd_crc32(0, h, 8), cells + n, count);
    h[8] = (uint8_t)crc; h[9] = (uint8_t)(crc >> 8); h[10] = (uint8_t)(crc >> 16); h[11] = (uint8_t)(crc >> 24);
    for (int g = 0; g < 4; g++) {
        uint32_t v = h[g * 3] | (h[g * 3 + 1] << 8) | ((uint32_t)h[g * 3 + 2] << 16);
        for (int i = 0; i < 16; i++) { cells[g * 16 + i] = (uint8_t)(v % 3); v /= 3; }
    }
}

static void test_crc(void)
{
    TEST("crc32: the check value");
    const uint8_t *s = (const uint8_t *)"123456789";
    uint32_t c = bd_crc32(0, s, 9);
    CHECK(c == 0xCBF43926u, "crc32(\"123456789\") = %08x", c);
    CHECK(bd_crc32(bd_crc32(0, s, 4), s + 4, 5) == c, "chained crc differs");
    CHECK(bd_crc32(0, s, 0) == 0, "empty crc is not 0");
}

static void test_geometry(void)
{
    TEST("geometry");
    CHECK(bd_geometry_ok(243, 1), "243 x 1");
    CHECK(bd_geometry_ok(243, 3), "243 x 3");
    CHECK(bd_geometry_ok(64, 1), "64 x 1");
    CHECK(!bd_geometry_ok(63, 1), "63 is under the header row");
    CHECK(!bd_geometry_ok(243, 0), "0 px");
    CHECK(!bd_geometry_ok(243, 5), "243 x 5 is 1220 px tall");
    CHECK(bd_geometry_ok(399, 3), "399 x 3 is 1200 px tall");
    CHECK(!bd_geometry_ok(400, 3), "400 x 3 is 1203 px tall");
    CHECK(bd_geometry_ok(1199, 1), "1199 x 1");
    CHECK(!bd_geometry_ok(1200, 1), "1200 x 1 is 1201 px tall");
    CHECK(!bd_geometry_ok(243, 1 << 30), "a huge p must not overflow into a yes");
    CHECK(bd_symbol_capacity(243) == 59049, "symbol capacity 243");
    CHECK(bd_byte_capacity(243) == 11070, "byte capacity 243 = %d", bd_byte_capacity(243));
    CHECK(bd_byte_capacity(64) == 768, "byte capacity 64 = %d", bd_byte_capacity(64));
    CHECK(bd_symbol_capacity(63) == BD_EGEOMETRY && bd_byte_capacity(-5) == BD_EGEOMETRY, "capacity of a bad side");
}

static void test_symbols(void)
{
    TEST("symbols: frame and unframe");
    symbols_round_trip(64, 64 * 64);
    symbols_round_trip(64, 0);
    symbols_round_trip(64, 1);
    symbols_round_trip(100, 777);
    symbols_round_trip(243, 243 * 243);
    symbols_round_trip(243, 12345);

    /* The CRC covers the payload: the same symbols framed twice are the same
     * cells, and one different symbol makes a different header. */
    random_symbols(g_sym, 4096);
    bd_frame(g_sym, 4096, BD_KIND_SYMBOLS, 0, 64, g_cells);
    g_sym[4095] = (uint8_t)((g_sym[4095] + 1) % 3);
    bd_frame(g_sym, 4096, BD_KIND_SYMBOLS, 0, 64, g_cells2);
    CHECK(memcmp(g_cells, g_cells2, 64) != 0, "the header does not depend on the last symbol");

    /* The header's first bytes, read back raw: 'B' 'D' version kind pad count. */
    uint8_t h[12];
    random_symbols(g_sym, 500);
    bd_frame(g_sym, 500, BD_KIND_SYMBOLS, 0, 100, g_cells);
    header_bytes(g_cells, h);
    CHECK(h[0] == 'B' && h[1] == 'D', "magic %02x %02x", h[0], h[1]);
    CHECK(h[2] == BD_VERSION && h[3] == BD_KIND_SYMBOLS && h[4] == 0, "version %d kind %d pad %d", h[2], h[3], h[4]);
    CHECK((h[5] | (h[6] << 8) | (h[7] << 16)) == 500, "count %d", h[5] | (h[6] << 8) | (h[7] << 16));
}

static void test_bytes(void)
{
    TEST("bytes: encode and decode");
    for (int k = 0; k <= 7; k++) bytes_round_trip(64, k);
    bytes_round_trip(64, 768);            /* the byte capacity edge, n = 64 */
    bytes_round_trip(243, 1000);
    bytes_round_trip(243, 11070);         /* the byte capacity edge, n = 243 */
    bytes_round_trip(243, 11069);
    bytes_round_trip(243, 11068);
    CHECK(bd_encode_bytes(g_bytes, 769, 64, g_cells) == BD_ECAP, "769 bytes fit a 64 grid");
    CHECK(bd_encode_bytes(g_bytes, 11071, 243, g_cells) == BD_ECAP, "11071 bytes fit a 243 grid");

    /* The digits: 3 bytes, little-endian, into 16 base-3 digits, least
     * significant first. 0x000001 is a 1 then fifteen 0s; 3 is 0 1 0 ...;
     * 0xFFFFFF is 2^24 - 1 = 3^15 + ... and must come back. */
    uint8_t in[3] = { 1, 0, 0 }, sym[16];
    int pad = -1;
    CHECK(bd_pack_bytes(in, 3, sym, 16, &pad) == 16 && pad == 0, "pack 3 bytes");
    CHECK(sym[0] == 1 && sym[1] == 0 && sym[15] == 0, "1 packs as %d %d .. %d", sym[0], sym[1], sym[15]);
    in[0] = 3;
    bd_pack_bytes(in, 3, sym, 16, &pad);
    CHECK(sym[0] == 0 && sym[1] == 1, "3 packs as %d %d", sym[0], sym[1]);
    in[0] = 0; in[1] = 1;                 /* 256 = 100111 in base 3 = 1 + 0*3 + ... */
    bd_pack_bytes(in, 3, sym, 16, &pad);
    CHECK(sym[0] == 1 && sym[1] == 1 && sym[2] == 1 && sym[3] == 0 && sym[4] == 0 && sym[5] == 1,
          "256 packs as %d%d%d%d%d%d", sym[0], sym[1], sym[2], sym[3], sym[4], sym[5]);
    in[0] = in[1] = in[2] = 0xFF;
    uint8_t out[3];
    bd_pack_bytes(in, 3, sym, 16, &pad);
    CHECK(bd_unpack_bytes(sym, 16, 0, out, 3) == 3 && out[0] == 0xFF && out[1] == 0xFF && out[2] == 0xFF,
          "0xFFFFFF did not come back");
    in[0] = 7;
    CHECK(bd_pack_bytes(in, 1, sym, 16, &pad) == 16 && pad == 2, "one byte has pad %d", pad);
    CHECK(bd_unpack_bytes(sym, 16, 2, out, 1) == 1 && out[0] == 7, "one byte back");
    CHECK(bd_pack_bytes(in, 2, sym, 16, &pad) == 16 && pad == 1, "two bytes have pad %d", pad);
    CHECK(bd_pack_bytes(in, 4, sym, 16, &pad) == BD_ECAP, "4 bytes into 16 cells");
    CHECK(bd_unpack_bytes(sym, 16, 0, out, 2) == BD_ECAP, "unpack into a short buffer");

    /* 2^24 itself, as digits, is not a group: 16777216 in base 3. */
    uint32_t v = 1u << 24;
    for (int i = 0; i < 16; i++) { sym[i] = (uint8_t)(v % 3); v /= 3; }
    CHECK(bd_unpack_bytes(sym, 16, 0, out, 3) == BD_ESYMBOL, "2^24 unpacked");
    for (int i = 0; i < 16; i++) sym[i] = 2;  /* 3^16 - 1, the largest digits make */
    CHECK(bd_unpack_bytes(sym, 16, 0, out, 3) == BD_ESYMBOL, "3^16 - 1 unpacked");
    CHECK(bd_unpack_bytes(sym, 15, 0, out, 3) == BD_ESYMBOL, "15 cells unpacked");
    CHECK(bd_unpack_bytes(sym, 16, 3, out, 3) == BD_ELENGTH, "pad 3 unpacked");
    /* A pad byte that is not zero was never written by us. */
    in[0] = 9; in[1] = 9; in[2] = 9;
    bd_pack_bytes(in, 3, sym, 16, &pad);
    CHECK(bd_unpack_bytes(sym, 16, 1, out, 3) == BD_ESYMBOL, "a non-zero pad byte unpacked");
}

static void test_errors(void)
{
    TEST("errors: every code reached");
    const int n = 100;
    int nb = 300;
    random_bytes(g_bytes, nb);
    random_symbols(g_sym, 1000);

    /* Geometry. */
    CHECK(bd_frame(g_sym, 10, BD_KIND_SYMBOLS, 0, 63, g_cells) == BD_EGEOMETRY, "frame n=63");
    CHECK(bd_unframe(g_cells, 1200, g_back, CELLS_MAX, NULL, NULL) == BD_EGEOMETRY, "unframe n=1200");
    CHECK(bd_paint(g_cells, 243, 5, g_rgba) == BD_EGEOMETRY, "paint 243 x 5");
    CHECK(bd_encode_bytes(g_bytes, 3, 0, g_cells) == BD_EGEOMETRY, "encode n=0");
    CHECK(bd_decode_bytes(g_cells, -1, g_bytes_back, 10) == BD_EGEOMETRY, "decode n=-1");
    CHECK(bd_sample(g_rgba, 100, 100, 100, g_cells, NULL) == BD_EGEOMETRY, "sample 100 px tall for 101 rows");
    CHECK(bd_sample(g_rgba, 99, 101, 100, g_cells, NULL) == BD_EGEOMETRY, "sample 99 px wide for 100 cells");
    CHECK(bd_sample(g_rgba, 100, BD_MAX_READ_SIDE + 1, 100, g_cells, NULL) == BD_EGEOMETRY, "sample too tall");

    /* Capacity. */
    CHECK(bd_frame(g_sym, n * n + 1, BD_KIND_SYMBOLS, 0, n, g_cells) == BD_ECAP, "frame over n*n");
    CHECK(bd_frame(g_sym, -1, BD_KIND_SYMBOLS, 0, n, g_cells) == BD_ECAP, "frame a negative count");
    CHECK(bd_frame(g_sym, 10, BD_KIND_SYMBOLS, 0, n, NULL) == BD_ECAP, "frame into NULL");
    bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
    CHECK(bd_unframe(g_cells, n, g_back, 999, NULL, NULL) == BD_ECAP, "unframe 1000 into 999");
    CHECK(bd_unframe(g_cells, n, g_back, 1000, NULL, NULL) == 1000, "unframe 1000 into 1000");
    bd_encode_bytes(g_bytes, nb, n, g_cells);
    CHECK(bd_decode_bytes(g_cells, n, g_bytes_back, nb - 1) == BD_ECAP, "decode 300 into 299");

    /* Symbols over 2, in what a caller frames and in what a picture holds. */
    g_sym[5] = 3;
    CHECK(bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells) == BD_ESYMBOL, "frame a 3");
    g_sym[5] = 0;
    bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
    g_cells[n + 7] = 3;
    CHECK(bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) == BD_ESYMBOL, "unframe a payload 3");
    CHECK(bd_paint(g_cells, n, 1, g_rgba) == BD_ESYMBOL, "paint a 3");
    bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
    g_cells[70] = 200;                     /* the header row, past the header */
    CHECK(bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) == BD_ESYMBOL, "unframe a header-row 200");

    /* Magic: a byte of 'B' 'D' changed, CRC made right again. */
    bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
    forge_header(g_cells, n, 0, 'b');
    CHECK(bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) == BD_EMAGIC, "magic 'bD'");
    bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
    forge_header(g_cells, n, 1, 'E');
    CHECK(bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) == BD_EMAGIC, "magic 'BE'");
    /* A blank picture (all white, all zero cells) is not ours. */
    memset(g_cells, 0, sizeof g_cells);
    CHECK(bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) == BD_EMAGIC, "a blank grid");
    /* A first header group that is not 24 bits: not ours either. */
    memset(g_cells, 2, sizeof g_cells);
    CHECK(bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) == BD_EMAGIC, "an all-black grid");

    /* Version. */
    bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
    forge_header(g_cells, n, 2, BD_VERSION + 1);
    CHECK(bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) == BD_EVERSION, "version 2");

    /* Kind: an unknown kind, and the wrong layer asked for. */
    bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
    forge_header(g_cells, n, 3, 7);
    CHECK(bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) == BD_EKIND, "kind 7");
    bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
    CHECK(bd_decode_bytes(g_cells, n, g_bytes_back, 10000) == BD_EKIND, "bytes read from a symbol board");
    CHECK(bd_frame(g_sym, 10, 2, 0, n, g_cells) == BD_EKIND, "frame kind 2");
    int kind = -1, pad = -1;
    bd_encode_bytes(g_bytes, nb - 1, n, g_cells);
    int got = bd_unframe(g_cells, n, g_back, CELLS_MAX, &kind, &pad);
    CHECK(got == 100 * 16 && kind == BD_KIND_BYTES && pad == 1, "a byte board unframed: %d kind %d pad %d", got, kind, pad);

    /* Length: a count past the grid, a bad pad. */
    bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
    forge_header(g_cells, n, 7, 0x10);    /* count + 0x100000, over 10,000 */
    CHECK(bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) == BD_ELENGTH, "count over n*n");
    bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
    forge_header(g_cells, n, 4, 1);
    CHECK(bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) == BD_ELENGTH, "a symbol board with pad 1");
    bd_encode_bytes(g_bytes, nb, n, g_cells);
    forge_header(g_cells, n, 4, 3);
    CHECK(bd_decode_bytes(g_cells, n, g_bytes_back, 10000) == BD_ELENGTH, "a byte board with pad 3");
    bd_encode_bytes(g_bytes, nb, n, g_cells);
    forge_header(g_cells, n, 5, (uint8_t)(1600 - 1));  /* 1599 cells: not whole groups */
    forge_header(g_cells, n, 6, (uint8_t)((1600 - 1) >> 8));
    CHECK(bd_decode_bytes(g_cells, n, g_bytes_back, 10000) == BD_ELENGTH, "a byte board of 1599 cells");
    CHECK(bd_frame(g_sym, 10, BD_KIND_SYMBOLS, 1, n, g_cells) == BD_ELENGTH, "frame symbols with pad 1");

    /* Checksum: one cell flipped anywhere a read looks. */
    int flips[] = { n, n + 1, n + 500, n + 999, 0, 15, 40, 63 };
    for (unsigned i = 0; i < sizeof flips / sizeof flips[0]; i++) {
        bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
        int at = flips[i];
        g_cells[at] = (uint8_t)((g_cells[at] + 1) % 3);
        int r = bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL);
        CHECK(r < 0, "a flip at cell %d read as %d symbols", at, r);
        if (at >= n) CHECK(r == BD_ECHECK, "a payload flip at %d gave %d", at, r);
    }
    /* A flipped CRC cell (header group 3, cells 48..63), which may also give a
     * group over 2^24: either way a checksum failure. */
    bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
    g_cells[50] = (uint8_t)((g_cells[50] + 1) % 3);
    CHECK(bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) == BD_ECHECK, "a CRC cell flipped");
    /* And the bytes, the same way. */
    bd_encode_bytes(g_bytes, nb, n, g_cells);
    g_cells[n + 33] = (uint8_t)((g_cells[n + 33] + 2) % 3);
    CHECK(bd_decode_bytes(g_cells, n, g_bytes_back, 10000) == BD_ECHECK, "a byte board flip");

    /* A refused read leaves the output alone. */
    memset(g_back, 0x77, 16);
    CHECK(bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) < 0 && g_back[0] == 0x77 && g_back[15] == 0x77,
          "a refused unframe wrote");
}

static void test_paint_sample(void)
{
    TEST("pixels: paint then sample");
    static const int ns[] = { 64, 243 }, ps[] = { 1, 3 };
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) {
        int n = ns[a], p = ps[b];
        random_symbols(g_sym, n * n);
        bd_frame(g_sym, n * n, BD_KIND_SYMBOLS, 0, n, g_cells);
        CHECK(bd_paint(g_cells, n, p, g_rgba) == BD_EOK, "paint n=%d p=%d", n, p);
        int w = bd_width(n, p), h = bd_height(n, p);
        int greys_ok = 1;
        for (int i = 0; i < w * h; i++) {
            uint8_t v = g_rgba[4 * i];
            greys_ok &= (v == 255 || v == 128 || v == 0) && g_rgba[4 * i + 1] == v && g_rgba[4 * i + 2] == v &&
                        g_rgba[4 * i + 3] == 255;
        }
        CHECK(greys_ok, "n=%d p=%d painted something other than the three greys", n, p);
        /* Cell (x, y) of the board is the block at (x * p, (y + 1) * p). */
        int x = 5, y = 7;
        uint8_t want = g_sym[y * n + x] == 0 ? 255 : g_sym[y * n + x] == 1 ? 128 : 0;
        CHECK(g_rgba[(((y + 1) * p + p - 1) * w + x * p + p - 1) * 4] == want, "n=%d p=%d cell (5,7) is the wrong grey", n, p);

        BdReading rd = { -1, -1, -1 };
        memset(g_cells2, 9, sizeof g_cells2);
        CHECK(bd_sample(g_rgba, w, h, n, g_cells2, &rd) == BD_EOK, "sample n=%d p=%d", n, p);
        CHECK(!memcmp(g_cells, g_cells2, (size_t)(n * (n + 1))), "n=%d p=%d sampled cells differ", n, p);
        CHECK(rd.cells == n * (n + 1) && rd.risky == 0 && rd.min_margin == 63,
              "n=%d p=%d reading %d cells, %d risky, margin %d", n, p, rd.cells, rd.risky, rd.min_margin);
        int got = bd_unframe(g_cells2, n, g_back, CELLS_MAX, NULL, NULL);
        CHECK(got == n * n && !memcmp(g_back, g_sym, (size_t)(n * n)), "n=%d p=%d unframe after sample", n, p);
    }

    TEST("pixels: sampled at another size");
    /* A 243 board painted at 3 px, then resampled by nearest neighbour to
     * sizes it did not paint: larger, 2.x px a cell and odd sizes. Each must
     * read exactly, because each cell's centre is still inside its block. */
    const int n = 243, p = 3;
    random_symbols(g_sym, n * n);
    bd_frame(g_sym, n * n, BD_KIND_SYMBOLS, 0, n, g_cells);
    bd_paint(g_cells, n, p, g_rgba);
    int W = bd_width(n, p), H = bd_height(n, p);
    static const int sizes[][2] = { { 600, 601 }, { 500, 640 }, { 243, 244 }, { 701, 503 } };
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        int w = sizes[s][0], h = sizes[s][1];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int sx = (int)(((2L * x + 1) * W) / (2L * w)), sy = (int)(((2L * y + 1) * H) / (2L * h));
                memcpy(g_rgba2 + ((long)y * w + x) * 4, g_rgba + ((long)sy * W + sx) * 4, 4);
            }
        BdReading rd;
        CHECK(bd_sample(g_rgba2, w, h, n, g_cells2, &rd) == BD_EOK, "sample at %dx%d", w, h);
        int got = bd_unframe(g_cells2, n, g_back, CELLS_MAX, NULL, NULL);
        CHECK(got == n * n && !memcmp(g_back, g_sym, (size_t)(n * n)), "the board read at %dx%d gave %d", w, h, got);
    }
}

/* Paint a 64 board at 1 px and push every pixel `d` away from its level,
 * toward the nearest threshold (white down, black up, grey alternately up and
 * down), then read. */
static int read_with_noise(int d, BdReading *rd)
{
    const int n = 64;
    random_symbols(g_sym, n * n);
    bd_frame(g_sym, n * n, BD_KIND_SYMBOLS, 0, n, g_cells);
    bd_paint(g_cells, n, 1, g_rgba);
    int px = bd_width(n, 1) * bd_height(n, 1);
    for (int i = 0; i < px; i++) {
        int v = g_rgba[4 * i];
        int nv = v == 255 ? v - d : v == 0 ? v + d : (i & 1) ? v + d : v - d;
        if (nv < 0) nv = 0;
        if (nv > 255) nv = 255;
        for (int c = 0; c < 3; c++) g_rgba[4 * i + c] = (uint8_t)nv;
    }
    bd_sample(g_rgba, bd_width(n, 1), bd_height(n, 1), n, g_cells2, rd);
    int wrong = 0;
    for (int i = 0; i < n * (n + 1); i++) wrong += g_cells2[i] != g_cells[i];
    return wrong;
}

static void test_margin(void)
{
    TEST("margin: +/-63 reads right, +/-70 does not");
    BdReading rd;
    int wrong = read_with_noise(63, &rd);
    CHECK(wrong == 0, "+/-63 misread %d cells", wrong);
    CHECK(rd.min_margin == 0, "+/-63 leaves margin %d", rd.min_margin);
    CHECK(rd.risky == rd.cells, "+/-63: %d of %d cells risky", rd.risky, rd.cells);
    wrong = read_with_noise(70, &rd);
    CHECK(wrong > 0, "+/-70 misread no cell");
    /* At 47, only a grey pushed UP sits 16 from a threshold (128 + 47 = 175,
     * 191 - 175 = 16): risky, and the others not. */
    wrong = read_with_noise(47, &rd);
    CHECK(wrong == 0 && rd.min_margin == 16 && rd.risky > 0 && rd.risky < rd.cells,
          "+/-47: %d wrong, margin %d, %d risky", wrong, rd.min_margin, rd.risky);
    wrong = read_with_noise(46, &rd);
    CHECK(wrong == 0 && rd.min_margin == 17 && rd.risky == 0, "+/-46: %d wrong, margin %d, %d risky",
          wrong, rd.min_margin, rd.risky);
    wrong = read_with_noise(10, &rd);
    CHECK(wrong == 0 && rd.min_margin == 53 && rd.risky == 0, "+/-10: %d wrong, margin %d, %d risky",
          wrong, rd.min_margin, rd.risky);

    /* The thresholds themselves: a mean of 191 is grey, 192 white, 64 grey,
     * 63 black; and the mean is of r, g and b, not of one channel. */
    const int n = 64;
    memset(g_rgba, 0, sizeof g_rgba);
    int w = bd_width(n, 1), h = bd_height(n, 1);
    static const uint8_t px[][3] = { { 191, 191, 191 }, { 192, 192, 192 }, { 64, 64, 64 }, { 63, 63, 63 },
                                     { 255, 255, 63 }, { 0, 0, 255 } };
    static const uint8_t want[] = { 1, 0, 1, 2, 1, 1 };
    for (int i = 0; i < 6; i++) memcpy(g_rgba + 4 * i, px[i], 3);
    bd_sample(g_rgba, w, h, n, g_cells2, &rd);
    for (int i = 0; i < 6; i++)
        CHECK(g_cells2[i] == want[i], "(%d,%d,%d) read as %d, want %d", px[i][0], px[i][1], px[i][2], g_cells2[i], want[i]);
}

/* Random grids, and grids that are ours with random header cells: every read
 * refused, and (under -fsanitize=address, which `make ctest` also
 * runs) none reads or writes out of bounds. */
static void test_hostile(void)
{
    TEST("hostile: random grids refused");
    const int n = 64;
    int accepted = 0;
    for (int it = 0; it < 3000; it++) {
        if (it % 3 == 0) {
            for (int i = 0; i < n * (n + 1); i++) g_cells[i] = (uint8_t)next();
        } else if (it % 3 == 1) {
            random_symbols(g_cells, n * (n + 1));
        } else {
            random_symbols(g_sym, 1000);
            bd_frame(g_sym, 1000, BD_KIND_SYMBOLS, 0, n, g_cells);
            for (int k = 0; k < 3; k++) g_cells[next() % 64] = (uint8_t)(next() % 3);
            /* Unless the three draws happened to leave it as it was. */
            if (bd_unframe(g_cells, n, g_back, CELLS_MAX, NULL, NULL) == 1000 && !memcmp(g_back, g_sym, 1000))
                continue;
        }
        accepted += bd_unframe(g_cells, n, g_back, 10, NULL, NULL) >= 0;
        accepted += bd_decode_bytes(g_cells, n, g_bytes_back, 10) >= 0;
    }
    CHECK(accepted == 0, "%d hostile grids were read", accepted);
}

int main(void)
{
    test_hostile();
    test_crc();
    test_geometry();
    test_symbols();
    test_bytes();
    test_errors();
    test_paint_sample();
    test_margin();
    return report("bubble_data");
}
