// The replay code's extras blob - see replay_extras.h for the wire layout and
// why it is not free to move.
//
// NO LIBM. This file is compiled into bots.wasm, which is freestanding: there
// is no log() and no pow() to call, and clang would fail the link rather than
// emit one. Both would also be a liability if they existed - the same C is
// built at -O3 -ffast-math for the native tests, at -O2 for the iOS slices and
// at -Oz for wasm, and a curve that is one transcendental call away from its
// answer is a curve those three builds can disagree about. So the log curve is
// walked by repeated multiplication instead, which is exact in the only sense
// that matters here: every build walks the same products in the same order.
//
// The old reference this replaces computed round(log_B(x)) directly. The walk
// finds the same integer by its definition - the v whose bucket
// [B^(v-0.5), B^(v+0.5)) contains x - and can only differ where the true value
// sits within ~1e-12 of a bucket edge, on a quantization already 7% coarse.
#include "replay_extras.h"
#include "replay.h"   // replay_b32_encode - the code and the link share one alphabet
#include <stdint.h>
#include <string.h>

// The curve, and the exponent bias for the stored unit. Wire format.
#define TIME_B      1.072
#define TIME_BIAS   64

// ---------- little-endian readers over the packed argument blob -------------

static int rd_u8(const unsigned char *b, int len, int *p, int *v) {
    if (*p + 1 > len) return 0;
    *v = b[(*p)++];
    return 1;
}

static int rd_u16(const unsigned char *b, int len, int *p, int *v) {
    if (*p + 2 > len) return 0;
    *v = b[*p] | (b[*p + 1] << 8);
    *p += 2;
    return 1;
}

static int rd_f64(const unsigned char *b, int len, int *p, double *v) {
    uint64_t bits = 0;
    if (*p + 8 > len) return 0;
    for (int i = 7; i >= 0; i--) bits = (bits << 8) | b[*p + i];
    memcpy(v, &bits, 8);
    *p += 8;
    return 1;
}

static void wr_u16(unsigned char *b, int v) {
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
}

static void wr_f64(unsigned char *b, double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)((bits >> (8 * i)) & 0xff);
}

// ---------- the time curve, without libm ------------------------------------

// 2^k, exact for every k this format can ask for (the stored exponent spans
// 2^-64 .. 2^191). Built from the IEEE bits rather than multiplied up, so it
// carries no rounding of its own.
static double pow2i(int k) {
    uint64_t bits;
    double d;
    if (k < -1022) return 0.0;
    if (k > 1023) k = 1023;
    bits = (uint64_t)(k + 1023) << 52;
    memcpy(&d, &bits, 8);
    return d;
}

// The unit a stored scale exponent means, in seconds.
static double unit_for(int scale_exp) { return pow2i(scale_exp - TIME_BIAS); }

// ceil(log2(x)) for x > 0, read off the exponent field: exact, and equal to
// what a correctly-rounded log2 would give (a mantissa of exactly 1 is the only
// case that does not round up).
static int ceil_log2(double x) {
    uint64_t bits;
    int e;
    memcpy(&bits, &x, 8);
    e = (int)((bits >> 52) & 0x7ff) - 1023;
    if (e == -1023) return -1074;              // zero or subnormal: below every unit
    return (bits & 0xfffffffffffffULL) ? e + 1 : e;
}

// The smallest exponent whose curve still reaches the game's largest gap, so a
// bot blitz quantizes at microseconds and a correspondence game at ~50ms for
// the same one byte per move. TIME_RANGE is the curve's reach in units,
// B^255 - 1; it is walked rather than written as a literal so this file has
// exactly one definition of B.
static int pick_scale_exp(double max_gap) {
    volatile double range = 1.0;
    int e;
    if (!(max_gap > 0)) return TIME_BIAS;      // all-zero gaps: unit = 1 s
    for (int i = 0; i < 255; i++) range *= TIME_B;
    e = ceil_log2(max_gap / (range - 1.0)) + TIME_BIAS;
    if (e < 0) e = 0;
    if (e > 255) e = 255;
    return e;
}

// The bucket `seconds` falls in: the v with B^(v-0.5) <= 1 + s/unit < B^(v+0.5),
// which is what rounding log_B means. Walked from the bottom, so the same
// products decide every build's answer.
static int quantize_gap(double seconds, double unit) {
    volatile double edge;
    double x;
    int v = 0;
    if (!(seconds > 0)) return 0;
    x = 1.0 + seconds / unit;
    edge = __builtin_sqrt(TIME_B);             // B^0.5, the first bucket edge
    while (v < 255 && x >= edge) { edge *= TIME_B; v++; }
    return v;
}

// The seconds a stored byte means. The same walk as quantize_gap's, so the two
// are inverse to each other by construction rather than by agreement.
static double dequantize_gap(int v, double unit) {
    volatile double p = 1.0;
    for (int i = 0; i < v; i++) p *= TIME_B;
    return unit * (p - 1.0);
}

// ---------- names ------------------------------------------------------------

// One name's wire bytes: NUL stripped, then cut to REPLAY_EXTRAS_MAX_NAME on a
// code-point boundary. Returns the length written into `out` (>= 48 capacity).
static int name_bytes(const unsigned char *src, int n, unsigned char *out) {
    int w = 0, k;
    for (int i = 0; i < n; i++) {
        if (src[i] == 0) continue;             // the terminator, never escaped
        if (w < REPLAY_EXTRAS_MAX_NAME + 4) out[w++] = src[i];
        else break;                            // nothing past here can survive the trim
    }
    if (w <= REPLAY_EXTRAS_MAX_NAME) return w;
    k = REPLAY_EXTRAS_MAX_NAME;
    while (k > 0 && (out[k] & 0xc0) == 0x80) k--;   // back off a continuation byte
    return k;
}

// ---------- encode -----------------------------------------------------------

int replay_extras_encode(const unsigned char *in, int in_len,
                         unsigned char *out, int cap) {
    int p = 0, flags = 0, w = 0;
    int in_flags = 0, n_names = 0, n_gaps = 0;
    int names_at = 0, gaps_at = 0;
    double start_time = 0;

    if (!in || !out || cap < 2) return -REPLAY_EXTRAS_ECAP;
    if (!rd_u8(in, in_len, &p, &in_flags)) return -REPLAY_EXTRAS_EINPUT;

    if (in_flags & REPLAY_EXTRAS_FLAG_NAMES) {
        if (!rd_u8(in, in_len, &p, &n_names)) return -REPLAY_EXTRAS_EINPUT;
        names_at = p;
        for (int i = 0; i < n_names; i++) {
            int len;
            if (!rd_u16(in, in_len, &p, &len)) return -REPLAY_EXTRAS_EINPUT;
            if (p + len > in_len) return -REPLAY_EXTRAS_EINPUT;
            p += len;
        }
    }
    if (in_flags & REPLAY_EXTRAS_FLAG_TIMES) {
        if (!rd_f64(in, in_len, &p, &start_time)) return -REPLAY_EXTRAS_EINPUT;
        if (!rd_u16(in, in_len, &p, &n_gaps)) return -REPLAY_EXTRAS_EINPUT;
        gaps_at = p;
        if (p + 8 * n_gaps > in_len) return -REPLAY_EXTRAS_EINPUT;
    }

    out[w++] = REPLAY_EXTRAS_VERSION;
    out[w++] = 0;                              // flags, back-filled below

    // A roster of zero seats says nothing, and the flags byte exists precisely
    // so a producer can answer one question and stay quiet about the other.
    if (n_names > 0) {
        int q = names_at;
        flags |= REPLAY_EXTRAS_FLAG_NAMES;
        for (int i = 0; i < n_names; i++) {
            unsigned char nb[REPLAY_EXTRAS_MAX_NAME + 4];
            int len = in[q] | (in[q + 1] << 8);
            int nw;
            q += 2;
            nw = name_bytes(in + q, len, nb);
            q += len;
            if (w + nw + 1 > cap) return -REPLAY_EXTRAS_ECAP;
            for (int j = 0; j < nw; j++) out[w++] = nb[j];
            out[w++] = 0;
        }
    }

    if (in_flags & REPLAY_EXTRAS_FLAG_TIMES) {
        double max_gap = 0, unit;
        int scale_exp;
        uint64_t start;
        flags |= REPLAY_EXTRAS_FLAG_TIMES;
        for (int i = 0; i < n_gaps; i++) {
            double g;
            int q = gaps_at + 8 * i;
            rd_f64(in, in_len, &q, &g);
            if (g > max_gap) max_gap = g;
        }
        scale_exp = pick_scale_exp(max_gap);
        unit = unit_for(scale_exp);
        if (w + 6 + n_gaps > cap) return -REPLAY_EXTRAS_ECAP;
        out[w++] = (unsigned char)scale_exp;
        // Five bytes is the field, so a start time is taken modulo 2^40 either
        // way; clamping first keeps the cast defined for a nonsense argument.
        start = 0;
        if (start_time > 0) {
            double f = __builtin_floor(start_time);
            start = (f >= 1099511627776.0) ? 1099511627775ULL : (uint64_t)f;
        }
        for (int i = 4; i >= 0; i--) out[w++] = (unsigned char)((start >> (8 * i)) & 0xff);
        for (int i = 0; i < n_gaps; i++) {
            double g;
            int q = gaps_at + 8 * i;
            rd_f64(in, in_len, &q, &g);
            out[w++] = (unsigned char)quantize_gap(g, unit);
        }
    }

    out[1] = (unsigned char)flags;
    return w;
}

int replay_extras_roster_speaks(const unsigned char *in, int in_len) {
    int p = 0, in_flags = 0, n_names = 0;
    if (!in) return 0;
    if (!rd_u8(in, in_len, &p, &in_flags)) return 0;
    if (!(in_flags & REPLAY_EXTRAS_FLAG_NAMES)) return 0;
    if (!rd_u8(in, in_len, &p, &n_names)) return 0;
    for (int i = 0; i < n_names; i++) {
        int len;
        if (!rd_u16(in, in_len, &p, &len)) return 0;
        if (p + len > in_len) return 0;
        // A name of nothing but NULs is a name of nothing: the encoder strips
        // them, so it would reach the wire empty.
        for (int j = 0; j < len; j++) if (in[p + j] != 0) return 1;
        p += len;
    }
    return 0;
}

// ---------- decode -----------------------------------------------------------

int replay_extras_decode(const unsigned char *blob, int blob_len,
                         int player_count, int move_count,
                         unsigned char *out, int cap) {
    int p = 0, w = 0, flags, n_gaps = 0, n_names = 0;
    int flags_at, n_names_at;
    double unit = 1.0;
    uint64_t start = 0;

    if (!blob || !out || cap < 4) return -REPLAY_EXTRAS_ECAP;
    if (blob_len < 2) return -REPLAY_EXTRAS_EHEADER;
    if (blob[0] != REPLAY_EXTRAS_VERSION) return -REPLAY_EXTRAS_EVERSION;
    flags = blob[1];
    p = 2;

    flags_at = w++;                            // flags, back-filled
    n_names_at = w++;                          // n_names, back-filled
    out[flags_at] = 0;
    out[n_names_at] = 0;

    if (flags & REPLAY_EXTRAS_FLAG_NAMES) {
        if (player_count < 0 || player_count > 255) return -REPLAY_EXTRAS_EINPUT;
        out[flags_at] |= REPLAY_EXTRAS_FLAG_NAMES;
        for (int i = 0; i < player_count; i++) {
            int start_i = p;
            while (p < blob_len && blob[p] != 0) p++;
            if (p >= blob_len) return -REPLAY_EXTRAS_ENAME;
            if (w + 2 + (p - start_i) > cap) return -REPLAY_EXTRAS_ECAP;
            wr_u16(out + w, p - start_i);
            w += 2;
            for (int j = start_i; j < p; j++) out[w++] = blob[j];
            p++;                               // step over the NUL
            n_names++;
        }
        out[n_names_at] = (unsigned char)n_names;
    }

    if (flags & REPLAY_EXTRAS_FLAG_TIMES) {
        int gaps_at;
        if (p + 6 > blob_len) return -REPLAY_EXTRAS_ETIMES;
        out[flags_at] |= REPLAY_EXTRAS_FLAG_TIMES;
        unit = unit_for(blob[p++]);
        for (int i = 0; i < 5; i++) start = (start << 8) | blob[p++];
        if (w + 10 > cap) return -REPLAY_EXTRAS_ECAP;
        wr_f64(out + w, (double)start);
        w += 8;
        gaps_at = w;                           // n_gaps, back-filled
        w += 2;
        for (int i = 0; i < move_count && p < blob_len; i++) {
            if (w + 8 > cap) return -REPLAY_EXTRAS_ECAP;
            wr_f64(out + w, dequantize_gap(blob[p++], unit));
            w += 8;
            n_gaps++;
        }
        // base32 padding can leave a stray trailing byte, so a blob may come up
        // one gap short of nothing; a real shortfall means a corrupt blob.
        if (n_gaps < move_count && n_gaps > 0) return -REPLAY_EXTRAS_EGAPS;
        wr_u16(out + gaps_at, n_gaps);
    }

    return w;
}

// ---------- the whole shareable link ----------------------------------------

int replay_extras_link(const char *moves,
                       const unsigned char *names, int names_len, int n_names,
                       char *out, int cap) {
    static const char prefix[] = REPLAY_LINK_PREFIX;
    // 8 seats of arbitrary Unicode with room to spare; a roster past this is a
    // caller bug, not a nickname.
    unsigned char in[4096];
    unsigned char blob[8192];
    int w = 0, n, bare;

    if (!moves || !out || cap < 1) return -REPLAY_EXTRAS_EINPUT;
    if (n_names < 0 || n_names > 255) return -REPLAY_EXTRAS_EINPUT;
    if (names_len < 0 || (names_len > 0 && !names)) return -REPLAY_EXTRAS_EINPUT;
    if (names_len > (int)sizeof(in) - 2) return -REPLAY_EXTRAS_EINPUT;

    for (int i = 0; i < (int)sizeof(prefix) - 1; i++) {
        if (w >= cap - 1) return -REPLAY_EXTRAS_ECAP;
        out[w++] = prefix[i];
    }
    for (int i = 0; moves[i]; i++) {
        if (w >= cap - 1) return -REPLAY_EXTRAS_ECAP;
        out[w++] = moves[i];
    }
    out[w] = 0;
    bare = w;
    if (n_names == 0) return bare;

    in[0] = REPLAY_EXTRAS_FLAG_NAMES;
    in[1] = (unsigned char)n_names;
    for (int i = 0; i < names_len; i++) in[2 + i] = names[i];
    if (!replay_extras_roster_speaks(in, names_len + 2)) return bare;

    n = replay_extras_encode(in, names_len + 2, blob, sizeof(blob));
    if (n < 0) return bare;                    // decoration: a bad roster costs the names, never the link
    if (w >= cap - 2) return -REPLAY_EXTRAS_ECAP;
    out[w++] = '-';
    n = replay_b32_encode(blob, n, out + w, cap - w);
    if (n < 0) { out[bare] = 0; return bare; }
    return w + n;
}
