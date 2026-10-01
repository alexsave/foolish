/* bubble_data.c - see bubble_data.h for the format; this file is its only
 * implementation.
 *
 * Every entry point checks its lengths and indices before it touches a
 * buffer, so a hostile or damaged picture can only produce a negative BD_E*.
 * Nothing allocates. The order of the checks in a read is part of the
 * contract: geometry, then the header's cells, magic, version, kind, pad and
 * count, then the checksum, and only then the caller's capacity, so BD_ECAP
 * means "a good picture, a small buffer" and never hides a damaged one. */
#include "include/bubble_data.h"

#define BD_GROUP_CELLS   16
#define BD_GROUP_BYTES   3
#define BD_HEADER_BYTES  12
#define BD_HEADER_CRC_AT 8          /* bytes 0..7 are the checksummed head */
#define BD_GROUP_LIMIT   (1u << 24) /* a group's value must stay under 2^24 */

/* Decision thresholds on the mean of r, g and b, kept as sums of the three
 * channels so the comparison is exact: a mean over 191 is white (0), under 64
 * is black (2), and everything from 64 to 191 is grey (1). The levels are
 * 255, 128 and 0, so each sits 63 or 64 away from its nearest threshold. */
#define BD_SUM_LO  (3 * 64)
#define BD_SUM_HI  (3 * 191)

/* The largest n for which an n x (n + 1) grid fits a send at one pixel a cell. */
#define BD_MAX_SIDE (BD_MAX_PIXELS - 1)

/* ---------------------------------------------------------------- CRC-32 */

/* CRC-32, IEEE 802.3, reflected, polynomial 0xEDB88320. The convention is
 * zlib's: the caller starts from 0 and chains by passing the last result
 * back in; the pre- and post-inversion live in here. So bd_crc32(0, "123456789", 9)
 * is the standard check value 0xCBF43926, and
 * bd_crc32(bd_crc32(0, a, na), b, nb) == bd_crc32(0, a || b, na + nb).
 * A negative or zero n, or a NULL p, leaves the running value unchanged. */
uint32_t bd_crc32(uint32_t crc, const uint8_t *p, int n)
{
    if (!p || n <= 0) return crc;
    crc = ~crc;
    for (int i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* -------------------------------------------------------------- geometry */

static int side_ok(int n) { return n >= BD_MIN_SIDE && n <= BD_MAX_SIDE; }

/* 1 when an n-cell, p-pixel picture is one a send keeps whole, else 0. */
int bd_geometry_ok(int n, int p)
{
    if (!side_ok(n) || p < 1) return 0;
    /* (n + 1) * p is the taller side; n <= 1199 keeps the product in range
     * only once p is bounded, so bound p first. */
    if (p > BD_MAX_PIXELS) return 0;
    return bd_height(n, p) <= BD_MAX_PIXELS;
}

int bd_symbol_capacity(int n)
{
    if (!side_ok(n)) return BD_EGEOMETRY;
    return n * n;
}

int bd_byte_capacity(int n)
{
    if (!side_ok(n)) return BD_EGEOMETRY;
    return (n * n / BD_GROUP_CELLS) * BD_GROUP_BYTES;
}

/* ------------------------------------------------------------ the groups */

/* 3 bytes (little-endian, b[0] the low byte) into 16 digits, the first digit
 * the least significant. */
static void put_group(const uint8_t *b, uint8_t *d)
{
    uint32_t v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
    for (int i = 0; i < BD_GROUP_CELLS; i++) {
        d[i] = (uint8_t)(v % 3);
        v /= 3;
    }
}

/* 16 digits back into 3 bytes; BD_ESYMBOL for a digit over 2 or a value of
 * 2^24 or more. Writes b only on success. */
static int get_group(const uint8_t *d, uint8_t *b)
{
    uint32_t v = 0;
    for (int i = BD_GROUP_CELLS - 1; i >= 0; i--) {
        if (d[i] > 2) return BD_ESYMBOL;
        v = v * 3 + d[i];               /* 3^16 - 1 < 2^32: no overflow */
    }
    if (v >= BD_GROUP_LIMIT) return BD_ESYMBOL;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    return BD_EOK;
}

/* --------------------------------------------------------- the byte layer */

int bd_pack_bytes(const uint8_t *in, int nbytes, uint8_t *sym, int cap, int *pad)
{
    if (nbytes < 0 || cap < 0) return BD_ECAP;
    if (nbytes > 0 && !in) return BD_ECAP;
    int groups = nbytes / BD_GROUP_BYTES + (nbytes % BD_GROUP_BYTES != 0);
    if (groups > cap / BD_GROUP_CELLS) return BD_ECAP;
    int nsym = groups * BD_GROUP_CELLS;
    if (nsym > 0 && !sym) return BD_ECAP;
    for (int g = 0; g < groups; g++) {
        uint8_t b[BD_GROUP_BYTES] = { 0, 0, 0 };
        for (int k = 0; k < BD_GROUP_BYTES; k++) {
            int at = g * BD_GROUP_BYTES + k;
            if (at < nbytes) b[k] = in[at];
        }
        put_group(b, sym + g * BD_GROUP_CELLS);
    }
    if (pad) *pad = groups * BD_GROUP_BYTES - nbytes;
    return nsym;
}

int bd_unpack_bytes(const uint8_t *sym, int nsym, int pad, uint8_t *out, int cap)
{
    if (nsym < 0 || nsym % BD_GROUP_CELLS != 0) return BD_ESYMBOL;
    if (pad < 0 || pad >= BD_GROUP_BYTES || (nsym == 0 && pad != 0)) return BD_ELENGTH;
    if (nsym > 0 && !sym) return BD_ESYMBOL;
    int groups = nsym / BD_GROUP_CELLS;
    int nbytes = groups * BD_GROUP_BYTES - pad;
    if (cap < nbytes || (nbytes > 0 && !out)) return BD_ECAP;
    /* Check every group before the first write, so a refused read leaves
     * `out` as it was. */
    for (int g = 0; g < groups; g++) {
        uint8_t b[BD_GROUP_BYTES];
        int r = get_group(sym + g * BD_GROUP_CELLS, b);
        if (r) return r;
        /* The pad bytes of the last group are zeros when we wrote them. */
        if (g == groups - 1)
            for (int k = BD_GROUP_BYTES - pad; k < BD_GROUP_BYTES; k++)
                if (b[k]) return BD_ESYMBOL;
    }
    for (int g = 0; g < groups; g++) {
        uint8_t b[BD_GROUP_BYTES];
        (void)get_group(sym + g * BD_GROUP_CELLS, b);
        for (int k = 0; k < BD_GROUP_BYTES; k++) {
            int at = g * BD_GROUP_BYTES + k;
            if (at < nbytes) out[at] = b[k];
        }
    }
    return nbytes;
}

/* ------------------------------------------------------------- the frame */

/* The payload already sits at cells + n (row 1 on). Write the header row and
 * the zeros after the payload. Arguments are checked by the caller. */
static void seal(uint8_t *cells, int n, int kind, int pad, int nsym)
{
    uint8_t h[BD_HEADER_BYTES];
    h[0] = 'B';
    h[1] = 'D';
    h[2] = BD_VERSION;
    h[3] = (uint8_t)kind;
    h[4] = (uint8_t)pad;
    h[5] = (uint8_t)nsym;
    h[6] = (uint8_t)(nsym >> 8);
    h[7] = (uint8_t)(nsym >> 16);
    uint32_t crc = bd_crc32(0, h, BD_HEADER_CRC_AT);
    crc = bd_crc32(crc, cells + n, nsym);
    h[8] = (uint8_t)crc;
    h[9] = (uint8_t)(crc >> 8);
    h[10] = (uint8_t)(crc >> 16);
    h[11] = (uint8_t)(crc >> 24);

    for (int g = 0; g < BD_HEADER_BYTES / BD_GROUP_BYTES; g++)
        put_group(h + g * BD_GROUP_BYTES, cells + g * BD_GROUP_CELLS);
    for (int i = BD_HEADER_CELLS; i < n; i++) cells[i] = 0;
    for (int i = n + nsym; i < n * (n + 1); i++) cells[i] = 0;
}

static int frame_args_ok(int nsym, int kind, int pad, int n)
{
    if (!side_ok(n)) return BD_EGEOMETRY;
    if (kind != BD_KIND_SYMBOLS && kind != BD_KIND_BYTES) return BD_EKIND;
    if (nsym < 0 || nsym > n * n) return BD_ECAP;
    if (kind == BD_KIND_SYMBOLS && pad != 0) return BD_ELENGTH;
    if (kind == BD_KIND_BYTES) {
        if (nsym % BD_GROUP_CELLS != 0) return BD_ESYMBOL;
        if (pad < 0 || pad >= BD_GROUP_BYTES || (nsym == 0 && pad != 0)) return BD_ELENGTH;
    }
    return BD_EOK;
}

int bd_frame(const uint8_t *sym, int nsym, int kind, int pad, int n, uint8_t *cells)
{
    int r = frame_args_ok(nsym, kind, pad, n);
    if (r) return r;
    if (!cells || (nsym > 0 && !sym)) return BD_ECAP;
    for (int i = 0; i < nsym; i++)
        if (sym[i] > 2) return BD_ESYMBOL;
    /* A forward copy: `sym` may be cells + n itself (it is, for
     * bd_encode_bytes), but must not otherwise overlap `cells`. */
    if (sym != cells + n)
        for (int i = 0; i < nsym; i++) cells[n + i] = sym[i];
    seal(cells, n, kind, pad, nsym);
    return BD_EOK;
}

/* Check n * (n + 1) cells and find the payload: it is at cells + n, *nsym
 * long. Reads only; the order of the checks is the one the file header
 * states. A damaged header group (a value of 2^24 or more) is BD_EMAGIC in
 * the first group, which holds the magic, and BD_ECHECK in the others: the
 * first is most likely a picture that is not ours, the others one of ours
 * with a misread cell. */
static int parse(const uint8_t *cells, int n, int *nsym, int *kind, int *pad)
{
    if (!side_ok(n)) return BD_EGEOMETRY;
    if (!cells) return BD_ECAP;
    for (int i = 0; i < n; i++)
        if (cells[i] > 2) return BD_ESYMBOL;

    uint8_t h[BD_HEADER_BYTES];
    for (int g = 0; g < BD_HEADER_BYTES / BD_GROUP_BYTES; g++)
        if (get_group(cells + g * BD_GROUP_CELLS, h + g * BD_GROUP_BYTES))
            return g == 0 ? BD_EMAGIC : BD_ECHECK;

    if (h[0] != 'B' || h[1] != 'D') return BD_EMAGIC;
    if (h[2] != BD_VERSION) return BD_EVERSION;
    if (h[3] != BD_KIND_SYMBOLS && h[3] != BD_KIND_BYTES) return BD_EKIND;
    int k = h[3], p = h[4];
    int count = (int)h[5] | ((int)h[6] << 8) | ((int)h[7] << 16);
    if (count > n * n) return BD_ELENGTH;
    if (k == BD_KIND_SYMBOLS && p != 0) return BD_ELENGTH;
    if (k == BD_KIND_BYTES &&
        (count % BD_GROUP_CELLS != 0 || p >= BD_GROUP_BYTES || (count == 0 && p != 0)))
        return BD_ELENGTH;

    const uint8_t *payload = cells + n;
    for (int i = 0; i < count; i++)
        if (payload[i] > 2) return BD_ESYMBOL;
    uint32_t crc = bd_crc32(0, h, BD_HEADER_CRC_AT);
    crc = bd_crc32(crc, payload, count);
    uint32_t want = (uint32_t)h[8] | ((uint32_t)h[9] << 8) | ((uint32_t)h[10] << 16) |
                    ((uint32_t)h[11] << 24);
    if (crc != want) return BD_ECHECK;

    *nsym = count;
    if (kind) *kind = k;
    if (pad) *pad = p;
    return BD_EOK;
}

int bd_unframe(const uint8_t *cells, int n, uint8_t *sym, int cap, int *kind, int *pad)
{
    int nsym = 0, k = 0, p = 0;
    int r = parse(cells, n, &nsym, &k, &p);
    if (r) return r;
    if (cap < nsym || (nsym > 0 && !sym)) return BD_ECAP;
    for (int i = 0; i < nsym; i++) sym[i] = cells[n + i];
    if (kind) *kind = k;
    if (pad) *pad = p;
    return nsym;
}

int bd_encode_bytes(const uint8_t *bytes, int nbytes, int n, uint8_t *cells)
{
    if (!side_ok(n)) return BD_EGEOMETRY;
    if (!cells) return BD_ECAP;
    int pad = 0;
    /* Pack straight into the payload rows: no buffer of our own. */
    int nsym = bd_pack_bytes(bytes, nbytes, cells + n, n * n, &pad);
    if (nsym < 0) return nsym;
    seal(cells, n, BD_KIND_BYTES, pad, nsym);
    return BD_EOK;
}

int bd_decode_bytes(const uint8_t *cells, int n, uint8_t *out, int cap)
{
    int nsym = 0, kind = 0, pad = 0;
    int r = parse(cells, n, &nsym, &kind, &pad);
    if (r) return r;
    if (kind != BD_KIND_BYTES) return BD_EKIND;
    return bd_unpack_bytes(cells + n, nsym, pad, out, cap);
}

/* ------------------------------------------------------------ the pixels */

static const uint8_t grey_of[BD_LEVELS] = { 255, 128, 0 };

int bd_paint(const uint8_t *cells, int n, int p, uint8_t *rgba)
{
    if (!bd_geometry_ok(n, p)) return BD_EGEOMETRY;
    if (!cells || !rgba) return BD_ECAP;
    int total = n * (n + 1);
    for (int i = 0; i < total; i++)
        if (cells[i] > 2) return BD_ESYMBOL;
    int w = bd_width(n, p), h = bd_height(n, p);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = cells + (y / p) * n;
        uint8_t *px = rgba + (long)y * w * 4;
        for (int x = 0; x < w; x++) {
            uint8_t v = grey_of[row[x / p]];
            px[4 * x + 0] = v;
            px[4 * x + 1] = v;
            px[4 * x + 2] = v;
            px[4 * x + 3] = 255;
        }
    }
    return BD_EOK;
}

int bd_sample(const uint8_t *rgba, int w, int h, int n, uint8_t *cells, BdReading *reading)
{
    if (!side_ok(n)) return BD_EGEOMETRY;
    /* At least a pixel per cell; a picture shrunk below that has lost cells
     * outright and is not read. */
    if (w < n || h < n + 1 || w > BD_MAX_READ_SIDE || h > BD_MAX_READ_SIDE) return BD_EGEOMETRY;
    if (!rgba || !cells) return BD_ECAP;

    int rows = n + 1, risky = 0, min_margin = 64;
    for (int cy = 0; cy < rows; cy++) {
        /* The centre of the cell, where this picture's own size puts it:
         * floor((2c + 1) * size / (2 * count)), always inside the picture. */
        long y = ((2L * cy + 1) * h) / (2L * rows);
        for (int cx = 0; cx < n; cx++) {
            long x = ((2L * cx + 1) * w) / (2L * n);
            const uint8_t *px = rgba + (y * w + x) * 4;
            int s = px[0] + px[1] + px[2];
            uint8_t level = s > BD_SUM_HI ? 0 : s < BD_SUM_LO ? 2 : 1;
            int dlo = s - BD_SUM_LO, dhi = s - BD_SUM_HI;
            if (dlo < 0) dlo = -dlo;
            if (dhi < 0) dhi = -dhi;
            int margin = (dlo < dhi ? dlo : dhi) / 3;
            if (margin > 64) margin = 64;
            if (margin <= BD_RISKY_MARGIN) risky++;
            if (margin < min_margin) min_margin = margin;
            cells[cy * n + cx] = level;
        }
    }
    if (reading) {
        reading->cells = rows * n;
        reading->risky = risky;
        reading->min_margin = min_margin;
    }
    return BD_EOK;
}
