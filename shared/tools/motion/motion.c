/* motion.c - the ruler finder and the ride scorer (motion.h). */
#include "motion.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- marks ------------------------------------------------------------ */

static const char *const QUAD[MT_QUADS] = {"", "_tl", "_tr", "_bl", "_br"};
static const char *const INK_NAME[MR_INK_COUNT] = {
    "red", "orange", "yellow", "lime", "green", "cyan", "blue", "violet", "magenta", "pink",
};

const char *mt_mark_name(int32_t mark, char *buf, int32_t n) {
    int32_t i = mark / MT_QUADS, q = mark % MT_QUADS;
    snprintf(buf, (size_t)n, "%s%s", INK_NAME[mr_square_ink(i)], QUAD[q]);
    return buf;
}

int32_t mt_mark_by_name(const char *name) {
    char b[32];
    for (int32_t m = 0; m < MT_MARKS; m++)
        if (!strcmp(mt_mark_name(m, b, sizeof b), name)) return m;
    return -1;
}

/* THE INK OF A PIXEL, or -1: exactly the HSV of the reader this replaced
 * (numpy in float32 - the hue is the channel that is largest, blue winning a
 * tie over green over red), then the nearest ink by hue within MR_HUE_TOL
 * above both floors. Inks sit 30 degrees apart and the tolerance is 13, so a
 * pixel is at most one ink. */
static int32_t ink_of(uint8_t R, uint8_t G, uint8_t B) {
    float r = R / 255.0f, g = G / 255.0f, b = B / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    if (mx <= (float)MR_VAL_MIN) return -1;
    float s = mx == 0 ? 0 : d / mx;
    if (!(s > (float)MR_SAT_MIN)) return -1;
    float dd = d == 0 ? 1 : d, h = 0;
    if (mx == r) { h = fmodf((g - b) / dd, 6.0f); if (h < 0) h += 6.0f; }
    if (mx == g) h = (b - r) / dd + 2;
    if (mx == b) h = (r - g) / dd + 4;
    h *= 60;
    for (int32_t k = 0; k < MR_INK_COUNT; k++) {
        float x = fmodf(h - (float)mr_ink_hue(k) + 180.0f, 360.0f);
        if (x < 0) x += 360.0f;
        if (fabsf(x - 180.0f) < (float)MR_HUE_TOL) return k;
    }
    return -1;
}

static double round_even(double x) { return nearbyint(x); }

/* The clock under the red bar, on the full-resolution frame. */
static int32_t read_clock(const uint8_t *rgb, int32_t w, int32_t h, double red_pt, double scale) {
    if (red_pt == MT_NONE) return -1;
    int32_t y = (int32_t)round_even((red_pt + MR_EDGE_PT / 2 + MR_CLOCK_CELL_PT / 2) * scale);
    if (y < 0 || y >= h) return -1;
    const uint8_t *row = rgb + (size_t)y * (size_t)w * 3;
    int32_t v = 0;
    for (int32_t i = 0; i < MR_CLOCK_BITS; i++) {
        int32_t x = (int32_t)round_even((24.0 + MR_CLOCK_CELL_PT * (i + 0.5)) * scale);
        double lo = 1e9, hi = -1e9;
        for (int32_t k = x - 3; k < x + 4; k++) {
            if (k < 0 || k >= w) return -1;
            double l = (row[3 * k] + row[3 * k + 1] + row[3 * k + 2]) / 3.0;
            if (l < lo) lo = l;
            if (l > hi) hi = l;
        }
        if (lo > MR_CLOCK_LIGHT) v = v * 2 + 1;
        else if (hi < MR_CLOCK_DARK) v = v * 2;
        else return -1;
    }
    return v;
}

static int32_t find_root(int32_t *p, int32_t a) {
    while (p[a] != a) { p[a] = p[p[a]]; a = p[a]; }
    return a;
}

typedef struct { int32_t first, area, x0, y0, x1, y1; } Blob;

void mt_find(const uint8_t *rgb, int32_t W, int32_t H, double scale, MtRow *row) {
    /* HALF RESOLUTION for the colour work (every other pixel of every other
     * row, not an average: an average would mix a square's edge with paper). */
    int32_t w = (W + 1) / 2, h = (H + 1) / 2;
    double s = scale / 2;
    int32_t skip = (int32_t)(MR_SQUARE_SKIP_PT * scale) / 2;
    int8_t *ink = malloc((size_t)w * (size_t)h);
    int32_t *lab = malloc(sizeof(int32_t) * (size_t)w * (size_t)h);
    int32_t cap = 1 << 16, n = 0;
    int32_t *par = malloc(sizeof(int32_t) * (size_t)cap);
    double rsum[2] = {0, 0};
    int32_t rcnt[2] = {0, 0};
    for (int32_t y = 0; y < h; y++) {
        int32_t cover[2] = {0, 0};
        const uint8_t *src = rgb + (size_t)(2 * y) * (size_t)W * 3;
        for (int32_t x = 0; x < w; x++) {
            const uint8_t *p = src + 6 * (size_t)x;
            int32_t k = ink_of(p[0], p[1], p[2]);
            if (k == MR_INK_RED) cover[0]++;
            if (k == MR_INK_GREEN) cover[1]++;
            /* the banded strip is not a square */
            ink[(size_t)y * w + x] = (int8_t)(x < skip ? -1 : k);
        }
        for (int32_t b = 0; b < 2; b++)
            if (cover[b] > MR_BAR_COVER * w) { rsum[b] += y; rcnt[b]++; }
    }
    row->red = rcnt[0] ? rsum[0] / rcnt[0] / s : MT_NONE;
    row->green = rcnt[1] ? rsum[1] / rcnt[1] / s : MT_NONE;
    row->clock = read_clock(rgb, W, H, row->red, scale);

    /* ONE LABELLING of the whole ink map, 4-connected, a blob one ink. A
     * blob's first label is its first pixel in raster order, so the blobs of
     * an ink come out in the order the reader this replaced numbered them. */
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            int32_t k = ink[i];
            lab[i] = -1;
            if (k < 0) continue;
            int32_t up = y > 0 && ink[i - w] == k ? lab[i - w] : -1;
            int32_t left = x > 0 && ink[i - 1] == k ? lab[i - 1] : -1;
            if (up < 0 && left < 0) {
                if (n == cap) { cap *= 2; par = realloc(par, sizeof(int32_t) * (size_t)cap); }
                par[n] = n;
                lab[i] = n++;
            } else if (up >= 0 && left >= 0) {
                int32_t a = find_root(par, up), b = find_root(par, left);
                if (a < b) par[b] = a; else par[a] = b;
                lab[i] = a < b ? a : b;
            } else {
                lab[i] = up >= 0 ? up : left;
            }
        }
    Blob *bl = calloc((size_t)(n ? n : 1), sizeof(Blob));
    for (int32_t k = 0; k < n; k++) { bl[k].first = -1; bl[k].x0 = bl[k].y0 = 1 << 30; bl[k].x1 = bl[k].y1 = -1; }
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            if (lab[i] < 0) continue;
            int32_t r = find_root(par, lab[i]);
            Blob *b = &bl[r];
            if (b->first < 0) b->first = ink[i];
            b->area++;
            if (x < b->x0) b->x0 = x;
            if (x > b->x1) b->x1 = x;
            if (y < b->y0) b->y0 = y;
            if (y > b->y1) b->y1 = y;
        }

    /* Every square found, per ink, in blob order. */
    enum { PER = 16 };
    double px[MR_INK_COUNT][PER], py[MR_INK_COUNT][PER];
    int32_t np[MR_INK_COUNT] = {0};
    double side = MR_SIDE_PT * s, lo = MR_AREA_MIN * side * side, hi = MR_AREA_MAX * side * side;
    for (int32_t r = 0; r < n; r++) {
        Blob *b = &bl[r];
        if (b->area == 0 || b->first == MR_INK_RED || b->first == MR_INK_GREEN) continue;
        if (b->area < lo || b->area > hi) continue;
        int32_t bh = b->y1 - b->y0 + 1, bw = b->x1 - b->x0 + 1;
        int32_t big = bh > bw ? bh : bw, small = bh < bw ? bh : bw;
        if (big > MR_BOX_MAX * side || small < MR_BOX_MIN * side) continue;
        /* the centre of mass of the ink in the box (any blob of it there) */
        double sx = 0, sy = 0, m = 0;
        for (int32_t y = b->y0; y <= b->y1; y++)
            for (int32_t x = b->x0; x <= b->x1; x++)
                if (ink[(size_t)y * w + x] == b->first) { sx += x; sy += y; m++; }
        int32_t k = b->first;
        if (np[k] < PER) { px[k][np[k]] = sx / m / s; py[k][np[k]] = sy / m / s; np[k]++; }
    }
    for (int32_t m = 0; m < MT_MARKS; m++) row->x[m] = row->y[m] = MT_NONE;
    int32_t have_c = np[MR_INK_MAGENTA] > 0;
    double cx = have_c ? px[MR_INK_MAGENTA][0] : 0, cy = have_c ? py[MR_INK_MAGENTA][0] : 0;
    for (int32_t i = 0; i < MR_SQUARE_INKS; i++) {
        int32_t k = mr_square_ink(i);
        if (np[k] == 1) {
            row->x[mt_mark(i, MT_Q_ONE)] = px[k][0];
            row->y[mt_mark(i, MT_Q_ONE)] = py[k][0];
        } else if (np[k] > 1 && have_c && k != MR_INK_MAGENTA) {
            /* told apart by quadrant around the board's centre; a later one
             * in the same quadrant wins, as it did in the reader replaced */
            for (int32_t j = 0; j < np[k]; j++) {
                int32_t q = py[k][j] < cy ? (px[k][j] < cx ? MT_Q_TL : MT_Q_TR)
                                          : (px[k][j] < cx ? MT_Q_BL : MT_Q_BR);
                row->x[mt_mark(i, q)] = px[k][j];
                row->y[mt_mark(i, q)] = py[k][j];
            }
        }
    }
    free(bl); free(par); free(lab); free(ink);
}

/* ---- the table -------------------------------------------------------- */

void mt_write_header(void *file) {
    FILE *f = file;
    char b[32];
    fprintf(f, "# motion marks v1: t (s) clock (ms mod 2^14, -1 unread) red green (pt) then x y (pt) per mark, - missing\n");
    fprintf(f, "t clock red green");
    for (int32_t m = 0; m < MT_MARKS; m++) {
        mt_mark_name(m, b, sizeof b);
        fprintf(f, " %s_x %s_y", b, b);
    }
    fprintf(f, "\n");
}

static void put(FILE *f, double v) {
    if (v == MT_NONE) fprintf(f, " -"); else fprintf(f, " %.2f", v);
}

void mt_write_row(void *file, const MtRow *r) {
    FILE *f = file;
    fprintf(f, "%.6f %d", r->t, r->clock);
    put(f, r->red);
    put(f, r->green);
    for (int32_t m = 0; m < MT_MARKS; m++) { put(f, r->x[m]); put(f, r->y[m]); }
    fprintf(f, "\n");
}

int32_t mt_read_table(const char *path, MtRow *rows, int32_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    static char line[1 << 16];
    int32_t n = 0, cols = 0;
    int32_t map[4 + 2 * MT_MARKS];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *save = NULL, *tok = strtok_r(line, " \n", &save);
        if (!tok) continue;
        if (!strcmp(tok, "t")) {                       /* the column names */
            cols = 0;
            for (; tok; tok = strtok_r(NULL, " \n", &save)) {
                int32_t id = -1;
                if (!strcmp(tok, "t")) id = 0;
                else if (!strcmp(tok, "clock")) id = 1;
                else if (!strcmp(tok, "red")) id = 2;
                else if (!strcmp(tok, "green")) id = 3;
                else {
                    size_t L = strlen(tok);
                    if (L > 2 && tok[L - 2] == '_') {
                        char nm[32];
                        snprintf(nm, sizeof nm, "%.*s", (int)(L - 2), tok);
                        int32_t m = mt_mark_by_name(nm);
                        if (m >= 0) id = 4 + 2 * m + (tok[L - 1] == 'y');
                    }
                }
                if (cols < 4 + 2 * MT_MARKS) map[cols++] = id;
            }
            continue;
        }
        if (n >= cap) break;
        MtRow *r = &rows[n];
        r->t = 0; r->clock = -1; r->red = r->green = MT_NONE;
        for (int32_t m = 0; m < MT_MARKS; m++) r->x[m] = r->y[m] = MT_NONE;
        for (int32_t c = 0; tok && c < cols; c++, tok = strtok_r(NULL, " \n", &save)) {
            double v = strcmp(tok, "-") ? atof(tok) : MT_NONE;
            int32_t id = map[c];
            if (id == 0) r->t = v;
            else if (id == 1) r->clock = v == MT_NONE ? -1 : (int32_t)v;
            else if (id == 2) r->red = v;
            else if (id == 3) r->green = v;
            else if (id >= 4) { int32_t m = (id - 4) / 2; if ((id - 4) % 2) r->y[m] = v; else r->x[m] = v; }
        }
        n++;
    }
    fclose(f);
    return n;
}

/* ---- scoring ---------------------------------------------------------- */

void mt_default_opts(MtScoreOpts *o) {
    memset(o, 0, sizeof *o);
    o->span = 1.2;
    o->snap = 4.0;
    o->response = 0.338;
    /* THE SPEC'S EDGES (docs/UI.html "What holds which edge", in both
     * products): the header holds the top, the doors the bottom, the board
     * the centre. Violet is the pen on a live board; pass --anchor for a door. */
    for (int32_t m = 0; m < MT_MARKS; m++) o->anchor[m] = MT_ANCHOR_MID;
    for (int32_t q = 0; q < MT_QUADS; q++) {
        o->anchor[mt_mark(3, q)] = MT_ANCHOR_RED;     /* orange: the you-are mark */
        o->anchor[mt_mark(2, q)] = MT_ANCHOR_RED;     /* yellow: the headline     */
        o->anchor[mt_mark(6, q)] = MT_ANCHOR_RED;     /* lime: the line under it  */
        o->anchor[mt_mark(4, q)] = MT_ANCHOR_GREEN;   /* blue: the rulebook door  */
    }
    for (int32_t m = 0; m < MT_MARKS; m++)
        o->scaled[m] = o->anchor[m] == MT_ANCHOR_MID && m != mt_mark(0, MT_Q_ONE);
}

void mt_fix_bottom(MtRow *rows, int32_t n, double y, double hc) {
    for (int32_t i = 0; i < n; i++) {
        if (rows[i].red == MT_NONE) continue;
        if (hc > 0 && rows[i].red + hc > y) rows[i].green = rows[i].red + hc;
        else if (rows[i].green == MT_NONE || fabs(rows[i].green - y) > MT_CARD_SHIFT) rows[i].green = y;
    }
}

double mt_host_progress(double t, double response) {
    double w = 2 * M_PI / response;
    return t > 0 ? 1 - (1 + w * t) * exp(-w * t) : 0.0;
}

static double bar(const MtRow *r, int32_t which) {
    return which == 0 ? r->red : r->green;
}

static double anchor_of(const MtRow *r, int32_t a) {
    if (a == MT_ANCHOR_NONE) return 0;
    if (a == MT_ANCHOR_MID)
        return r->red == MT_NONE || r->green == MT_NONE ? MT_NONE : (r->red + r->green) / 2;
    return bar(r, a == MT_ANCHOR_RED ? 0 : 1);
}

static double side_at(const MtScoreOpts *o, double h) {
    double lo = o->side_h0, hi = o->side_h1;
    if (h < lo) h = lo;
    if (h > hi) h = hi;
    int32_t i = (int32_t)floor(h - lo);
    double f = h - lo - i;
    if (i >= o->side_h1 - o->side_h0) return o->side[o->side_h1 - o->side_h0];
    return o->side[i] * (1 - f) + o->side[i + 1] * f;
}

static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* THE WINDOW a take is scored over: from three frames before the first move
 * of either bar (more than 1pt from its first reading) to `span` after it, or
 * the whole take. Returns 0 when nothing moved and `whole` is off. */
static int32_t window(const MtRow *rows, int32_t n, const MtScoreOpts *o,
                      int32_t *a_out, int32_t *z_out, double *t0_out) {
    if (n < 4) return 0;
    int32_t i0 = -1;
    for (int32_t b = 0; b < 2; b++) {
        double v0 = MT_NONE;
        for (int32_t i = 0; i < n; i++) {
            double v = bar(&rows[i], b);
            if (v == MT_NONE) continue;
            if (v0 == MT_NONE) { v0 = v; continue; }
            if (fabs(v - v0) > 1) { if (i0 < 0 || i < i0) i0 = i; break; }
        }
    }
    double span = o->span;
    if (o->whole) { i0 = 3; span = 1e9; }
    if (i0 < 0) return 0;
    double t0 = rows[i0].t;
    int32_t a = i0 - 3 < 0 ? 0 : i0 - 3, z = a;
    while (z < n && rows[z].t <= t0 + span) z++;
    *a_out = a; *z_out = z; *t0_out = t0;
    return 1;
}

int32_t mt_score(const MtRow *rows, int32_t n, const MtScoreOpts *o, MtScore out[MT_MARKS]) {
    memset(out, 0, sizeof(MtScore) * MT_MARKS);
    int32_t a, z;
    double t0;
    if (!window(rows, n, o, &a, &z, &t0)) return 0;
    int32_t L = z - a;
    double *A = malloc(sizeof(double) * (size_t)L), *Y = malloc(sizeof(double) * (size_t)L);
    double *E = malloc(sizeof(double) * (size_t)L), *HH = malloc(sizeof(double) * (size_t)L);
    for (int32_t m = 0; m < MT_MARKS; m++) {
        int32_t seen = 0, fs = -1, ls = -1;
        for (int32_t k = 0; k < L; k++) {
            const MtRow *r = &rows[a + k];
            Y[k] = r->y[m];
            A[k] = anchor_of(r, o->anchor[m]);
            HH[k] = r->red == MT_NONE || r->green == MT_NONE ? MT_NONE : r->green - r->red;
            if (Y[k] != MT_NONE) { seen++; if (fs < 0) fs = k; ls = k; }
        }
        if (seen < 3) continue;
        MtScore *s = &out[m];
        s->seen = 1;
        /* A BOARD MARK OFF THE CENTRE SCALES WITH THE BOARD: its expected
         * offset is its offset where the drawer is shorter (the side is
         * sensitive to the height there) times the side's ratio. */
        if (o->side && o->scaled[m] && o->anchor[m] == MT_ANCHOR_MID) {
            int32_t kf = -1, kl = -1;
            for (int32_t k = 0; k < L; k++)
                if (Y[k] != MT_NONE && A[k] != MT_NONE && HH[k] != MT_NONE) { if (kf < 0) kf = k; kl = k; }
            if (kf >= 0) {
                int32_t k0 = HH[kl] < HH[kf] ? kl : kf;
                double e0 = fabs(Y[k0] - A[k0]), sg = Y[k0] >= A[k0] ? 1 : -1;
                double best = 1e300; int32_t bc = 0;
                for (int32_t c = 0; c <= 24; c++) {
                    double cost = 0, prev = MT_NONE;
                    for (int32_t k = 0; k < L; k++) {
                        if (Y[k] == MT_NONE || A[k] == MT_NONE || HH[k] == MT_NONE) continue;
                        double res = Y[k] - (A[k] + sg * e0 * side_at(o, HH[k] + c) / side_at(o, HH[k0] + c));
                        if (prev != MT_NONE) cost += (res - prev) * (res - prev);
                        prev = res;
                    }
                    if (cost < best) { best = cost; bc = c; }
                }
                for (int32_t k = 0; k < L; k++)
                    E[k] = A[k] == MT_NONE || HH[k] == MT_NONE ? MT_NONE
                         : A[k] + sg * e0 * side_at(o, HH[k] + bc) / side_at(o, HH[k0] + bc);
                memcpy(A, E, sizeof(double) * (size_t)L);
            }
        }
        /* ride.py: the anchor has stopped after the last step above 0.5pt */
        int32_t last = 0;
        for (int32_t k = 1; k < L; k++)
            if (A[k] != MT_NONE && A[k - 1] != MT_NONE && fabs(A[k] - A[k - 1]) > 0.5) last = k;
        for (int32_t k = fs; k <= ls; k++) if (Y[k] == MT_NONE) s->miss++;
        for (int32_t k = 0; k < L; k++) {
            const MtRow *r = &rows[a + k];
            if (Y[k] == MT_NONE || r->red == MT_NONE || r->green == MT_NONE) continue;
            if (Y[k] < r->red - MT_OFF_TOL || Y[k] > r->green + MT_OFF_TOL) s->off++;
        }
        for (int32_t k = 1; k < L; k++) {
            if (Y[k] == MT_NONE || Y[k - 1] == MT_NONE || A[k] == MT_NONE || A[k - 1] == MT_NONE) continue;
            double d = (Y[k] - A[k]) - (Y[k - 1] - A[k - 1]);
            if (fabs(d) > s->maxstep) s->maxstep = fabs(d);
            if (fabs(d) > o->snap) {
                s->snaps++;
                if (fabs(d) > s->maxsnap) s->maxsnap = fabs(d);
                if (k > last) s->late++;
            }
        }
        for (int32_t k = 2; k < L; k++)
            if (Y[k] != MT_NONE && Y[k - 1] != MT_NONE && Y[k - 2] != MT_NONE) {
                double d2 = Y[k] - 2 * Y[k - 1] + Y[k - 2];
                s->rough += d2 * d2;
            }
        /* bars.py, from the first move on: a step across a gap longer than a
         * frame and a half is not a step */
        int32_t q = 0;
        double *tw = malloc(sizeof(double) * (size_t)L), *yw = malloc(sizeof(double) * (size_t)L);
        for (int32_t k = 0; k < L; k++) {
            double t = rows[a + k].t - t0;
            if (Y[k] == MT_NONE || t < 0) continue;
            tw[q] = t; yw[q] = Y[k]; q++;
        }
        if (q >= 5) {
            int32_t *cl = calloc((size_t)q, sizeof(int32_t));
            if (q > 3) {
                double *dt = malloc(sizeof(double) * (size_t)(q - 1));
                for (int32_t k = 0; k + 1 < q; k++) dt[k] = tw[k + 1] - tw[k];
                qsort(dt, (size_t)(q - 1), sizeof(double), cmpd);
                double med = (q - 1) % 2 ? dt[(q - 1) / 2] : (dt[(q - 1) / 2 - 1] + dt[(q - 1) / 2]) / 2;
                for (int32_t k = 1; k < q; k++) if (tw[k] - tw[k - 1] > 1.5 * med) cl[k] = 1;
                free(dt);
            }
            s->travel = fabs(yw[q - 1] - yw[0]);
            for (int32_t k = 1; k < q; k++) {
                if (cl[k] || cl[k - 1]) continue;
                double d = yw[k] - yw[k - 1];
                double f = s->travel * (mt_host_progress(tw[k], o->response) - mt_host_progress(tw[k - 1], o->response));
                s->jerk += d * d;
                s->floor += f * f;
            }
            for (int32_t k = 0; k < q; k++) {
                if (cl[k]) continue;
                double line = tw[q - 1] > tw[0]
                    ? yw[0] + (yw[q - 1] - yw[0]) * (tw[k] - tw[0]) / (tw[q - 1] - tw[0]) : yw[0];
                s->stray += (yw[k] - line) * (yw[k] - line);
            }
            free(cl);
        }
        free(tw); free(yw);
    }
    free(A); free(Y); free(E); free(HH);
    return 1;
}

/* ---- the board's size --------------------------------------------------- */

static double pair_mean(double a0, double a1, double b0, double b1) {
    int32_t k = 0;
    double sum = 0;
    if (a0 != MT_NONE && a1 != MT_NONE) { sum += a1 - a0; k++; }
    if (b0 != MT_NONE && b1 != MT_NONE) { sum += b1 - b0; k++; }
    return k ? sum / k : MT_NONE;
}

/* the board's corner marks: the cyan squares, split by quadrant */
static void corners(int32_t *tl, int32_t *tr, int32_t *bl, int32_t *br) {
    static int32_t c[4] = {-1, -1, -1, -1};
    if (c[0] < 0) {
        c[0] = mt_mark_by_name("cyan_tl"); c[1] = mt_mark_by_name("cyan_tr");
        c[2] = mt_mark_by_name("cyan_bl"); c[3] = mt_mark_by_name("cyan_br");
    }
    *tl = c[0]; *tr = c[1]; *bl = c[2]; *br = c[3];
}

double mt_board_w(const MtRow *r) {
    int32_t tl, tr, bl, br;
    corners(&tl, &tr, &bl, &br);
    return pair_mean(r->x[tl], r->x[tr], r->x[bl], r->x[br]);
}

double mt_board_h(const MtRow *r) {
    int32_t tl, tr, bl, br;
    corners(&tl, &tr, &bl, &br);
    return pair_mean(r->y[tl], r->y[bl], r->y[tr], r->y[br]);
}

int32_t mt_reversals(const double *v, int32_t n, double tol) {
    int32_t dir = 0, revs = 0;
    double ext = MT_NONE, start = MT_NONE;
    for (int32_t i = 0; i < n; i++) {
        if (v[i] == MT_NONE) continue;
        if (start == MT_NONE) { start = ext = v[i]; continue; }
        if (dir == 0) {
            if (fabs(v[i] - start) > tol) { dir = v[i] > start ? 1 : -1; ext = v[i]; }
        } else if ((v[i] - ext) * dir > 0) {
            ext = v[i];
        } else if ((ext - v[i]) * dir > tol) {
            revs++; dir = -dir; ext = v[i];
        }
    }
    return revs;
}

static void series(const double *v, int32_t n, double *maxstep, double *rough, double *first, double *last) {
    *maxstep = 0; *rough = 0; *first = *last = MT_NONE;
    for (int32_t k = 0; k < n; k++) {
        if (v[k] == MT_NONE) continue;
        if (*first == MT_NONE) *first = v[k];
        *last = v[k];
        if (k >= 1 && v[k - 1] != MT_NONE && fabs(v[k] - v[k - 1]) > *maxstep) *maxstep = fabs(v[k] - v[k - 1]);
        if (k >= 2 && v[k - 1] != MT_NONE && v[k - 2] != MT_NONE) {
            double d2 = v[k] - 2 * v[k - 1] + v[k - 2];
            *rough += d2 * d2;
        }
    }
}

int32_t mt_board(const MtRow *rows, int32_t n, const MtScoreOpts *o, MtBoard *out) {
    memset(out, 0, sizeof *out);
    int32_t a, z;
    double t0;
    if (!window(rows, n, o, &a, &z, &t0)) return 0;
    int32_t L = z - a;
    if (L < 2) return 0;
    double *w = malloc(sizeof(double) * (size_t)L), *h = malloc(sizeof(double) * (size_t)L);
    double *d = malloc(sizeof(double) * (size_t)L);
    for (int32_t k = 0; k < L; k++) {
        const MtRow *r = &rows[a + k];
        w[k] = mt_board_w(r);
        h[k] = mt_board_h(r);
        d[k] = r->red == MT_NONE || r->green == MT_NONE ? MT_NONE : r->green - r->red;
        if (w[k] != MT_NONE || h[k] != MT_NONE) out->seen++;
        if (w[k] != MT_NONE && h[k] != MT_NONE && fabs(w[k] - h[k]) > out->maxskew)
            out->maxskew = fabs(w[k] - h[k]);
    }
    series(w, L, &out->w_maxstep, &out->w_rough, &out->w_first, &out->w_last);
    series(h, L, &out->h_maxstep, &out->h_rough, &out->h_first, &out->h_last);
    if (o->side) {
        /* the bars' distance plus a constant is the drawer's height the side
         * table is keyed by; the constant is fitted 0..24pt as mt_score does */
        double *e = malloc(sizeof(double) * (size_t)L);
        for (int32_t pass = 0; pass < 2; pass++) {
            double *v = pass ? h : w;
            int32_t k0 = -1;
            for (int32_t k = 0; k < L; k++) if (v[k] != MT_NONE && d[k] != MT_NONE) { k0 = k; break; }
            if (k0 < 0) continue;
            double best = 1e300; int32_t bc = 0;
            for (int32_t c = 0; c <= 24; c++) {
                double cost = 0, prev = MT_NONE;
                for (int32_t k = 0; k < L; k++) {
                    if (v[k] == MT_NONE || d[k] == MT_NONE) continue;
                    double r = v[k] - v[k0] - side_at(o, d[k] + c) + side_at(o, d[k0] + c);
                    if (prev != MT_NONE) cost += (r - prev) * (r - prev);
                    prev = r;
                }
                if (cost < best) { best = cost; bc = c; }
            }
            double mstep = 0, mabs = 0, prev = MT_NONE;
            for (int32_t k = 0; k < L; k++) {
                e[k] = MT_NONE;
                if (v[k] == MT_NONE || d[k] == MT_NONE) { prev = MT_NONE; continue; }
                e[k] = v[k] - v[k0] - side_at(o, d[k] + bc) + side_at(o, d[k0] + bc);
                if (fabs(e[k]) > mabs) mabs = fabs(e[k]);
                if (prev != MT_NONE && fabs(e[k] - prev) > mstep) mstep = fabs(e[k] - prev);
                prev = e[k];
            }
            if (pass) { out->h_res_step = mstep; out->h_res_max = mabs; }
            else { out->w_res_step = mstep; out->w_res_max = mabs; }
        }
        free(e);
    }
    out->w_rev = mt_reversals(w, L, MT_REV_TOL);
    out->h_rev = mt_reversals(h, L, MT_REV_TOL);
    out->drawer_rev = mt_reversals(d, L, MT_REV_TOL);
    free(w); free(h); free(d);
    return out->seen > 0;
}

/* ---- pace -------------------------------------------------------------- */

int32_t mt_box_ink(const uint8_t *rgb, int32_t W, int32_t H, MtBox b, int32_t lum) {
    int32_t n = 0;
    for (int32_t y = b.y < 0 ? 0 : b.y; y < b.y + b.h && y < H; y++)
        for (int32_t x = b.x < 0 ? 0 : b.x; x < b.x + b.w && x < W; x++) {
            const uint8_t *p = rgb + ((size_t)y * (size_t)W + (size_t)x) * 3;
            int32_t l = (299 * p[0] + 587 * p[1] + 114 * p[2]) / 1000;
            if (l < lum && ink_of(p[0], p[1], p[2]) < 0) n++;
        }
    return n;
}

int32_t mt_box_diff(const uint8_t *a, const uint8_t *b, int32_t W, int32_t H, MtBox box,
                    int32_t tol) {
    int32_t n = 0;
    for (int32_t y = box.y < 0 ? 0 : box.y; y < box.y + box.h && y < H; y++)
        for (int32_t x = box.x < 0 ? 0 : box.x; x < box.x + box.w && x < W; x++) {
            size_t o = ((size_t)y * (size_t)W + (size_t)x) * 3;
            for (int32_t c = 0; c < 3; c++)
                if (abs((int32_t)a[o + c] - (int32_t)b[o + c]) > tol) { n++; break; }
        }
    return n;
}

void mt_pace(const double *t, const int32_t *ink, const int32_t *changed, int32_t n,
             MtPace *o) {
    memset(o, 0, sizeof *o);
    if (n < 2) return;
    int32_t first = -1, last = -1;
    double prev_t = 0;
    for (int32_t i = 1; i < n; i++) {
        if (!changed[i]) continue;
        if (first < 0) first = i;
        else if (t[i] - prev_t > o->maxgap) o->maxgap = t[i] - prev_t;
        prev_t = t[i];
        last = i;
        o->frames++;
    }
    if (first < 0) return;
    o->t0 = t[first]; o->t1 = t[last];
    o->fps = o->t1 > o->t0 ? (o->frames - 1) / (o->t1 - o->t0) : 0;
    /* the ink's share: from what the box held before the first change to
     * what it holds at the end */
    double i0 = ink[first - 1], i1 = ink[n - 1], span = i1 - i0;
    if (span <= 0) return;
    for (int32_t i = first; i <= last; i++) {
        double step = (ink[i] - ink[i - 1]) / span;
        if (step > o->maxstep) o->maxstep = step;
    }
    /* as the eye sees it: each frame held until the next, on a 60 Hz grid */
    double a = 0, b = 0, r = 0;
    int32_t k = first - 1, m = 0;
    for (double g = o->t0 - 1.0 / 60; g <= o->t1 + 1.0 / 60; g += 1.0 / 60, m++) {
        while (k + 1 < n && t[k + 1] <= g + 1e-9) k++;
        double v = (ink[k] - i0) / span;
        if (m >= 2) { double d = v - 2 * b + a; r += d * d; }
        a = b; b = v;
    }
    o->rough = r * 1000;
}

/* ---- grid --------------------------------------------------------------- */

static int32_t paper_px(const uint8_t *p) {
    int32_t mn = p[0] < p[1] ? p[0] : p[1]; mn = mn < p[2] ? mn : p[2];
    int32_t mx = p[0] > p[1] ? p[0] : p[1]; mx = mx > p[2] ? mx : p[2];
    return mn >= MT_GRID_PAPER_MIN && mx - mn <= MT_GRID_PAPER_SPREAD;
}

static int32_t ink_px(const uint8_t *p) {
    int32_t mn = p[0] < p[1] ? p[0] : p[1]; mn = mn < p[2] ? mn : p[2];
    int32_t mx = p[0] > p[1] ? p[0] : p[1]; mx = mx > p[2] ? mx : p[2];
    return mx < MT_GRID_INK_MAX && mx - mn <= MT_GRID_INK_SPREAD;
}

/* The two heaviest runs of a profile over `thr`: consecutive entries (gaps up
 * to `gap`) are one line, placed at its weighted centre. */
static int32_t two_lines(const int32_t *prof, int32_t n, int32_t thr, int32_t gap, double *a, double *b) {
    double best[2] = {0, 0}, at[2] = {0, 0};
    int32_t i = 0;
    while (i < n) {
        if (prof[i] <= thr) { i++; continue; }
        double sw = 0, sx = 0;
        int32_t j = i, last = i;
        while (j < n && j - last <= gap) {
            if (prof[j] > thr) { sw += prof[j]; sx += (double)prof[j] * j; last = j; }
            j++;
        }
        double c = sx / sw;
        if (sw > best[0]) { best[1] = best[0]; at[1] = at[0]; best[0] = sw; at[0] = c; }
        else if (sw > best[1]) { best[1] = sw; at[1] = c; }
        i = last + 1;
    }
    if (best[1] <= 0) return 0;
    *a = at[0] < at[1] ? at[0] : at[1];
    *b = at[0] < at[1] ? at[1] : at[0];
    return 1;
}

void mt_grid(const uint8_t *rgb, int32_t w, int32_t h, double scale, MtGrid *g) {
    g->top = g->h1 = g->h2 = g->v1 = g->v2 = MT_NONE;
    /* THE DRAWER'S TOP: the first row below the status bar that is mostly
     * paper across the middle of the screen, and stays so for a few rows (a
     * white bubble is narrower than the drawer) */
    int32_t x0 = w / 20, x1 = w - w / 20, top = -1;
    for (int32_t y = (int32_t)(40 * scale); y + 12 < h && top < 0; y++) {
        int32_t ok = 1;
        for (int32_t dy = 0; dy <= 12 && ok; dy += 6) {
            int32_t c = 0, k = 0;
            for (int32_t x = x0; x < x1; x += 4, k++) c += paper_px(rgb + ((size_t)(y + dy) * w + x) * 3);
            ok = c * 100 >= k * 85;
        }
        if (ok) top = y;
    }
    if (top < 0) return;
    g->top = top / scale;
    /* THE HEAVY ROWS: dark neutral ink across a third of the screen, inside
     * the drawer - not the screen's own dark edges (a recording's last rows,
     * the wallpaper past the drawer's rounded corners) */
    int32_t edge = (int32_t)(12 * scale), yend = h - edge;
    int32_t *prof = calloc((size_t)(h > w ? h : w), sizeof(int32_t));
    for (int32_t y = top + (int32_t)(4 * scale); y < yend; y++) {
        int32_t c = 0;
        for (int32_t x = edge; x < w - edge; x += 2) c += ink_px(rgb + ((size_t)y * w + x) * 3);
        prof[y] = c;
    }
    double a, b;
    if (two_lines(prof, h, w / 2 / 3, (int32_t)(2 * scale), &a, &b) && b - a > 20 * scale) {
        g->h1 = a / scale; g->h2 = b / scale;
        /* THE HEAVY COLUMNS, over the board's span (a third either side) */
        double s = b - a;
        int32_t ya = (int32_t)(a - s), yb = (int32_t)(b + s);
        if (ya < top) ya = top;
        if (yb > yend) yb = yend;
        memset(prof, 0, sizeof(int32_t) * (size_t)w);
        for (int32_t x = edge; x < w - edge; x++) {
            int32_t c = 0;
            for (int32_t y = ya; y < yb; y += 2) c += ink_px(rgb + ((size_t)y * w + x) * 3);
            prof[x] = c;
        }
        if (two_lines(prof, w, (int32_t)(s / 2 * 1.2), (int32_t)(2 * scale), &a, &b)) {
            g->v1 = a / scale; g->v2 = b / scale;
        }
    }
    free(prof);
}

int32_t mt_grid_board(const MtGrid *g, double *cx, double *cy, double *side) {
    if (g->h1 == MT_NONE || g->v1 == MT_NONE) return 0;
    *cx = (g->v1 + g->v2) / 2;
    *cy = (g->h1 + g->h2) / 2;
    *side = 1.5 * ((g->v2 - g->v1) + (g->h2 - g->h1));
    return 1;
}
