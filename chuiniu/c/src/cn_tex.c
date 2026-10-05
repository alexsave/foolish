/* The textures the cups and dice wear, derived from the baked pack (cn_tex.h).
 * Freestanding: memcpy / memset only. Every function here is a port of the
 * study's TEX3 (docs/UI.html): the same offsets from the same hash, the same
 * gradient stops, the same draw order, each draw rounded to 8 bits as a
 * canvas stores it. */
#include "cn_tex.h"
#include <string.h>

/* ---- small arithmetic ------------------------------------------------------------- */
static double dsqrt(double x) { return __builtin_sqrt(x); }   /* one instruction everywhere, correctly rounded */
static double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
/* a canvas draw stores 8 bits: the value rounded, half up, and clamped */
static uint8_t q8(double v) { if (v <= 0) return 0; if (v >= 255) return 255; return (uint8_t)(int)(v + .5); }
/* source-over of a flat colour at coverage-times-alpha A on one opaque texel */
static void over(uint8_t *t, double r, double g, double b, double A)
{
    if (A <= 0) return;
    t[0] = q8(t[0] + (r - t[0]) * A); t[1] = q8(t[1] + (g - t[1]) * A); t[2] = q8(t[2] + (b - t[2]) * A);
}
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

uint32_t cn_tex_fnv1a(const uint8_t *b, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* the study's TEX.hash on UInt32: Math.imul is the low 32 bits of the product, >>> a logical shift */
uint32_t cn_tex_hash(uint32_t ix, uint32_t iy, uint32_t seed)
{
    uint32_t h = ix * 0x8da6b343u ^ iy * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h = (h ^ (h >> 15)) * 0x2c1b3c6du;
    h = (h ^ (h >> 12)) * 0x297a2d39u;
    return h ^ (h >> 15);
}
static double H(uint32_t ix, uint32_t iy, uint32_t seed) { return cn_tex_hash(ix, iy, seed) / 4294967296.0; }

/* ---- the pack ------------------------------------------------------------------------ */
int cn_tex_pack_open(CnTexPack *p, const uint8_t *b, uint32_t len)
{
    memset(p, 0, sizeof *p);
    if (!b || len < CN_TEX_HEAD_BYTES) return CN_TEX_E_SHORT;
    if (rd32(b) != CN_TEX_MAGIC) return CN_TEX_E_MAGIC;
    if (rd16(b + 4) != CN_TEX_VERSION) return CN_TEX_E_VERSION;
    uint32_t n = rd16(b + 6), total = rd32(b + 8);
    if (total != len || (uint64_t)CN_TEX_HEAD_BYTES + (uint64_t)n * CN_TEX_ENTRY_BYTES > len) return CN_TEX_E_SHORT;
    if (rd32(b + 12) != cn_tex_fnv1a(b + CN_TEX_HEAD_BYTES, len - CN_TEX_HEAD_BYTES)) return CN_TEX_E_CHECK;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = b + CN_TEX_HEAD_BYTES + i * CN_TEX_ENTRY_BYTES;
        uint32_t kind = e[0], bpt = e[1], digit = e[2], px = rd16(e + 4), w = rd16(e + 6), h = rd16(e + 8);
        int ox = (int16_t)rd16(e + 10), oy = (int16_t)rd16(e + 12);
        uint32_t off = rd32(e + 16);
        uint64_t end = (uint64_t)off + (uint64_t)w * h * bpt;
        if (off & 3 || end > len || off < CN_TEX_HEAD_BYTES + n * CN_TEX_ENTRY_BYTES) return CN_TEX_E_ENTRY;
        if (kind == CN_TEXK_VERD) {
            if (bpt != 3 || w != CN_TEX_VERD || h != CN_TEX_VERD) return CN_TEX_E_ENTRY;
            p->verd = b + off;
        } else if (kind == CN_TEXK_BONE) {
            if (bpt != 3 || w != CN_TEX_BONE || h != CN_TEX_BONE) return CN_TEX_E_ENTRY;
            p->bone = b + off;
        } else if (kind == CN_TEXK_GLYPH) {
            int s = px == CN_TEX_NUMERAL_SMALL ? 0 : px == CN_TEX_NUMERAL_LARGE ? 1 : -1;
            if (bpt != 1 || s < 0 || digit >= CN_TEX_DIGITS || w == 0 || h == 0) return CN_TEX_E_ENTRY;
            CnTexGlyph *g = &p->glyph[s][digit];
            g->px = b + off; g->w = (int)w; g->h = (int)h; g->ox = ox; g->oy = oy;
        } else return CN_TEX_E_ENTRY;
    }
    if (!p->verd || !p->bone) return CN_TEX_E_MISSING;
    for (int s = 0; s < CN_TEX_GLYPH_SIZES; s++)
        for (int d = 0; d < CN_TEX_DIGITS; d++) if (!p->glyph[s][d].px) return CN_TEX_E_MISSING;
    return CN_TEX_OK;
}

/* ---- the verdigris at an offset ---------------------------------------------------- */
/* TEX3's `drawImage(src, i * 256 - x, j * 256 - y)` over the whole canvas: texel (X, Y) is the tile's (X + x, Y + y), wrapped */
static void verd_at(const CnTexPack *p, int w, int h, int x, int y, uint8_t *rgba)
{
    for (int Y = 0; Y < h; Y++) {
        const uint8_t *row = p->verd + ((Y + y) & (CN_TEX_VERD - 1)) * CN_TEX_VERD * 3;
        uint8_t *o = rgba + (size_t)Y * w * 4;
        for (int X = 0; X < w; X++) {
            const uint8_t *s = row + ((X + x) & (CN_TEX_VERD - 1)) * 3;
            o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; o += 4;
        }
    }
}
/* hash * 256 | 0 is the top 8 bits; * 128 | 0 the top 7 */
static int off256(uint32_t seed, uint32_t k) { return (int)(cn_tex_hash(seed, k, 977) >> 24); }
static int off128(uint32_t seed, uint32_t k) { return (int)(cn_tex_hash(seed, k, 977) >> 25); }

/* a vertical gradient of a flat colour, alpha by stops, sampled at each row's centre */
static double stops_at(const double *pos, const double *val, int n, double t)
{
    if (t <= pos[0]) return val[0];
    for (int i = 1; i < n; i++) if (t <= pos[i]) return val[i - 1] + (val[i] - val[i - 1]) * (t - pos[i - 1]) / (pos[i] - pos[i - 1]);
    return val[n - 1];
}
static void vgrad(uint8_t *rgba, int w, int h, double r, double g, double b, const double *pos, const double *alpha, int n)
{
    for (int y = 0; y < h; y++) {
        double A = stops_at(pos, alpha, n, (y + .5) / h);
        if (A <= 0) continue;
        for (int x = 0; x < w; x++) over(rgba + ((size_t)y * w + x) * 4, r, g, b, A);
    }
}

void cn_tex_cup_side(const CnTexPack *p, uint32_t seed, uint8_t *rgba)
{
    verd_at(p, CN_TEX_SIDE_W, CN_TEX_SIDE_H, off256(seed, 91), off256(seed, 92), rgba);
    /* no light baked in; only a little grime toward the mouth (stops 0: 0, .9: 0, 1: .3 black) */
    static const double pos[3] = { 0, .9, 1 }, al[3] = { 0, 0, .3 };
    vgrad(rgba, CN_TEX_SIDE_W, CN_TEX_SIDE_H, 0, 0, 0, pos, al, 3);
}

void cn_tex_cup_inner(const CnTexPack *p, uint32_t seed, uint8_t *rgba)
{
    verd_at(p, CN_TEX_SIDE_W, CN_TEX_SIDE_H, off256(seed, 95), off256(seed, 96), rgba);
    static const double pos[3] = { 0, .7, 1 }, al[3] = { .72, .38, .12 };
    vgrad(rgba, CN_TEX_SIDE_W, CN_TEX_SIDE_H, 0, 4, 3, pos, al, 3);
}

void cn_tex_cup_floor(const CnTexPack *p, uint32_t seed, uint8_t *rgba)
{
    const int S = CN_TEX_CROWN;
    verd_at(p, S, S, off128(seed, 97), off128(seed, 98), rgba);
    /* a radial gradient about the centre, from 20 to 130 texels: .32 to .72 of rgb(0, 4, 3) */
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        double dx = x + .5 - 128, dy = y + .5 - 128, t = clamp01((dsqrt(dx * dx + dy * dy) - 20) / 110);
        over(rgba + ((size_t)y * S + x) * 4, 0, 4, 3, .32 + (.72 - .32) * t);
    }
}

void cn_tex_tint(uint8_t *rgba, int w, int h, int r, int g, int b, int a255)
{
    double A = a255 / 255.0;
    for (int i = 0; i < w * h; i++) over(rgba + (size_t)i * 4, r, g, b, A);
}

static const CnTexGlyph *glyph_of(const CnTexPack *p, int digit, int numeral_px)
{
    int s = numeral_px == CN_TEX_NUMERAL_SMALL ? 0 : numeral_px == CN_TEX_NUMERAL_LARGE ? 1 : -1;
    if (s < 0 || digit < 0 || digit >= CN_TEX_DIGITS || !p->glyph[s][digit].px) return 0;
    return &p->glyph[s][digit];
}
static void blit(uint8_t *rgba, int w, int h, const CnTexGlyph *g, int ax, int ay, double r, double gg, double b, double alpha)
{
    for (int y = 0; y < g->h; y++) {
        int Y = ay + g->oy + y; if (Y < 0 || Y >= h) continue;
        for (int x = 0; x < g->w; x++) {
            int X = ax + g->ox + x, c = g->px[y * g->w + x]; if (X < 0 || X >= w || !c) continue;
            over(rgba + ((size_t)Y * w + X) * 4, r, gg, b, alpha * c / 255.0);
        }
    }
}
int cn_tex_stamp_numeral(const CnTexPack *p, uint8_t *rgba, int w, int h, int digit, int numeral_px)
{
    const CnTexGlyph *g = glyph_of(p, digit, numeral_px);
    if (!g) return 0;
    /* the study: centred, middle baseline; a shadow at (130, 140), the face at (128, 136) */
    blit(rgba, w, h, g, w / 2 + 2, h / 2 + 12, 10, 13, 11, .85);
    blit(rgba, w, h, g, w / 2, h / 2 + 8, 232, 236, 216, .92);
    return 1;
}

void cn_tex_cup_crown(const CnTexPack *p, uint32_t seed, int count, int out, int numeral_px, uint8_t *rgba)
{
    verd_at(p, CN_TEX_CROWN, CN_TEX_CROWN, off128(seed, 93), off128(seed, 94), rgba);
    if (out) cn_tex_tint(rgba, CN_TEX_CROWN, CN_TEX_CROWN, 4, 14, 15, 153);   /* rgba(4,14,15,.6) */
    else if (count >= 0) cn_tex_stamp_numeral(p, rgba, CN_TEX_CROWN, CN_TEX_CROWN, count, numeral_px);
}

/* ---- the die ---------------------------------------------------------------------------- */
/* SHAPE.pips: the spots in quarters of the face (1 is .25, 2 the middle, 3 is .75) */
static const uint8_t PIPS[7][6][2] = {
    { { 0 } },
    { { 2, 2 } },
    { { 3, 1 }, { 1, 3 } },
    { { 3, 1 }, { 2, 2 }, { 1, 3 } },
    { { 1, 1 }, { 3, 1 }, { 1, 3 }, { 3, 3 } },
    { { 1, 1 }, { 3, 1 }, { 2, 2 }, { 1, 3 }, { 3, 3 } },
    { { 1, 1 }, { 3, 1 }, { 1, 2 }, { 3, 2 }, { 1, 3 }, { 3, 3 } },
};
typedef struct { double x, y, r; } Pip;
/* the face's pips with their jitter, centres in atlas texels; returns how many */
static int pips_of(uint32_t seed, int f, Pip *out)
{
    const double S = CN_TEX_DIE_CELL, dia = S * (f == 1 ? .26 : .21), jit = S * .012;
    for (int i = 0; i < f; i++) {
        uint32_t qx = PIPS[f][i][0], qy = PIPS[f][i][1];
        double px = qx * .25, py = qy * .25;
        double jx = (H(qx * 25, qy * 25, seed + f) - .5) * 2 * jit, jy = (H(qy * 25, qx * 25, seed + f + 1) - .5) * 2 * jit;
        out[i].x = (f - 1) * S + px * S + jx; out[i].y = py * S + jy; out[i].r = dia / 2;
    }
    return f;
}
/* piecewise-linear RGB stops */
static void rgb_at(const double *pos, const uint8_t (*col)[3], int n, double t, double *c)
{
    for (int k = 0; k < 3; k++) {
        double v[4]; for (int i = 0; i < n; i++) v[i] = col[i][k];
        c[k] = stops_at(pos, v, n, t);
    }
}
static double seg_dist(double px, double py, double ax, double ay, double bx, double by)
{
    double dx = bx - ax, dy = by - ay, t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    t = clamp01(t);
    double ex = px - (ax + t * dx), ey = py - (ay + t * dy);
    return dsqrt(ex * ex + ey * ey);
}

void cn_tex_die_atlas(const CnTexPack *p, uint32_t seed, uint8_t *rgba)
{
    const int S = CN_TEX_DIE_CELL, W = CN_TEX_DIE_W;
    for (int f = 1; f <= 6; f++) {
        const int x0 = (f - 1) * S;
        const int ox = (int)(cn_tex_hash(seed, 30 + f, 977) >> 25), oy = (int)(cn_tex_hash(seed, 40 + f, 977) >> 25);
        /* the bone at the face's own offset */
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
            const uint8_t *s = p->bone + (((y + oy) & (S - 1)) * S + ((x + ox) & (S - 1))) * 3;
            uint8_t *o = rgba + ((size_t)y * W + x0 + x) * 4;
            o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255;
        }
        /* strike wear ('light'): each corner a radial stain, each its own depth */
        for (uint32_t cn = 0; cn < 4; cn++) {
            double cx = x0 + (cn & 1 ? S : 0), cy = cn & 2 ? S : 0;
            double rr = S * (.16 + .18 * H(cn, seed + f, 911)), op = .2 + .24 * H(cn + 4, seed + f, 911);
            const double pos[3] = { 0, .45, 1 }, al[3] = { op, op * .55, 0 };
            for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
                double dx = x0 + x + .5 - cx, dy = y + .5 - cy, t = dsqrt(dx * dx + dy * dy) / rr;
                if (t < 1) over(rgba + ((size_t)y * W + x0 + x) * 4, 78, 63, 38, stops_at(pos, al, 3, t));
            }
        }
        /* ...and each edge a round-capped stroke along it, S * .11 wide, half of it off the face */
        const double E[4][4] = { { x0 + S * .16, 0, x0 + S * .84, 0 }, { x0 + S, S * .16, x0 + S, S * .84 },
                                 { x0 + S * .16, S, x0 + S * .84, S }, { x0, S * .16, x0, S * .84 } };
        for (uint32_t e = 0; e < 4; e++) {
            double op = .08 + .18 * H(e + 8, seed + f, 911), half = S * .11 / 2;
            for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
                double cov = clamp01(half - seg_dist(x0 + x + .5, y + .5, E[e][0], E[e][1], E[e][2], E[e][3]) + .5);
                if (cov > 0) over(rgba + ((size_t)y * W + x0 + x) * 4, 78, 63, 38, op * cov);
            }
        }
        /* the pips: drilled, the far wall lit (a radial gradient from a focus up and left of the centre); the 1 in blood */
        static const double P1[3] = { 0, .55, 1 }, PN[3] = { 0, .62, 1 };
        static const uint8_t C1[3][3] = { { 58, 9, 6 }, { 127, 31, 23 }, { 178, 80, 63 } };
        static const uint8_t CN[3][3] = { { 0, 4, 6 }, { 10, 18, 16 }, { 54, 70, 61 } };
        Pip pp[6]; int n = pips_of(seed, f, pp);
        for (int i = 0; i < n; i++) {
            const double r = pp[i].r, fx = pp[i].x + r * .2, fy = pp[i].y + r * .25, dvx = pp[i].x - fx, dvy = pp[i].y - fy;
            const double a = dvx * dvx + dvy * dvy - r * r, lw = r * .1 > .8 ? r * .1 : .8;
            int ya = (int)(pp[i].y - r - 2), yb = (int)(pp[i].y + r + 2), xa = (int)(pp[i].x - r - 2), xb = (int)(pp[i].x + r + 2);
            if (ya < 0) ya = 0;
            if (yb > S - 1) yb = S - 1;
            if (xa < x0) xa = x0;
            if (xb > x0 + S - 1) xb = x0 + S - 1;
            for (int y = ya; y <= yb; y++) for (int x = xa; x <= xb; x++) {
                double px = x + .5, py = y + .5, dx = px - pp[i].x, dy = py - pp[i].y, d = dsqrt(dx * dx + dy * dy);
                double cov = clamp01(r - d + .5);
                if (cov <= 0) continue;
                /* the two-circle gradient's t: the larger root of |q - t dv| = t r, q from the focus */
                double qx = px - fx, qy = py - fy, b = -2 * (qx * dvx + qy * dvy), c = qx * qx + qy * qy;
                double disc = b * b - 4 * a * c, t = clamp01((-b - dsqrt(disc > 0 ? disc : 0)) / (2 * a)), col[3];
                rgb_at(f == 1 ? P1 : PN, f == 1 ? C1 : CN, 3, t, col);
                over(rgba + ((size_t)y * W + x) * 4, col[0], col[1], col[2], cov);
            }
            for (int y = ya; y <= yb; y++) for (int x = xa; x <= xb; x++) {
                double dx = x + .5 - pp[i].x, dy = y + .5 - pp[i].y, d = dsqrt(dx * dx + dy * dy);
                double ad = d > r ? d - r : r - d, cov = clamp01(lw / 2 - ad + .5);
                if (cov > 0) over(rgba + ((size_t)y * W + x) * 4, 0, 0, 0, .45 * cov);
            }
        }
    }
}

/* ---- relief ------------------------------------------------------------------------------ */
/* the slope of a height map as the study's normalMap: -(right - left) / 255 * strength, at a 20th, rounded, clamped */
static int8_t slope(int d, int strength20)
{
    int num = -d * strength20, m = num < 0 ? -num : num, v = (2 * m + 255) / 510;   /* no ties: 255 is odd and S20 has no factor of it */
    if (v > 127) v = 127;
    return (int8_t)(num < 0 ? -v : v);
}
/* the height the study draws: mid-grey with the red over it at half alpha */
static int relief_h(const uint8_t *rgba, int w, int x, int y) { return (128 + rgba[((size_t)y * w + x) * 4] + 1) >> 1; }

void cn_tex_relief(const uint8_t *rgba, int w, int h, int strength20, int8_t *bump)
{
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int l = relief_h(rgba, w, x > 0 ? x - 1 : 0, y), r = relief_h(rgba, w, x < w - 1 ? x + 1 : w - 1, y);
        int u = relief_h(rgba, w, x, y > 0 ? y - 1 : 0), dn = relief_h(rgba, w, x, y < h - 1 ? y + 1 : h - 1);
        bump[((size_t)y * w + x) * 2] = slope(r - l, strength20);
        bump[((size_t)y * w + x) * 2 + 1] = slope(dn - u, strength20);
    }
}

/* the die's height at a texel: mid-grey, the grain at .12, and each pip a hole with a soft rim (radius 1.12 of the pip) */
static int die_h(const uint8_t *atlas, const Pip (*pips)[6], int x, int y)
{
    const int f = x / CN_TEX_DIE_CELL + 1;
    double h = q8(128 + (atlas[((size_t)y * CN_TEX_DIE_W + x) * 4] - 128) * .12);
    static const double pos[5] = { 0, .55, .9, .97, 1 }, grey[5] = { 48, 52, 106, 138, 128 };
    for (int i = 0; i < f; i++) {
        const Pip *p = &pips[f - 1][i];
        double dx = x + .5 - p->x, dy = y + .5 - p->y, d = dsqrt(dx * dx + dy * dy), R = p->r * 1.12, cov = clamp01(R - d + .5);
        if (cov > 0) h = q8(h + (stops_at(pos, grey, 5, d / R) - h) * cov);
    }
    return (int)h;
}
void cn_tex_die_relief(const uint8_t *atlas, uint32_t seed, int8_t *bump)
{
    const int W = CN_TEX_DIE_W, Hh = CN_TEX_DIE_CELL;
    Pip pips[6][6];
    for (int f = 1; f <= 6; f++) pips_of(seed, f, pips[f - 1]);
    for (int y = 0; y < Hh; y++) for (int x = 0; x < W; x++) {
        int l = die_h(atlas, pips, x > 0 ? x - 1 : 0, y), r = die_h(atlas, pips, x < W - 1 ? x + 1 : W - 1, y);
        int u = die_h(atlas, pips, x, y > 0 ? y - 1 : 0), dn = die_h(atlas, pips, x, y < Hh - 1 ? y + 1 : Hh - 1);
        bump[((size_t)y * W + x) * 2] = slope(r - l, CN_TEX_RELIEF_DIE);
        bump[((size_t)y * W + x) * 2 + 1] = slope(dn - u, CN_TEX_RELIEF_DIE);
    }
}

void cn_tex_mip(const uint8_t *pr, const int8_t *pb, int pw, int ph, uint8_t *rg, int8_t *bm)
{
    const int w = pw / 2, h = ph / 2;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int i00 = ((2 * y) * pw + 2 * x), i10 = i00 + 1, i01 = i00 + pw, i11 = i01 + 1, o = y * w + x;
        for (int c = 0; c < 4; c++) rg[o * 4 + c] = (uint8_t)((pr[i00 * 4 + c] + pr[i10 * 4 + c] + pr[i01 * 4 + c] + pr[i11 * 4 + c] + 2) >> 2);
        if (pb && bm) for (int c = 0; c < 2; c++) bm[o * 2 + c] = (int8_t)((pb[i00 * 2 + c] + pb[i10 * 2 + c] + pb[i01 * 2 + c] + pb[i11 * 2 + c]) / 4);
    }
}

/* ---- upload ---------------------------------------------------------------------------- */
void cn_tex_size(CnTexWhat what, int *w, int *h)
{
    switch (what) {
    case CN_TEX_SIDE: case CN_TEX_INNER: *w = CN_TEX_SIDE_W; *h = CN_TEX_SIDE_H; return;
    case CN_TEX_FLOOR: case CN_TEX_CROWN_T: *w = *h = CN_TEX_CROWN; return;
    case CN_TEX_DIE: *w = CN_TEX_DIE_W; *h = CN_TEX_DIE_CELL; return;
    }
    *w = *h = 0;
}

int cn_tex_upload(const CnTexPack *p, const CnTexSink *sink, const CnTexSpec *s)
{
    int w = 0, h = 0;
    cn_tex_size(s->what, &w, &h);
    if (!w || !p->verd || !sink || !sink->alloc) return -1;
    CnTexImage img = { 0, 0, 0, 0 };
    int id = sink->alloc(sink->ctx, w, h, s->bump ? 1 : 0, &img);
    if (id < 0 || !img.rgba || img.w != w || img.h != h || (s->bump && !img.bump)) return -1;
    int strength = CN_TEX_RELIEF_CUP;
    switch (s->what) {
    case CN_TEX_SIDE: cn_tex_cup_side(p, s->seed, img.rgba); strength = CN_TEX_RELIEF_SIDE; break;
    case CN_TEX_INNER: cn_tex_cup_inner(p, s->seed, img.rgba); break;
    case CN_TEX_FLOOR: cn_tex_cup_floor(p, s->seed, img.rgba); break;
    case CN_TEX_CROWN_T: cn_tex_cup_crown(p, s->seed, s->count, s->out, s->numeral_px, img.rgba); break;
    case CN_TEX_DIE:
        cn_tex_die_atlas(p, s->seed, img.rgba);
        if (s->bump) cn_tex_die_relief(img.rgba, s->seed, img.bump);
        return id;
    }
    if (s->bump) cn_tex_relief(img.rgba, w, h, strength, img.bump);
    return id;
}
