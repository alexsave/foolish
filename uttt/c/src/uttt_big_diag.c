/* uttt_big_diag.c - see uttt_big_diag.h; this file is its only
 * implementation. */
#include "uttt_big_diag.h"
#include "uttt_big_msg.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ImageIO's luma tables, quality 0.00 to 1.00 (generated; see the file). */
#include "uttt_jpeg_imageio.inc"

/* ============================================================== the JPEG */

static void estimate(UbdJpeg *o)
{
    /* EXACT: every ImageIO step whose table is this one. */
    o->q_lo = o->q_hi = -1;
    for (int pct = 0; pct <= 100; pct++) {
        int same = 1;
        for (int k = 0; k < 64 && same; k++) same = ubd_imageio[pct][k] == o->luma[k];
        if (!same) continue;
        if (o->q_lo < 0) o->q_lo = pct * 10;
        o->q_hi = pct * 10;
    }
    o->q_exact = o->q_lo >= 0;

    /* INTERPOLATED on the sums, which never rise with the quality: the last
     * step at or above this sum, and the fraction of the way to the next. */
    int s = o->luma_sum;
    if (o->q_exact) {
        o->q_est = o->q_lo;
    } else if (s >= ubd_imageio_sum[0]) {
        o->q_est = 0;
    } else if (s <= ubd_imageio_sum[100]) {
        o->q_est = 1000;
    } else {
        int p = 0;
        while (p < 100 && ubd_imageio_sum[p + 1] >= s) p++;
        int hi = ubd_imageio_sum[p], lo = ubd_imageio_sum[p + 1];   /* hi >= s > lo */
        o->q_est = p * 10 + (10 * (hi - s) + (hi - lo) / 2) / (hi - lo);
    }

    /* THE LIBJPEG SCALING, for an encoder that is not ImageIO: libjpeg
     * writes the Annex K table times scale / 100, and the quality that
     * scale comes from is 50 / (scale / 100) under 50, (200 - scale) / 2
     * from 50 up. The scale is read back as the tables' ratio of sums. */
    static const int annex_k_sum = 3688;   /* the sum of Annex K's luma table */
    double scale = 100.0 * s / annex_k_sum;
    double q = scale <= 100.0 ? (200.0 - scale) / 2.0 : 5000.0 / scale;
    if (q < 1) q = 1;
    if (q > 100) q = 100;
    o->q_ijg = (int)(q * 10 + 0.5);
}

int ubd_jpeg_parse(const uint8_t *p, long n, UbdJpeg *o)
{
    memset(o, 0, sizeof *o);
    o->q_exact = 0;
    o->q_lo = o->q_hi = o->q_est = o->q_ijg = -1;
    if (!p || n < 4 || p[0] != 0xFF || p[1] != 0xD8) return o->status = UBD_JPEG_NOT_JPEG;

    uint16_t table[4][64];
    int have[4] = { 0, 0, 0, 0 }, bits[4] = { 0, 0, 0, 0 };
    int frame = 0, luma_tq = -1;
    long i = 2;
    for (;;) {
        if (i >= n || p[i] != 0xFF) return o->status = UBD_JPEG_TRUNCATED;
        while (i < n && p[i] == 0xFF) i++;              /* fill bytes */
        if (i >= n) return o->status = UBD_JPEG_TRUNCATED;
        int m = p[i++];
        /* the markers that stand alone, with no length */
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) break;                            /* EOI before a scan */
        if (i + 2 > n) return o->status = UBD_JPEG_TRUNCATED;
        long len = ((long)p[i] << 8) | p[i + 1];
        if (len < 2 || i + len > n) return o->status = UBD_JPEG_TRUNCATED;
        const uint8_t *s = p + i + 2;
        long sl = len - 2;
        if (m == 0xDA) break;                            /* the first scan */
        if (m == 0xDB) {
            long k = 0;
            while (k < sl) {
                int pq = s[k] >> 4, tq = s[k] & 15;
                k++;
                long need = pq ? 128 : 64;
                if (pq > 1 || tq > 3 || k + need > sl) return o->status = UBD_JPEG_TRUNCATED;
                for (int z = 0; z < 64; z++)
                    table[tq][z] = pq ? (uint16_t)((s[k + 2 * z] << 8) | s[k + 2 * z + 1]) : s[k + z];
                have[tq] = 1;
                bits[tq] = pq ? 16 : 8;
                k += need;
            }
        } else if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (sl < 6) return o->status = UBD_JPEG_BAD_FRAME;
            int nf = s[5];
            if (nf < 1 || sl < 6 + 3L * nf) return o->status = UBD_JPEG_BAD_FRAME;
            frame = 1;
            o->sof = m;
            o->precision = s[0];
            o->height = (s[1] << 8) | s[2];
            o->width = (s[3] << 8) | s[4];
            o->components = nf;
            for (int c = 0; c < nf && c < 4; c++) {
                o->h[c] = s[6 + 3 * c + 1] >> 4;
                o->v[c] = s[6 + 3 * c + 1] & 15;
            }
            luma_tq = s[6 + 2] & 15;
        } else if (m == 0xE0 && sl >= 5 && !memcmp(s, "JFIF", 5)) {
            o->jfif = 1;
        } else if (m == 0xE1 && sl >= 6 && !memcmp(s, "Exif\0", 6)) {
            o->exif = 1;
        } else if (m == 0xE2 && sl >= 12 && !memcmp(s, "ICC_PROFILE", 12)) {
            o->icc = 1;
        } else if (m == 0xEE && sl >= 5 && !memcmp(s, "Adobe", 5)) {
            o->adobe = 1;
        }
        i += len;
    }
    if (!frame) return o->status = UBD_JPEG_NO_FRAME;
    if (luma_tq >= 0 && luma_tq < 4 && have[luma_tq]) {
        o->has_luma_table = 1;
        o->luma_bits = bits[luma_tq];
        for (int z = 0; z < 64; z++) {
            o->luma[z] = table[luma_tq][z];
            o->luma_sum += o->luma[z];
        }
        estimate(o);
    }
    return o->status = UBD_JPEG_OK;
}

const char *ubd_jpeg_chroma(const UbdJpeg *j)
{
    if (j->components == 1) return "grey";
    if (j->components != 3) return "other";
    int lh = j->h[0], lv = j->v[0], ch = j->h[1], cv = j->v[1];
    if (ch != j->h[2] || cv != j->v[2] || ch < 1 || cv < 1) return "other";
    if (lh == ch && lv == cv) return "4:4:4";
    if (lh == 2 * ch && lv == 2 * cv) return "4:2:0";
    if (lh == 2 * ch && lv == cv) return "4:2:2";
    if (lh == ch && lv == 2 * cv) return "4:4:0";
    return "other";
}

const uint8_t *ubd_imageio_luma(int pct)
{
    return pct >= 0 && pct <= 100 ? ubd_imageio[pct] : NULL;
}

/* ============================================================= the greys */

/* bubble_data.h's geometry: a side of at least 64 cells, under 1200 at one
 * pixel a cell, and a picture of at most 16384 a side to read. */
#define UBD_MIN_SIDE       64
#define UBD_MAX_SIDE       1199
#define UBD_MAX_READ_SIDE  16384

int ubd_luma(const uint8_t *rgba, int w, int h, int n, UbdLuma *out)
{
    memset(out, 0, sizeof *out);
    if (n < UBD_MIN_SIDE || n > UBD_MAX_SIDE || w < n || h < n + 1 ||
        w > UBD_MAX_READ_SIDE || h > UBD_MAX_READ_SIDE || !rgba) return -1;
    double sum[3] = { 0, 0, 0 }, sq[3] = { 0, 0, 0 };
    int lo[3] = { 765, 765, 765 }, hi[3] = { 0, 0, 0 };
    int rows = n + 1, min_margin = 64;
    for (int cy = 0; cy < rows; cy++) {
        /* bd_sample's centre: floor((2c + 1) * size / (2 * count)) */
        long y = ((2L * cy + 1) * h) / (2L * rows);
        for (int cx = 0; cx < n; cx++) {
            long x = ((2L * cx + 1) * w) / (2L * n);
            const uint8_t *px = rgba + (y * w + x) * 4;
            int s = px[0] + px[1] + px[2];
            int c = s > UBD_SUM_HI ? UBD_CLASS_EMPTY : s < UBD_SUM_LO ? UBD_CLASS_O : UBD_CLASS_X;
            int dlo = s - UBD_SUM_LO, dhi = s - UBD_SUM_HI;
            if (dlo < 0) dlo = -dlo;
            if (dhi < 0) dhi = -dhi;
            int margin = (dlo < dhi ? dlo : dhi) / 3;
            if (margin > 64) margin = 64;
            if (margin <= UBD_RISKY) out->risky++;
            if (margin < min_margin) min_margin = margin;
            int dev = s - 3 * UBD_NOMINAL(c);
            if (dev < 0) dev = -dev;
            if (dev > out->worst_dev3) out->worst_dev3 = dev;
            /* ROW 0 IS NOT THE BOARD: the kit's header, then empty */
            if (cy == 0) {
                UbdPart *part = cx < UBD_HEADER_CELLS ? &out->header : &out->rest;
                part->cells++;
                part->n[c]++;
                if (dev > part->worst_dev3) part->worst_dev3 = dev;
                continue;
            }
            if (dev > out->board_worst_dev3) out->board_worst_dev3 = dev;
            out->cls[c].n++;
            sum[c] += s;
            sq[c] += (double)s * s;
            if (s < lo[c]) lo[c] = s;
            if (s > hi[c]) hi[c] = s;
            out->hist[(s / 3) / (256 / UBD_BUCKETS)]++;
        }
    }
    out->cells = rows * n;
    out->board_cells = n * n;
    out->min_margin = min_margin;
    for (int c = 0; c < 3; c++) {
        UbdClass *k = &out->cls[c];
        if (!k->n) continue;
        double mean = sum[c] / k->n;
        double var = sq[c] / k->n - mean * mean;
        k->mean = mean / 3.0;
        k->sd = var > 0 ? sqrt(var) / 3.0 : 0;
        k->min = lo[c] / 3;
        k->max = (hi[c] + 2) / 3;
    }
    return 0;
}

/* ============================================================== the ring */

static const char *result_names[UBD_R_COUNT] = {
    "BD_EOK", "BD_EGEOMETRY", "BD_ECAP", "BD_EMAGIC", "BD_EVERSION", "BD_EKIND",
    "BD_ELENGTH", "BD_ECHECK", "BD_ESYMBOL", "image", "no picture", "not read",
};

const char *ubd_result_name(int r)
{
    return r >= 0 && r < UBD_R_COUNT ? result_names[r] : "?";
}

static int ring_ok(const uint8_t *b, int n)
{
    if (!b || n < 4 || b[0] != 'U' || b[1] != 'R' || b[2] != UBD_RING_VERSION) return 0;
    if (b[3] > UBD_RING_MAX || n != 4 + b[3] * UBD_REC_LEN) return 0;
    return 1;
}

int ubd_ring_count(const uint8_t *ring, int n) { return ring_ok(ring, n) ? ring[3] : 0; }

static uint32_t get_le(const uint8_t *p, int k)
{
    uint32_t v = 0;
    for (int i = k - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void put_le(uint8_t *p, uint32_t v, int k)
{
    for (int i = 0; i < k; i++) { p[i] = (uint8_t)v; v >>= 8; }
}

int ubd_ring_get(const uint8_t *ring, int n, int i, UbdEvent *out)
{
    if (i < 0 || i >= ubd_ring_count(ring, n)) return 0;
    const uint8_t *r = ring + 4 + i * UBD_REC_LEN;
    UbdEvent e;
    e.when = (int64_t)get_le(r, 4);
    e.role = r[4];
    e.q = r[5] == 255 ? -1 : r[5] * 10;
    e.width = (int)get_le(r + 6, 2);
    e.height = (int)get_le(r + 8, 2);
    e.bytes = (long)get_le(r + 10, 4);
    uint32_t risky = get_le(r + 14, 2);
    e.risky = risky == 0xFFFF ? -1 : (int)risky;
    e.min_margin = r[16] == 255 ? -1 : r[16];
    e.result = r[17] < UBD_R_COUNT ? r[17] : UBD_R_NOT_READ;
    e.flags = r[18];
    *out = e;
    return 1;
}

static uint32_t clamp_u(long v, uint32_t max)
{
    return v < 0 ? 0 : (unsigned long)v > max ? max : (uint32_t)v;
}

int ubd_ring_push(const uint8_t *ring, int n, const UbdEvent *e, uint8_t *out, int cap)
{
    int keep = ubd_ring_count(ring, n);
    if (keep > UBD_RING_MAX - 1) keep = UBD_RING_MAX - 1;
    int len = 4 + (keep + 1) * UBD_REC_LEN;
    if (!out || cap < len || !e) return -1;
    out[0] = 'U';
    out[1] = 'R';
    out[2] = UBD_RING_VERSION;
    out[3] = (uint8_t)(keep + 1);
    uint8_t *r = out + 4;
    memset(r, 0, UBD_REC_LEN);
    put_le(r, clamp_u((long)e->when, 0xFFFFFFFFu), 4);
    r[4] = (uint8_t)e->role;
    /* the quality to the step: the ring is a glance, the report has the rest */
    r[5] = e->q < 0 ? 255 : (uint8_t)clamp_u((e->q + 5) / 10, 100);
    put_le(r + 6, clamp_u(e->width, 0xFFFF), 2);
    put_le(r + 8, clamp_u(e->height, 0xFFFF), 2);
    put_le(r + 10, clamp_u(e->bytes, 0xFFFFFFFFu), 4);
    put_le(r + 14, e->risky < 0 ? 0xFFFF : clamp_u(e->risky, 0xFFFE), 2);
    r[16] = e->min_margin < 0 ? 255 : (uint8_t)clamp_u(e->min_margin, 254);
    r[17] = (uint8_t)(e->result >= 0 && e->result < UBD_R_COUNT ? e->result : UBD_R_NOT_READ);
    r[18] = (uint8_t)(e->flags & 0xFF);
    if (keep) memcpy(out + 4 + UBD_REC_LEN, ring + 4, (size_t)keep * UBD_REC_LEN);
    return len;
}

/* ============================================================ the report */

typedef struct {
    char *out;
    int   cap, n, cut;
} Text;

/* One line, cut at UBD_LINE_MAX: no line of the report is ever wider. */
static void line(Text *t, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (k < 0) k = 0;
    if (k > UBD_LINE_MAX) k = UBD_LINE_MAX;
    buf[k] = 0;
    if (t->cut || t->n + k + 2 > t->cap) { t->cut = 1; return; }
    memcpy(t->out + t->n, buf, (size_t)k);
    t->n += k;
    t->out[t->n++] = '\n';
    t->out[t->n] = 0;
}

/* The mark beside a judged value: blank when it is what was expected, "!"
 * when it is not. */
static const char *mark(int ok) { return ok ? " " : "!"; }

static const char *str(const char *s) { return s && *s ? s : "?"; }

void ubd_civil(int64_t seconds, UbdCivil *c)
{
    /* Days and the time of day, with the remainder kept non-negative so a
     * moment before 1970 breaks down the same way as one after it. */
    int64_t days = seconds / 86400, rem = seconds % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    c->hour = (int)(rem / 3600);
    c->minute = (int)(rem % 3600 / 60);
    c->second = (int)(rem % 60);
    /* The proleptic Gregorian date of a day count (Howard Hinnant's
     * days-from-civil, inverted): eras of 400 years, 146,097 days each,
     * counted from 0000-03-01 so a leap day is the last day of a year. */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    c->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    c->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    c->year = (int)(yoe + era * 400 + (c->month <= 2));
}

static void clock_of(int64_t when, int utc_offset, char *out, int cap, int with_day)
{
    UbdCivil c;
    ubd_civil(when + utc_offset, &c);
    if (with_day)
        snprintf(out, (size_t)cap, "%02d %02d:%02d:%02d", c.day, c.hour, c.minute, c.second);
    else
        snprintf(out, (size_t)cap, "%04d-%02d-%02d %02d:%02d:%02d", c.year, c.month, c.day,
                 c.hour, c.minute, c.second);
}

static void len_of(char *out, int cap, int n)
{
    if (n < 0) snprintf(out, (size_t)cap, "nil");
    else snprintf(out, (size_t)cap, "%d", n);
}

/* A quality in thousandths as text: two decimals when the third is zero
 * (0.89), three when not (0.893). */
static int q_text(char *out, int cap, int q)
{
    if (q % 10 == 0) return snprintf(out, (size_t)cap, "%d.%02d", q / 1000, (q % 1000) / 10);
    return snprintf(out, (size_t)cap, "%d.%03d", q / 1000, q % 1000);
}

/* "0.89", or "0.89-0.91" for a range */
static void q_of(char *out, int cap, int lo, int hi)
{
    if (lo < 0) { snprintf(out, (size_t)cap, "?"); return; }
    int k = q_text(out, cap, lo);
    if (hi > lo && k > 0 && k < cap - 1) {
        out[k] = '-';
        q_text(out + k + 1, cap - k - 1, hi);
    }
}

/* What a picture of the big board is expected to be (shared/tools/
 * layout_probe/README.md, measured 2026-10-01): robust243 is 729 x 732 px;
 * the sender's extension writes it at quality 0.50, 4:2:0, about 255 KB on
 * a mid-game board; the transport's transcoder writes it again at about
 * 0.89 (ImageIO's 0.89-0.91 table), 4:2:0, about 460 KB; a side over
 * 1200 px is cut to 1200. */
#define UBD_EXPECT_W   (UTB_SIDE * 3)
#define UBD_EXPECT_H   ((UTB_SIDE + 1) * 3)
#define UBD_Q_SENDER   500
#define UBD_Q_TRANSPORT_LO 890
#define UBD_Q_TRANSPORT_HI 910

static int q_is(int lo, int hi, int want_lo, int want_hi)
{
    return lo >= 0 && hi >= lo && lo <= want_hi && hi >= want_lo;
}

static uint32_t crc_of_text(const char *s)
{
    return s ? utb_board_check((const uint8_t *)s, (int)strlen(s)) : 0;
}

static const char *from_name(int from)
{
    switch (from) {
    case UBD_FROM_SELECTED:    return "selectedMessage";
    case UBD_FROM_DID_SELECT:  return "didSelect";
    case UBD_FROM_DID_RECEIVE: return "didReceive";
    default:                   return "no message (+ menu)";
    }
}

static const char *utm_error_name(int r)
{
    switch (r) {
    case UTM_EOK:     return "UTM_EOK";
    case UTM_ESHORT:  return "UTM_ESHORT";
    case UTM_EMAGIC:  return "UTM_EMAGIC";
    case UTM_EFORMAT: return "UTM_EFORMAT";
    case UTM_EFLAGS:  return "UTM_EFLAGS";
    case UTM_ECHECK:  return "UTM_ECHECK";
    case UTM_EGAME:   return "UTM_EGAME";
    case UTM_EROSTER: return "UTM_EROSTER";
    case UTM_ECAP:    return "UTM_ECAP";
    case UTM_ETEXT:   return "UTM_ETEXT";
    case UTB_EBOARD:  return "UTB_EBOARD";
    default:          return "?";
    }
}

static void kb_of(char *out, int cap, long bytes)
{
    snprintf(out, (size_t)cap, "%ld.%ldK", bytes / 1000, (bytes % 1000) / 100);
}

static void section_message(Text *t, const UbdFacts *f)
{
    char when[32];
    clock_of(f->now, f->utc_offset, when, sizeof when, 0);
    line(t, "243 PICTURE DIAGNOSTICS");
    line(t, "app %s (%s) %s", str(f->app_version), str(f->app_build), str(f->install));
    line(t, "iOS %s, %s", str(f->os_version), str(f->model));
    line(t, "at %s", when);
    line(t, "");
    line(t, "== the message");
    line(t, "opened by %s", from_name(f->from));
    if (f->from == UBD_FROM_NONE) return;
    line(t, "sent by %s", f->who == UBD_WHO_ME ? "this device" : f->who == UBD_WHO_OTHER ? "the other person" : "?");
    line(t, "pending %s", f->pending ? "yes" : "no");
    if (f->session) line(t, "session %08x", (unsigned)crc_of_text(f->session));
    else line(t, "session none");
}

/* "SEAT HERE": this device's seat, which only the kernel's one resident big
 * game can say, and only when that game is this link's. The resident is the
 * last big game the drawer read, staged or played - often not the bubble the
 * report is on: my own reply staged over the bubble I opened, or a newer
 * bubble of the game shown instead. So: the same link, the seat; the same
 * game at another ply, the seat and that ply; another game or none, "?" and
 * which. (Build 1.1(16) said "not the game on screen" for all of the last
 * three, which read as if the drawer showed a different game.) */
static void seat_lines(Text *t, const UbdFacts *f, const UtbHead *link)
{
    static const char *seats[] = { "spectator", "X", "O", "waiting (my invitation)", "open (X is mine to take)" };
    static const char *bys[] = { "nothing", "record", "tag", "sender" };
    UtbHead r;
    if (!f->resident || utb_msg_text_peek(f->resident, &r) != UTM_EOK) {
        line(t, "seat here ? - the app holds no 243 game");
        return;
    }
    /* the same position, by the links' heads rather than their spelling, so
     * a link Messages re-spelled is still this one */
    int game = utb_head_same_game(&r, link);
    int same = !strcmp(f->resident, f->url) ||
               (game && r.n_plies == link->n_plies && r.board_check == link->board_check);
    if (!same && !game) {
        line(t, "seat here ? - the app holds another game");
        return;
    }
    int s = f->resident_seat, b = f->resident_by;
    line(t, "seat here %s, by %s", s >= 0 && s <= 4 ? seats[s] : "?", b >= 0 && b <= 3 ? bys[b] : "?");
    if (!same)
        line(t, "~ as at ply %ld, which the app holds", f->resident_plies);
}

/* Returns the URL's board check, and *known = 1, when the link is a big
 * game's that peeks. */
static uint32_t section_url(Text *t, const UbdFacts *f, int *known)
{
    *known = 0;
    line(t, "");
    line(t, "== the link");
    if (!f->url) { line(t, "none"); return 0; }
    uint8_t b[64];
    int nb = utb_msg_text_bytes(f->url, b, sizeof b);
    if (nb < 0) {
        line(t, "%d chars, no m= value (%s)", (int)strlen(f->url), utm_error_name(nb));
        return 0;
    }
    line(t, "%d chars, %d bytes", (int)strlen(f->url), nb);
    int format = nb >= 2 ? b[1] : -1;
    line(t, "%sformat %d%s", "", format,
         format == UTB_FORMAT ? " (243)" : format == 2 || format == 1 ? " (9 x 9)" : "");
    UtbHead h;
    int r = utb_msg_peek(b, nb, &h);
    line(t, "%s header and check %s", mark(r == UTM_EOK), utm_error_name(r));
    if (r != UTM_EOK) return 0;
    line(t, "seed %ld", (long)h.seed);
    line(t, "flags 0x%02x, %s", nb >= 7 ? b[6] : 0, h.sealed ? "sealed (X taken)" : "open (X free)");
    if (h.last < 0) line(t, "plies %ld, last none", (long)h.n_plies);
    else line(t, "plies %ld, last %ld", (long)h.n_plies, (long)h.last);
    seat_lines(t, f, &h);
    line(t, "board crc %08x", (unsigned)h.board_check);
    *known = 1;
    return h.board_check;
}

static void section_layout(Text *t, const UbdFacts *f, UbdJpeg *j)
{
    char a[16], b[16], c[16];
    line(t, "");
    line(t, "== the layout");
    line(t, "%s", f->layout ? f->layout : "none");
    if (!f->layout) return;
    len_of(a, sizeof a, f->caption_len);
    len_of(b, sizeof b, f->subcaption_len);
    len_of(c, sizeof c, f->summary_len);
    line(t, "lengths caption %s sub %s summary %s", a, b, c);
    if (f->has_image) {
        int ok = f->image_w == UBD_EXPECT_W && f->image_h == UBD_EXPECT_H;
        line(t, "%s image %d x %d (expect %d x %d)", mark(ok), f->image_w, f->image_h,
             UBD_EXPECT_W, UBD_EXPECT_H);
        line(t, "  image scale %d.%02d", f->image_scale_pct / 100, f->image_scale_pct % 100);
    } else {
        line(t, "! image none (expect one)");
    }
    if (!f->has_file) { line(t, "! file none (expect a jpeg)"); return; }
    line(t, "%s file .%s (expect .jpeg)", mark(f->file_ext && !strcmp(f->file_ext, "jpeg")), str(f->file_ext));
    line(t, "~ bytes %ld, not judged:", f->file_bytes);
    line(t, "    a full board is ~255K at q0.50 and");
    line(t, "    ~460K at q0.89; fewer marks, fewer bytes");

    ubd_jpeg_parse(f->file, f->file_n, j);
    line(t, "");
    line(t, "== the jpeg, from the file's bytes");
    if (j->status != UBD_JPEG_OK) {
        static const char *why[] = { "", "not a JPEG", "cut short", "no frame", "a bad frame" };
        line(t, "! not read: %s", why[-j->status]);
        return;
    }
    line(t, "%s size %d x %d (expect %d x %d)", mark(j->width == UBD_EXPECT_W && j->height == UBD_EXPECT_H),
         j->width, j->height, UBD_EXPECT_W, UBD_EXPECT_H);
    if (j->width >= 1200 || j->height >= 1200) line(t, "! a side at 1200: the transport's cut");
    line(t, "%s components %d (expect 3)", mark(j->components == 3), j->components);
    const char *ch = ubd_jpeg_chroma(j);
    line(t, "%s chroma %s, luma %dx%d (expect 4:2:0)", mark(!strcmp(ch, "4:2:0")), ch, j->h[0], j->v[0]);
    const char *kind = j->sof == UBD_SOF_BASELINE ? "baseline" : j->sof == UBD_SOF_PROGRESSIVE ? "progressive"
                     : j->sof == UBD_SOF_EXTENDED ? "extended" : "other";
    line(t, "%s frame %s SOF%d (expect baseline)", mark(j->sof == UBD_SOF_BASELINE), kind, j->sof - 0xC0);
    line(t, "  precision %d bit, markers%s%s%s%s%s", j->precision, j->jfif ? " JFIF" : "", j->exif ? " Exif" : "",
         j->icc ? " ICC" : "", j->adobe ? " Adobe" : "", j->jfif || j->exif || j->icc || j->adobe ? "" : " none");
    if (!j->has_luma_table) { line(t, "! quality ? (no luma table)"); return; }
    /* THE QUALITY THAT COPY SHOULD HAVE: a copy from the other person went
     * through the transport (0.89); this device's own may be either. */
    int theirs = f->who == UBD_WHO_OTHER;
    char q[32];
    int in_sender, in_transport;
    if (j->q_exact) {
        q_of(q, sizeof q, j->q_lo, j->q_hi);
        in_sender = q_is(j->q_lo, j->q_hi, UBD_Q_SENDER, UBD_Q_SENDER);
        in_transport = q_is(j->q_lo, j->q_hi, UBD_Q_TRANSPORT_LO, UBD_Q_TRANSPORT_HI);
    } else {
        q_of(q, sizeof q, j->q_est, j->q_est);
        in_sender = j->q_est >= UBD_Q_SENDER - 20 && j->q_est <= UBD_Q_SENDER + 20;
        in_transport = j->q_est >= UBD_Q_TRANSPORT_LO - 20 && j->q_est <= UBD_Q_TRANSPORT_HI + 20;
    }
    int qok = theirs ? in_transport : in_sender || in_transport;
    line(t, "%s quality %s%s (expect %s)", mark(qok), j->q_exact ? "" : "~", q, theirs ? "0.89" : "0.50 or 0.89");
    line(t, "  %s ImageIO table, luma sum %d", j->q_exact ? "an exact" : "no", j->luma_sum);
    line(t, "  libjpeg's scaling would say q%d.%d", j->q_ijg / 10, j->q_ijg % 10);
}

/* A deviation in thirds of a level as "18.0" (one decimal, exact thirds). */
static void dev_of(char *out, int cap, int dev3)
{
    snprintf(out, (size_t)cap, "%d.%d", dev3 / 3, (dev3 % 3) * 10 / 3);
}

/* Two lines for a part of row 0: its cells and how they read, its worst. */
static void part_lines(Text *t, const char *name, const UbdPart *p)
{
    char d[16];
    dev_of(d, sizeof d, p->worst_dev3);
    line(t, "%s %d cells, worst deviation %s", name, p->cells, d);
    line(t, "  read empty %d, X %d, O %d", p->n[UBD_CLASS_EMPTY], p->n[UBD_CLASS_X], p->n[UBD_CLASS_O]);
}

static void section_reading(Text *t, const UbdFacts *f, int url_known, uint32_t url_crc)
{
    line(t, "");
    line(t, "== the reading, at the size it came");
    line(t, "%s result %s", mark(f->read_result == UBD_R_OK), ubd_result_name(f->read_result));
    if (f->read_result == UBD_R_NO_PICTURE || f->read_result == UBD_R_NOT_READ) return;
    if (f->read_cells > 0)
        line(t, "%s cells %d, risky %d, min margin %d", mark(f->read_risky == 0), f->read_cells,
             f->read_risky, f->read_min_margin);
    if (f->symbols) {
        uint32_t crc = utb_board_check(f->symbols, UTB_CELLS);
        if (url_known)
            line(t, "%s board crc %08x %s link", mark(crc == url_crc), (unsigned)crc,
                 crc == url_crc ? "=" : "!=");
        else
            line(t, "  board crc %08x (no link to hold)", (unsigned)crc);
        int count[3] = { 0, 0, 0 };
        for (int i = 0; i < UTB_CELLS; i++) if (f->symbols[i] < 3) count[f->symbols[i]]++;
        line(t, "  marks X %d, O %d, empty %d", count[1], count[2], count[0]);
    }
    line(t, "  read %d.%d ms", f->read_us / 1000, (f->read_us % 1000) / 100);
    if (!f->rgba) return;
    UbdLuma l;
    if (ubd_luma(f->rgba, f->rgba_w, f->rgba_h, UTB_SIDE, &l) != 0) {
        line(t, "! greys: a size the kit cannot sample");
        return;
    }
    /* THE BOARD'S CELLS (rows 1..243) by class, then row 0 apart: the
     * header's 64 cells are greys and blacks that are not marks, and
     * counting them as X and O put 20 marks on a board of one (phone,
     * build 1.1(16)). */
    char d[16];
    line(t, "");
    line(t, "== the greys, sampled where the kit reads");
    line(t, "luminance (r+g+b)/3 by class as read");
    line(t, "the board, rows 1-%d: %d cells", UTB_SIDE, l.board_cells);
    static const char *names[3] = { "empty", "X", "O" };
    for (int c = 0; c < 3; c++) {
        const UbdClass *k = &l.cls[c];
        line(t, "%s (painted %d): %d cell%s", names[c], UBD_NOMINAL(c), k->n, k->n == 1 ? "" : "s");
        if (!k->n) continue;
        line(t, "  mean %.1f sd %.2f min %d max %d", k->mean, k->sd, k->min, k->max);
    }
    dev_of(d, sizeof d, l.board_worst_dev3);
    line(t, "  board worst deviation %s", d);
    line(t, "board histogram, 16 levels a bucket, 0 first:");
    char row[128];
    int at = 0;
    for (int b = 0; b < UBD_BUCKETS; b++) {
        at += snprintf(row + at, sizeof row - (size_t)at, "%s%d", at ? " " : "", l.hist[b]);
        if (b == 7 || b == 15) { line(t, "  %s", row); at = 0; }
    }
    line(t, "row 0, the kit's header and CRC, not marks:");
    part_lines(t, "header", &l.header);
    part_lines(t, "rest of row 0", &l.rest);
    dev_of(d, sizeof d, l.worst_dev3);
    line(t, "worst deviation from nominal: %s", d);
    line(t, "  (every cell, row 0 and the board)");
}

static void section_history(Text *t, const UbdFacts *f)
{
    int n = ubd_ring_count(f->ring, f->ring_n);
    line(t, "");
    line(t, "== history, newest first (%d)", n);
    line(t, "day time role px bytes quality, then");
    line(t, "  risky r, min margin m, result");
    for (int i = 0; i < n; i++) {
        UbdEvent e;
        if (!ubd_ring_get(f->ring, f->ring_n, i, &e)) break;
        char when[32], kb[24], q[16], rm[24];
        clock_of(e.when, f->utc_offset, when, sizeof when, 1);
        if (e.bytes) kb_of(kb, sizeof kb, e.bytes); else snprintf(kb, sizeof kb, "-");
        if (e.q < 0) snprintf(q, sizeof q, "q?");
        else {
            q[0] = 'q';
            q[1] = e.flags & UBD_EV_Q_EXACT ? '=' : '~';
            q_text(q + 2, (int)sizeof q - 2, e.q);
        }
        if (e.risky < 0) snprintf(rm, sizeof rm, "-");
        else snprintf(rm, sizeof rm, "r%d m%d", e.risky, e.min_margin);
        const char *res = e.result == UBD_R_OK ? ((e.flags & UBD_EV_CRC_KNOWN) && !(e.flags & UBD_EV_CRC_SAME) ? "crc!" : "ok")
                        : e.result == UBD_R_NOT_READ ? "" : ubd_result_name(e.result);
        /* TWO LINES A RECORD, so the widest (a 1200 px picture, a 1.4 MB
         * file, a refusal's name) never meets the line's cut */
        line(t, "%s %s %dx%d %s %s", when, e.role == UBD_ROLE_SENT ? "sent" : "open",
             e.width, e.height, kb, q);
        char row[64];
        int k = snprintf(row, sizeof row, "  %s %s", rm, res);
        while (k > 0 && row[k - 1] == ' ') row[--k] = 0;
        line(t, "%s", row);
    }
}

int ubd_report(const UbdFacts *f, char *out, int cap)
{
    if (!f || !out || cap < 1) return -1;
    Text t = { out, cap, 0, 0 };
    out[0] = 0;
    section_message(&t, f);
    int known = 0;
    uint32_t crc = 0;
    UbdJpeg j;
    memset(&j, 0, sizeof j);
    if (f->from != UBD_FROM_NONE) {
        crc = section_url(&t, f, &known);
        section_layout(&t, f, &j);
        section_reading(&t, f, known, crc);
    }
    section_history(&t, f);
    return t.cut ? -1 : t.n;
}

UbdEvent ubd_event_of(const UbdFacts *f, int role)
{
    UbdEvent e;
    memset(&e, 0, sizeof e);
    e.when = f->now;
    e.role = role;
    e.q = -1;
    e.width = f->image_w;
    e.height = f->image_h;
    e.bytes = f->has_file ? f->file_bytes : 0;
    if (f->has_file && f->file) {
        UbdJpeg j;
        if (ubd_jpeg_parse(f->file, f->file_n, &j) == UBD_JPEG_OK) {
            e.width = j.width;
            e.height = j.height;
            e.q = j.q_est;
            if (j.q_exact) e.flags |= UBD_EV_Q_EXACT;
        }
    }
    e.result = f->read_result;
    e.risky = f->read_cells > 0 ? f->read_risky : -1;
    e.min_margin = f->read_cells > 0 ? f->read_min_margin : -1;
    if (f->symbols && f->url) {
        UtbHead h;
        if (utb_msg_text_peek(f->url, &h) == UTM_EOK) {
            e.flags |= UBD_EV_CRC_KNOWN;
            if (utb_board_check(f->symbols, UTB_CELLS) == h.board_check) e.flags |= UBD_EV_CRC_SAME;
        }
    }
    return e;
}
