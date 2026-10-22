// unit_mvt.c - the MVT writer decoded back by a protobuf reader of its own (so the bytes, not
// the writer's idea of them, are what is checked), and the renderer over a fakegraph grid:
// which segments each layer holds, the geometry of covered runs, the zoom rules, the tile path
// parser, and how long a busy z14 tile takes.
#include "coverage.h"
#include "graph.h"
#include "lib_fakegraph.h"
#include "mvt.h"
#include "t.h"
#include "util/clock.h"
#include "util/err.h"

#include <math.h>
#include <stdatomic.h>
#include <unistd.h>

// ---- a protobuf reader ----------------------------------------------------------------------

typedef struct {
    const uint8_t *p;
    size_t n, at;
    bool err;
} pb;

static uint64_t pb_varint(pb *r) {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->at >= r->n) {
            r->err = true;
            return 0;
        }
        uint8_t b = r->p[r->at++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
    }
    r->err = true;
    return 0;
}

// The next field's number and wire type; false at the end or on an error.
static bool pb_tag(pb *r, uint32_t *field, uint32_t *wt) {
    if (r->at >= r->n || r->err) return false;
    uint64_t t = pb_varint(r);
    if (r->err) return false;
    *field = (uint32_t)(t >> 3);
    *wt = (uint32_t)(t & 7);
    return true;
}

// A length-delimited field as a reader of its own.
static pb pb_sub(pb *r) {
    pb s = { 0 };
    uint64_t len = pb_varint(r);
    if (r->err || len > r->n - r->at) {
        r->err = true;
        return s;
    }
    s.p = r->p + r->at;
    s.n = (size_t)len;
    r->at += (size_t)len;
    return s;
}

static void pb_skip(pb *r, uint32_t wt) {
    if (wt == 0) pb_varint(r);
    else if (wt == 2) pb_sub(r);
    else if (wt == 1) r->at += 8;
    else if (wt == 5) r->at += 4;
    else r->err = true;
    if (r->at > r->n) r->err = true;
}

#define D_MAX_FEATURES 8192
#define D_MAX_PTS      64
#define D_MAX_LAYERS   4
#define D_MAX_LINES    (1 << 18)

// One line of a feature as decoded: a grouped feature (a MULTILINESTRING) has many. Its feature,
// its point count, its two ends, and a hash of every point, so two encodings of the same lines
// compare equal however the features hold them.
typedef struct {
    int      feat;
    int      n;
    int32_t  x0, y0, x1, y1;
    uint64_t h;
} d_line;

typedef struct {
    uint64_t id;
    int      type;
    int      n_tags;                        // pairs
    uint32_t tags[2 * ADV_MVT_MAX_TAGS];    // key, value, key, value...
    int      n_pts;                         // decoded; xy holds the first D_MAX_PTS
    int32_t  xy[2 * D_MAX_PTS];
    bool     geom_ok;                       // pairs of (MoveTo of 1, LineTo of k >= 1): one pair is a line
    int      n_lines;                       // the pairs: 1 for a LINESTRING, more for a grouped feature
    bool     has_id;
} d_feat;

typedef struct {
    char     name[32];
    int      n_keys;
    char     keys[ADV_MVT_MAX_TAGS][8];
    int      n_values;
    int64_t  values[ADV_MVT_MAX_VALUE + 1];
    int      extent, version;
    int      n_features;
    d_feat  *f;
    int      n_lines;
    d_line  *lines;                         // every line of every feature, in order
} d_layer;

typedef struct {
    int     n_layers;
    d_layer l[D_MAX_LAYERS];
    bool    err;
} d_tile;

static d_tile *d_tile_new(void) {
    d_tile *t = calloc(1, sizeof *t);
    for (int i = 0; i < D_MAX_LAYERS; i++) {
        t->l[i].f = calloc(D_MAX_FEATURES, sizeof(d_feat));
        t->l[i].lines = calloc(D_MAX_LINES, sizeof(d_line));
    }
    return t;
}

static int64_t unzigzag(uint64_t v) { return (int64_t)(v >> 1) ^ -(int64_t)(v & 1); }

static void d_geometry(pb g, d_feat *f, d_layer *l, int feat, bool *err) {
    int32_t cx = 0, cy = 0;
    int cmds = 0;
    f->n_pts = 0;
    f->n_lines = 0;
    f->geom_ok = true;
    d_line *ln = NULL;
    while (g.at < g.n && !g.err) {
        uint64_t c = pb_varint(&g);
        uint32_t id = (uint32_t)(c & 7), count = (uint32_t)(c >> 3);
        // Even commands are a MoveTo of one point, odd ones a LineTo of at least one.
        if (cmds % 2 == 0 && !(id == 1 && count == 1)) f->geom_ok = false;
        if (cmds % 2 == 1 && !(id == 2 && count >= 1)) f->geom_ok = false;
        if (cmds % 2 == 0) {
            if (l->n_lines >= D_MAX_LINES) *err = true;
            else {
                ln = &l->lines[l->n_lines++];
                memset(ln, 0, sizeof *ln);
                ln->feat = feat;
                ln->h = 1469598103934665603ull;
            }
            f->n_lines++;
        }
        cmds++;
        for (uint32_t k = 0; k < count && !g.err; k++) {
            // MVT parameters are 32-bit: a wider varint is a wrong encoding, not a big delta.
            uint64_t zx = pb_varint(&g), zy = pb_varint(&g);
            if (zx > UINT32_MAX || zy > UINT32_MAX) f->geom_ok = false;
            cx += (int32_t)unzigzag(zx);
            cy += (int32_t)unzigzag(zy);
            if (f->n_pts < D_MAX_PTS) {
                f->xy[2 * f->n_pts] = cx;
                f->xy[2 * f->n_pts + 1] = cy;
            }
            f->n_pts++;
            if (ln) {
                if (ln->n == 0) { ln->x0 = cx; ln->y0 = cy; }
                ln->x1 = cx;
                ln->y1 = cy;
                ln->h = (ln->h ^ (uint32_t)cx) * 1099511628211ull;
                ln->h = (ln->h ^ (uint32_t)cy) * 1099511628211ull;
                ln->n++;
            }
        }
    }
    if (g.err || cmds < 2 || cmds % 2) f->geom_ok = false;
}

static void d_feature(pb r, d_layer *l, bool *err) {
    if (l->n_features >= D_MAX_FEATURES) {
        *err = true;
        return;
    }
    d_feat *f = &l->f[l->n_features++];
    memset(f, 0, sizeof *f);
    uint32_t field, wt;
    while (pb_tag(&r, &field, &wt)) {
        if (field == 1 && wt == 0) {
            f->id = pb_varint(&r);
            f->has_id = true;
        }
        else if (field == 3 && wt == 0) f->type = (int)pb_varint(&r);
        else if (field == 2 && wt == 2) {
            pb t = pb_sub(&r);
            int k = 0;
            while (t.at < t.n && !t.err) {
                uint64_t v = pb_varint(&t);
                if (k < 2 * ADV_MVT_MAX_TAGS) f->tags[k] = (uint32_t)v;
                k++;
            }
            if (k % 2 || t.err) *err = true;
            f->n_tags = k / 2;
        } else if (field == 4 && wt == 2) d_geometry(pb_sub(&r), f, l, l->n_features - 1, err);
        else pb_skip(&r, wt);
    }
    if (r.err) *err = true;
}

static void d_layer_read(pb r, d_layer *l, bool *err) {
    uint32_t field, wt;
    while (pb_tag(&r, &field, &wt)) {
        if (field == 1 && wt == 2) {
            pb s = pb_sub(&r);
            size_t n = s.n < sizeof l->name - 1 ? s.n : sizeof l->name - 1;
            memcpy(l->name, s.p, n);
            l->name[n] = 0;
        } else if (field == 2 && wt == 2) d_feature(pb_sub(&r), l, err);
        else if (field == 3 && wt == 2) {
            pb s = pb_sub(&r);
            if (l->n_keys < ADV_MVT_MAX_TAGS) {
                size_t n = s.n < 7 ? s.n : 7;
                memcpy(l->keys[l->n_keys], s.p, n);
                l->keys[l->n_keys][n] = 0;
            }
            l->n_keys++;
        } else if (field == 4 && wt == 2) {
            pb s = pb_sub(&r);
            uint32_t vf, vw;
            int64_t value = -1;
            while (pb_tag(&s, &vf, &vw)) {
                if (vf == 4 && vw == 0) value = (int64_t)pb_varint(&s);
                else pb_skip(&s, vw);
            }
            if (s.err || value < 0) *err = true;
            if (l->n_values <= ADV_MVT_MAX_VALUE) l->values[l->n_values] = value;
            l->n_values++;
        } else if (field == 5 && wt == 0) l->extent = (int)pb_varint(&r);
        else if (field == 15 && wt == 0) l->version = (int)pb_varint(&r);
        else pb_skip(&r, wt);
    }
    if (r.err) *err = true;
}

static void d_decode(const uint8_t *p, size_t n, d_tile *t) {
    for (int i = 0; i < D_MAX_LAYERS; i++) {
        d_feat *f = t->l[i].f;
        d_line *ln = t->l[i].lines;
        memset(&t->l[i], 0, sizeof t->l[i]);
        t->l[i].f = f;
        t->l[i].lines = ln;
    }
    t->n_layers = 0;
    t->err = false;
    pb r = { p, n, 0, false };
    uint32_t field, wt;
    while (pb_tag(&r, &field, &wt)) {
        if (field == 3 && wt == 2) {
            if (t->n_layers >= D_MAX_LAYERS) {
                t->err = true;
                return;
            }
            d_layer_read(pb_sub(&r), &t->l[t->n_layers++], &t->err);
        } else pb_skip(&r, wt);
    }
    if (r.err) t->err = true;
}

static const d_layer *d_find(const d_tile *t, const char *name) {
    for (int i = 0; i < t->n_layers; i++)
        if (strcmp(t->l[i].name, name) == 0) return &t->l[i];
    return NULL;
}

static const d_feat *d_feature_by_id(const d_layer *l, uint64_t id) {
    for (int i = 0; i < l->n_features; i++)
        if (l->f[i].id == id) return &l->f[i];
    return NULL;
}

static double d_length(const d_feat *f) {
    double len = 0;
    int n = f->n_pts < D_MAX_PTS ? f->n_pts : D_MAX_PTS;
    for (int i = 1; i < n; i++) len += hypot((double)f->xy[2 * i] - f->xy[2 * i - 2], (double)f->xy[2 * i + 1] - f->xy[2 * i - 1]);
    return len;
}

// Every feature a linestring (one line, with an id no other feature of the layer has) or a
// grouped multilinestring (no id, a tag list no other feature of the layer has), decodable.
static void check_layer_sane(const d_layer *l) {
    T_CHECK(l->extent == ADV_MVT_EXTENT && l->version == 2);
    for (int i = 0; i < l->n_features; i++) {
        const d_feat *f = &l->f[i];
        T_CHECK_MSG(f->type == 2 && f->geom_ok && f->n_pts >= 2 * f->n_lines && f->n_lines >= 1, "%s feature %d id %llu type %d ok %d pts %d",
                    l->name, i, (unsigned long long)f->id, f->type, f->geom_ok, f->n_pts);
        T_CHECK_MSG(f->has_id || f->n_lines >= 1, "%s feature %d", l->name, i);
        if (f->has_id) T_CHECK_MSG(f->n_lines == 1, "%s feature %d: an id on %d lines", l->name, i, f->n_lines);
        for (int t = 0; t < f->n_tags; t++)
            T_CHECK((int)f->tags[2 * t] < l->n_keys && (int)f->tags[2 * t + 1] < l->n_values);
        for (int j = i + 1; j < l->n_features; j++) {
            if (f->has_id && l->f[j].has_id) T_CHECK_MSG(l->f[j].id != f->id, "%s: id %llu twice", l->name, (unsigned long long)f->id);
            if (!f->has_id && !l->f[j].has_id)
                T_CHECK_MSG(f->n_tags != l->f[j].n_tags || memcmp(f->tags, l->f[j].tags, (size_t)f->n_tags * 2 * sizeof f->tags[0]) != 0,
                            "%s: features %d and %d group the same tags", l->name, i, j);
        }
    }
}

static int tag_value(const d_layer *l, const d_feat *f, const char *key) {
    for (int t = 0; t < f->n_tags; t++)
        if (strcmp(l->keys[f->tags[2 * t]], key) == 0) return (int)l->values[f->tags[2 * t + 1]];
    return -1;
}

// ---- the writer ------------------------------------------------------------------------------

static void test_writer(d_tile *t) {
    static uint8_t scratch[4096];
    adv_mvt w;
    adv_mvt_init(&w, scratch, sizeof scratch);
    const char *const keys[] = { "c", "t" };
    adv_mvt_layer_begin(&w, "roads", keys, 2);
    int32_t a[] = { 10, 20, 30, 25, 30, 25, 100, 0 };   // a repeated point in the middle
    int ta[] = { 5, -1 };
    adv_mvt_feature_line(&w, 7, ta, a, 4);
    int32_t b[] = { 0, 0, -5, -7 };
    int tb[] = { 2, 1 };
    adv_mvt_feature_line(&w, 9, tb, b, 2);
    int32_t c[] = { 50, 50, 50, 50, 50, 50 };   // a speck: dropped
    adv_mvt_feature_line(&w, 11, tb, c, 3);
    adv_mvt_feature_line(&w, 12, tb, b, 1);     // one point is not a line
    adv_mvt_layer_end(&w);
    adv_mvt_layer_begin(&w, "empty", keys, 1);
    adv_mvt_layer_end(&w);
    const uint8_t *out;
    int len = adv_mvt_finish(&w, &out);
    T_CHECK_MSG(len > 0, "len=%d", len);
    d_decode(out, (size_t)len, t);
    T_CHECK(!t->err);
    T_CHECK_MSG(t->n_layers == 2, "layers=%d", t->n_layers);
    const d_layer *l = &t->l[0];
    T_CHECK(strcmp(l->name, "roads") == 0);
    T_CHECK(l->n_keys == 2 && strcmp(l->keys[0], "c") == 0 && strcmp(l->keys[1], "t") == 0);
    T_CHECK_MSG(l->n_values == 6, "n_values=%d", l->n_values);
    for (int i = 0; i < l->n_values && i <= ADV_MVT_MAX_VALUE; i++) T_CHECK(l->values[i] == i);
    T_CHECK(l->extent == 4096 && l->version == 2);
    T_CHECK_MSG(l->n_features == 2, "features=%d", l->n_features);
    const d_feat *f = &l->f[0];
    T_CHECK(f->id == 7 && f->type == 2 && f->geom_ok);
    T_CHECK_MSG(f->n_pts == 3, "pts=%d", f->n_pts);
    T_CHECK(f->xy[0] == 10 && f->xy[1] == 20 && f->xy[2] == 30 && f->xy[3] == 25 && f->xy[4] == 100 && f->xy[5] == 0);
    T_CHECK(f->n_tags == 1 && f->tags[0] == 0 && f->tags[1] == 5);
    T_CHECK(tag_value(l, f, "c") == 5 && tag_value(l, f, "t") == -1);
    f = &l->f[1];
    T_CHECK(f->id == 9 && f->geom_ok && f->n_pts == 2);
    T_CHECK(f->xy[0] == 0 && f->xy[1] == 0 && f->xy[2] == -5 && f->xy[3] == -7);
    T_CHECK(f->n_tags == 2 && tag_value(l, f, "c") == 2 && tag_value(l, f, "t") == 1);
    check_layer_sane(l);
    l = &t->l[1];
    T_CHECK(strcmp(l->name, "empty") == 0 && l->n_features == 0 && l->n_values == 0 && l->n_keys == 1);
    T_CHECK(l->extent == 4096 && l->version == 2);
    // The raw bytes: the first layer's tag and the second's name field where the layout puts them.
    T_CHECK(out[0] == 0x1A);

    // A value above the cap is clamped, not written: the values table stays bounded.
    adv_mvt_init(&w, scratch, sizeof scratch);
    int th[] = { 1000, -1 };
    adv_mvt_layer_begin(&w, "x", keys, 2);
    adv_mvt_feature_line(&w, 1, th, b, 2);
    adv_mvt_layer_end(&w);
    len = adv_mvt_finish(&w, &out);
    T_CHECK(len > 0);
    d_decode(out, (size_t)len, t);
    T_CHECK(!t->err && t->l[0].n_values == ADV_MVT_MAX_VALUE + 1 && tag_value(&t->l[0], &t->l[0].f[0], "c") == ADV_MVT_MAX_VALUE);

    // Overflow: a feature that does not fit the layer half of a tiny scratch.
    int32_t many[2 * 100];
    for (int i = 0; i < 100; i++) {
        many[2 * i] = i * 37;
        many[2 * i + 1] = -i * 41;
    }
    // The feature is dropped and counted; the tile is still a tile, its layer empty.
    adv_mvt_init(&w, scratch, 64);
    adv_mvt_layer_begin(&w, "roads", keys, 2);
    adv_mvt_feature_line(&w, 1, tb, many, 100);
    adv_mvt_layer_end(&w);
    len = adv_mvt_finish(&w, &out);
    T_CHECK_MSG(len > 0 && w.dropped == 1, "len=%d dropped=%u", len, w.dropped);
    d_decode(out, (size_t)len, t);
    T_CHECK(!t->err && t->n_layers == 1 && t->l[0].n_features == 0);
    // Overflow at layer_end: each layer fits the layer half, the two together not the tile half:
    // the second layer is left out whole, with its feature counted.
    adv_mvt_init(&w, scratch, 256);
    for (int k = 0; k < 2; k++) {
        adv_mvt_layer_begin(&w, "roads", keys, 2);
        adv_mvt_feature_line(&w, 1, tb, many, 20);
        adv_mvt_layer_end(&w);
    }
    len = adv_mvt_finish(&w, &out);
    T_CHECK_MSG(len > 0 && w.dropped == 2, "len=%d dropped=%u", len, w.dropped);
    d_decode(out, (size_t)len, t);
    T_CHECK(!t->err && t->n_layers == 1 && t->l[0].n_features == 1);
    // ... and one of them alone does fit, so the refusal above was the second's.
    adv_mvt_init(&w, scratch, 256);
    adv_mvt_layer_begin(&w, "roads", keys, 2);
    adv_mvt_feature_line(&w, 1, tb, many, 20);
    adv_mvt_layer_end(&w);
    len = adv_mvt_finish(&w, &out);
    T_CHECK_MSG(len > 0 && len <= 128, "len=%d", len);
    d_decode(out, (size_t)len, t);
    T_CHECK(!t->err && t->l[0].n_features == 1 && t->l[0].f[0].n_pts == 20 && t->l[0].f[0].geom_ok);
    T_CHECK(t->l[0].f[0].xy[2 * 19] == 19 * 37 && t->l[0].f[0].xy[2 * 19 + 1] == -19 * 41);
}

// ---- the renderer ----------------------------------------------------------------------------

// The renderer's grown tile, spelled again here as the oracle for "which segments belong".
static void grown_bounds(int z, uint32_t x, uint32_t y, int64_t b[4]) {
    adv_pt mn, mx;
    adv_tile_bounds(z, x, y, &mn, &mx);
    int64_t dlat = ((int64_t)mx.lat - mn.lat) / 16, dlon = ((int64_t)mx.lon - mn.lon) / 16;
    b[0] = mn.lat - dlat;
    b[1] = mn.lon - dlon;
    b[2] = mx.lat + dlat;
    b[3] = mx.lon + dlon;
}

static bool seg_in(const adv_seg *s, const int64_t b[4]) {
    return !(s->max_lat < b[0] || s->min_lat > b[2] || s->max_lon < b[1] || s->min_lon > b[3]);
}

static int runs_of(uint16_t mask, int first[16]) {
    int n = 0;
    for (int bit = 0; bit < 16;) {
        if (!((mask >> bit) & 1)) {
            bit++;
            continue;
        }
        first[n++] = bit;
        while (bit < 16 && ((mask >> bit) & 1)) bit++;
    }
    return n;
}

// The mask every segment of the test grid gets, by its id: five patterns, cycling.
static uint16_t pattern_mask(uint32_t seg) {
    switch (seg % 5) {
    case 0: return 0xFFFF;
    case 1: return 0x00FF;
    case 2: return 0x0F00;
    case 3: return 0xF00F;
    default: return 0;
    }
}

#define FOOT_SEG 7   // one segment also walked: foot 0x00FF on top of its bike 0x0F00

static void build_coverage(const adv_graph *g, adv_cov *cov) {
    adv_cov_init(cov);
    adv_hit *h = calloc(g->h->n_segs, sizeof *h);
    uint32_t n = 0;
    for (uint32_t s = 0; s < g->h->n_segs; s++) {
        uint16_t m = pattern_mask(s);
        if (!m) continue;
        h[n].seg = s;
        h[n].mask = m;
        n++;
    }
    n = adv_hits_finish(h, n);
    float new_m[3] = { 0, 0, 0 };
    T_CHECK(adv_cov_merge(cov, g, h, n, ADV_T_BIKE, NULL, new_m) == CB_OK);
    adv_hit foot = { FOOT_SEG, 0x00FF, 0 };
    T_CHECK(adv_cov_merge(cov, g, &foot, 1, ADV_T_FOOT, NULL, new_m) == CB_OK);
    free(h);
}

typedef struct {
    const adv_graph *g;
    const adv_cov   *cov;
    const uint32_t  *owners;
    uint32_t         user;
    adv_tile_scratch s;
    d_tile          *t;
} ctx;

// Renders one tile with every layer and decodes it: its length, or a negative. render_u: with
// a feature per line below the shape zoom too (the grouped layers' reference).
static int render_g(ctx *c, int z, uint32_t x, uint32_t y, int type, bool with_cov, bool ungrouped) {
    adv_tile_req r = { .z = z, .x = x, .y = y, .type = type, .roads = true, .traveled = true, .untraveled = true, .unique = true,
                       .ungrouped = ungrouped };
    const uint8_t *out;
    int len = adv_tile_render(c->g, &r, c->user, with_cov ? c->cov : NULL, c->owners, &c->s, &out);
    if (len > 0) d_decode(out, (size_t)len, c->t);
    return len;
}
// The checks by segment id read the per-line reference (ungrouped): test_grouped holds the served,
// grouped tiles to it line for line.
static int render(ctx *c, int z, uint32_t x, uint32_t y, int type, bool with_cov) { return render_g(c, z, x, y, type, with_cov, true); }
static int render_u(ctx *c, int z, uint32_t x, uint32_t y, int type, bool with_cov) { return render_g(c, z, x, y, type, with_cov, true); }
static int render_grouped(ctx *c, int z, uint32_t x, uint32_t y, int type, bool with_cov) { return render_g(c, z, x, y, type, with_cov, false); }

// One z16 tile checked layer by layer against the oracle.
static void check_z16_tile(ctx *c, uint32_t x, uint32_t y, int type) {
    int len = render(c, 16, x, y, type, true);
    T_CHECK_MSG(len >= 0, "render %d", len);
    if (len < 0) return;
    d_tile *t = c->t;
    if (len == 0) return;   // off the grid
    T_CHECK(!t->err && t->n_layers == 4);
    const d_layer *roads = d_find(t, "roads"), *trav = d_find(t, "traveled"), *untrav = d_find(t, "untraveled"), *uniq = d_find(t, "unique");
    T_CHECK(roads && trav && untrav && uniq);
    if (!roads || !trav || !untrav || !uniq) return;
    T_CHECK(strcmp(t->l[0].name, "roads") == 0 && strcmp(t->l[1].name, "traveled") == 0 && strcmp(t->l[3].name, "unique") == 0);
    for (int i = 0; i < 4; i++) check_layer_sane(&t->l[i]);
    T_CHECK(trav->n_keys == 2 && strcmp(trav->keys[1], "t") == 0);
    int64_t b[4];
    grown_bounds(16, x, y, b);
    int expect_roads = 0, expect_trav = 0, expect_untrav = 0, expect_uniq = 0;
    for (uint32_t s = 0; s < c->g->h->n_segs; s++) {
        const adv_seg *sg = &c->g->segs[s];
        bool in = seg_in(sg, b);
        const d_feat *fr = d_feature_by_id(roads, s);
        T_CHECK_MSG((fr != NULL) == in, "seg %u in=%d roads=%d", s, in, fr != NULL);
        if (!in) continue;
        expect_roads++;
        T_CHECK(fr && fr->n_pts == 4 && tag_value(roads, fr, "c") == sg->hclass);
        double full = fr ? d_length(fr) : 0;
        // Every shape point where geo.h's exact projection puts it (the renderer projects the
        // first point exactly and the rest through a frame at it; rounding may differ by one).
        if (fr && fr->n_pts == 4) {
            adv_pt pts[ADV_SEG_MAX_PTS];
            int np = adv_seg_points(c->g, s, pts, ADV_SEG_MAX_PTS);
            T_CHECK(np == 4);
            for (int i = 0; i < np; i++) {
                double px, py;
                adv_tile_project(16, x, y, pts[i], &px, &py);
                T_CHECK_MSG(fabs(fr->xy[2 * i] - px) <= 1.0 && fabs(fr->xy[2 * i + 1] - py) <= 1.0,
                            "seg %u pt %d: (%d,%d) vs (%.2f,%.2f)", s, i, fr->xy[2 * i], fr->xy[2 * i + 1], px, py);
            }
        }
        // traveled: a run per block of set bits, each the right share of the length.
        uint16_t mask = adv_cov_mask(c->cov, s, type);
        int first[16], n = runs_of(mask, first);
        for (int k = 0; k < n; k++) {
            const d_feat *f = d_feature_by_id(trav, (uint64_t)s * 16 + (uint64_t)first[k]);
            T_CHECK_MSG(f != NULL, "seg %u run %d missing", s, first[k]);
            if (!f) continue;
            expect_trav++;
            int bits = 0;
            for (int bit = first[k]; bit < 16 && ((mask >> bit) & 1); bit++) bits++;
            double want = full * bits / 16.0, got = d_length(f);
            T_CHECK_MSG(fabs(got - want) <= 0.05 * want, "seg %u run %d: len %f want %f", s, first[k], got, want);
            T_CHECK(tag_value(trav, f, "c") == sg->hclass && tag_value(trav, f, "t") == type);
        }
        // untraveled: the complement's runs.
        uint16_t un = (uint16_t)(~mask & 0xFFFF);
        n = runs_of(un, first);
        for (int k = 0; k < n; k++) {
            const d_feat *f = d_feature_by_id(untrav, (uint64_t)s * 16 + (uint64_t)first[k]);
            T_CHECK_MSG(f != NULL, "seg %u untraveled run %d missing", s, first[k]);
            if (f) {
                expect_untrav++;
                int bits = 0;
                for (int bit = first[k]; bit < 16 && ((un >> bit) & 1); bit++) bits++;
                double want = full * bits / 16.0, got = d_length(f);
                T_CHECK_MSG(fabs(got - want) <= 0.05 * want, "seg %u untraveled run %d: len %f want %f", s, first[k], got, want);
            }
        }
        // unique: the owned ones.
        const d_feat *fu = d_feature_by_id(uniq, s);
        T_CHECK_MSG((fu != NULL) == (c->owners[s] == c->user), "seg %u owner %u unique=%d", s, c->owners[s], fu != NULL);
        if (fu) expect_uniq++;
    }
    T_CHECK_MSG(roads->n_features == expect_roads, "roads %d expect %d", roads->n_features, expect_roads);
    T_CHECK_MSG(trav->n_features == expect_trav, "traveled %d expect %d", trav->n_features, expect_trav);
    T_CHECK_MSG(untrav->n_features == expect_untrav, "untraveled %d expect %d", untrav->n_features, expect_untrav);
    T_CHECK_MSG(uniq->n_features == expect_uniq, "unique %d expect %d", uniq->n_features, expect_uniq);
}

static void tile_range(const adv_graph *g, int z, uint32_t *x0, uint32_t *y0, uint32_t *x1, uint32_t *y1) {
    adv_pt nw = { g->h->max_lat, g->h->min_lon }, se = { g->h->min_lat, g->h->max_lon };
    adv_tile_of(nw, z, x0, y0);
    adv_tile_of(se, z, x1, y1);
}

// EVERYONE'S TILE: hand-made words over the grid. Segment A (0) has one user with every
// sixteenth, segment B (1) three users on its first half, segment C (2) two bike users on its
// second half and nobody on foot, the rest nobody. In the combined view: traveled is A whole
// (n 1), B's first half (n 3), C's second half (n 2); untraveled is B's second half, C's first
// half and every other segment whole; unique is A alone. On foot, C is nobody's: untraveled whole.
static bool type_allows_test(uint8_t access, int type) {
    if (type == ADV_T_ALL) return access != 0;
    return (access & (type == ADV_T_BIKE ? ADV_ACC_BIKE : ADV_ACC_FOOT)) != 0;
}

static void test_everyone(d_tile *t) {
    fg_opts o = { .cols = 4, .rows = 3, .block_m = 120, .shape_m = 40, .regions = false };
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_mvt_all");
    adv_graph g;
    T_CHECK(fg_build(&o, path, &g) == 0);
    adv_all_seg *all = calloc(g.h->n_segs, sizeof *all);
    for (int ty = 0; ty < ADV_T_N; ty++) { atomic_store(&all[0].mask[ty], 0xFFFF); atomic_store(&all[0].users[ty], 1); }
    for (int ty = 0; ty < ADV_T_N; ty++) { atomic_store(&all[1].mask[ty], 0x00FF); atomic_store(&all[1].users[ty], 3); }
    atomic_store(&all[2].mask[ADV_T_BIKE], 0xFF00); atomic_store(&all[2].users[ADV_T_BIKE], 2);
    atomic_store(&all[2].mask[ADV_T_ALL], 0xFF00); atomic_store(&all[2].users[ADV_T_ALL], 2);
    ctx c = { .g = &g, .cov = NULL, .owners = NULL, .user = 0, .t = t };
    T_CHECK(adv_tile_scratch_init(&c.s, &g) == CB_OK);
    uint32_t x0, y0, x1, y1;
    tile_range(&g, 16, &x0, &y0, &x1, &y1);
    for (int type = 0; type < ADV_T_N; type++) {
        int seen_a = 0, seen_b = 0, seen_c = 0;
        for (uint32_t y = y0; y <= y1; y++)
            for (uint32_t x = x0; x <= x1; x++) {
                adv_tile_req r = { .z = 16, .x = x, .y = y, .type = type, .roads = true, .traveled = true, .untraveled = true, .unique = true, .all = all,
                                   .ungrouped = true };
                const uint8_t *out;
                int len = adv_tile_render(&g, &r, 0, NULL, NULL, &c.s, &out);
                T_CHECK(len >= 0);
                if (len <= 0) continue;
                d_decode(out, (size_t)len, t);
                T_CHECK(!t->err && t->n_layers == 4);
                const d_layer *roads = d_find(t, "roads"), *trav = d_find(t, "traveled"), *untrav = d_find(t, "untraveled"), *uniq = d_find(t, "unique");
                T_CHECK(roads && trav && untrav && uniq);
                if (!roads || !trav || !untrav || !uniq) continue;
                T_CHECK(trav->n_keys == 3 && strcmp(trav->keys[2], "n") == 0);
                for (int i = 0; i < 4; i++) check_layer_sane(&t->l[i]);
                int64_t b[4];
                grown_bounds(16, x, y, b);
                for (uint32_t sg = 0; sg < g.h->n_segs; sg++) {
                    if (!seg_in(&g.segs[sg], b)) continue;
                    uint16_t mask = atomic_load(&all[sg].mask[type]);
                    uint16_t users = atomic_load(&all[sg].users[type]);
                    const d_feat *fa = d_feature_by_id(trav, (uint64_t)sg * 16), *fb = d_feature_by_id(trav, (uint64_t)sg * 16 + 8);
                    const d_feat *ua = d_feature_by_id(untrav, (uint64_t)sg * 16), *ub = d_feature_by_id(untrav, (uint64_t)sg * 16 + 8);
                    const d_feat *un = d_feature_by_id(uniq, sg);
                    // A full mask is one run from bit 0; a half is one run from bit 0 or bit 8.
                    T_CHECK_MSG((fa != NULL) == ((mask & 1) != 0), "seg %u type %d traveled from 0", sg, type);
                    T_CHECK_MSG((fb != NULL) == (mask == 0xFF00), "seg %u type %d traveled from 8", sg, type);
                    T_CHECK((ua != NULL) == ((mask & 1) == 0 && type_allows_test(g.segs[sg].access, type)));
                    T_CHECK((ub != NULL) == (mask == 0x00FF));
                    T_CHECK_MSG((un != NULL) == (users == 1), "seg %u type %d unique", sg, type);
                    const d_feat *f = fa ? fa : fb;
                    if (f) {
                        T_CHECK_MSG(tag_value(trav, f, "n") == users, "seg %u n %d want %u", sg, tag_value(trav, f, "n"), users);
                        T_CHECK(tag_value(trav, f, "t") == type && tag_value(trav, f, "c") == g.segs[sg].hclass);
                    }
                    if (sg == 0 && fa) seen_a++;
                    if (sg == 1 && fa) seen_b++;
                    if (sg == 2 && fb) seen_c++;
                }
            }
        T_CHECK(seen_a > 0 && seen_b > 0);
        T_CHECK((seen_c > 0) == (type != ADV_T_FOOT));
    }
    // A low zoom: the cells are walked and only the majors drawn, as end points: a primary
    // segment somebody has shows, a residential one (A) does not.
    {
        uint32_t prim = UINT32_MAX;
        for (uint32_t sg = 0; sg < g.h->n_segs && prim == UINT32_MAX; sg++) if (g.segs[sg].hclass == ADV_HC_PRIMARY) prim = sg;
        T_CHECK(prim != UINT32_MAX && g.segs[0].hclass == ADV_HC_RESIDENTIAL);
        atomic_store(&all[prim].mask[ADV_T_ALL], 0xFFFF);
        atomic_store(&all[prim].users[ADV_T_ALL], 1);
        adv_tile_req r = { .z = 8, .type = ADV_T_ALL, .traveled = true, .unique = true, .all = all, .ungrouped = true };
        adv_pt mid = adv_seg_at(&g, 0, 0.5);
        adv_tile_of(mid, 8, &r.x, &r.y);
        const uint8_t *out;
        int len = adv_tile_render(&g, &r, 0, NULL, NULL, &c.s, &out);
        T_CHECK(len > 0);
        d_decode(out, (size_t)len, t);
        const d_layer *trav = d_find(t, "traveled"), *uniq = d_find(t, "unique");
        T_CHECK(trav && uniq);
        if (trav && uniq) {
            const d_feat *fp = d_feature_by_id(trav, (uint64_t)prim * 16);
            T_CHECK(fp && fp->n_pts == 2 && tag_value(trav, fp, "n") == 1);
            T_CHECK(d_feature_by_id(trav, 0) == NULL);
            T_CHECK(d_feature_by_id(uniq, prim) != NULL && d_feature_by_id(uniq, 0) == NULL);
        }
        // Two zooms up (z9, the roads layer's z11 rule): the residential street shows too.
        r.z = 9;
        adv_tile_of(mid, 9, &r.x, &r.y);
        len = adv_tile_render(&g, &r, 0, NULL, NULL, &c.s, &out);
        T_CHECK(len > 0);
        d_decode(out, (size_t)len, t);
        trav = d_find(t, "traveled");
        T_CHECK(trav && d_feature_by_id(trav, 0) != NULL && d_feature_by_id(trav, (uint64_t)prim * 16) != NULL);
    }
    adv_tile_scratch_free(&c.s);
    free(all);
    adv_graph_close(&g);
    unlink(path);
}

// THE GROUPED LAYERS ARE THE SAME LINES: at every zoom every tile of a grid, for each
// coverage view, a user's and everyone's, decodes to the same lines under the same tags whether
// its layers are grouped (a MULTILINESTRING per tag tuple) or not (a feature per line); and the
// grouped tile is the smaller.
typedef struct { uint64_t tags, h; int32_t x0, y0, x1, y1; int n; } line_key;
static int line_key_cmp(const void *a, const void *b) { return memcmp(a, b, sizeof(line_key)); }
static int layer_lines(const d_layer *l, line_key *out) {
    for (int i = 0; i < l->n_lines; i++) {
        const d_line *ln = &l->lines[i];
        const d_feat *f = &l->f[ln->feat];
        line_key k;
        memset(&k, 0, sizeof k);
        for (int t = 0; t < f->n_tags; t++) k.tags = k.tags * 1000003u + f->tags[2 * t] * 257u + (uint64_t)l->values[f->tags[2 * t + 1]] + 1;
        k.h = ln->h;
        k.x0 = ln->x0; k.y0 = ln->y0; k.x1 = ln->x1; k.y1 = ln->y1;
        k.n = ln->n;
        out[i] = k;
    }
    qsort(out, (size_t)l->n_lines, sizeof *out, line_key_cmp);
    return l->n_lines;
}
static void test_grouped(d_tile *t) {
    fg_opts o = { .cols = 10, .rows = 10, .block_m = 120, .shape_m = 40, .regions = true };
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_mvt_grouped");
    adv_graph g;
    T_CHECK(fg_build(&o, path, &g) == 0);
    adv_cov cov;
    build_coverage(&g, &cov);
    uint32_t *owners = calloc(g.h->n_segs, sizeof *owners);
    for (uint32_t s = 0; s < g.h->n_segs; s += 4) owners[s] = 42;
    adv_all_seg *all = calloc(g.h->n_segs, sizeof *all);
    for (uint32_t s = 0; s < g.h->n_segs; s++)
        for (int ty = 0; ty < ADV_T_N; ty++) {
            atomic_store(&all[s].mask[ty], (uint16_t)(s % 3 == 0 ? 0xFFFF : s % 3 == 1 ? 0x0FF0 : 0));
            atomic_store(&all[s].users[ty], (uint16_t)(s % 3 == 2 ? 0 : 1 + s % 5));
        }
    ctx c = { .g = &g, .cov = &cov, .owners = owners, .user = 42, .t = t };
    T_CHECK(adv_tile_scratch_init(&c.s, &g) == CB_OK);
    d_tile *u = d_tile_new();
    line_key *a = malloc(D_MAX_LINES * sizeof *a), *b = malloc(D_MAX_LINES * sizeof *b);
    int tiles = 0, lines = 0;
    size_t bytes_g = 0, bytes_u = 0;
    // From z4, where a 120 m street is under a unit of the extent and most lines are specks the
    // writer must drop in both encodings, past the shape zoom (whole shapes) to z16.
    for (int z = 4; z <= 16; z++)
        for (int everyone = 0; everyone < 2; everyone++)
            for (int type = 0; type < ADV_T_N; type++) {
                uint32_t x0, y0, x1, y1;
                tile_range(&g, z, &x0, &y0, &x1, &y1);
                for (uint32_t y = y0; y <= y1; y++)
                    for (uint32_t x = x0; x <= x1; x++) {
                        adv_tile_req r = { .z = z, .x = x, .y = y, .type = type, .roads = true, .traveled = true, .untraveled = true,
                                           .unique = true, .all = everyone ? all : NULL };
                        const uint8_t *out;
                        int lg = adv_tile_render(&g, &r, 42, everyone ? NULL : &cov, everyone ? NULL : owners, &c.s, &out);
                        T_CHECK(lg > 0);
                        if (lg <= 0) continue;
                        d_decode(out, (size_t)lg, t);
                        r.ungrouped = true;
                        int lu = adv_tile_render(&g, &r, 42, everyone ? NULL : &cov, everyone ? NULL : owners, &c.s, &out);
                        T_CHECK(lu > 0);
                        if (lu <= 0) continue;
                        d_decode(out, (size_t)lu, u);
                        T_CHECK(!t->err && !u->err && t->n_layers == 4 && u->n_layers == 4);
                        bytes_g += (size_t)lg;
                        bytes_u += (size_t)lu;
                        for (int i = 0; i < 4 && i < t->n_layers; i++) {
                            check_layer_sane(&t->l[i]);
                            T_CHECK(strcmp(t->l[i].name, u->l[i].name) == 0);
                            int na = layer_lines(&t->l[i], a), nb = layer_lines(&u->l[i], b);
                            T_CHECK_MSG(na == nb, "z%d %u/%u %s: %d grouped lines, %d ungrouped", z, x, y, t->l[i].name, na, nb);
                            if (na == nb) T_CHECK_MSG(memcmp(a, b, (size_t)na * sizeof *a) == 0, "z%d %u/%u %s: the lines differ", z, x, y, t->l[i].name);
                            for (int k = 0; k < t->l[i].n_features; k++) T_CHECK(!t->l[i].f[k].has_id);
                            lines += na;
                        }
                        tiles++;
                    }
            }
    T_CHECK(tiles > 20 && lines > 1000);
    T_CHECK_MSG(bytes_g * 10 < bytes_u * 7, "grouped %zu bytes, ungrouped %zu", bytes_g, bytes_u);
    printf("grouped layers: %d tiles, %d lines, %zu bytes grouped vs %zu a feature each\n", tiles, lines, bytes_g, bytes_u);
    for (int k = 0; k < D_MAX_LAYERS; k++) { free(u->l[k].f); free(u->l[k].lines); }
    free(u);
    free(a);
    free(b);
    adv_tile_scratch_free(&c.s);
    adv_cov_free(&cov);
    free(owners);
    free(all);
    adv_graph_close(&g);
    unlink(path);
}

// THE TILE FRAME (geo.h): from its zoom up, a point of the tile, its grown edge or 2 km past it
// projects within 0.005 extent units of the exact Mercator at 41 N and 0.01 at 70 N (measured:
// 0.0010 and 0.0018).
static void test_tile_frame(void) {
    static const double lats[2] = { 40.75, 70.0 }, bound[2] = { 0.005, 0.01 };
    double worst[2] = { 0, 0 };
    uint64_t rng = 88172645463325252ull;
    for (int k = 0; k < 2; k++)
        for (int z = ADV_TILE_FRAME_MIN_ZOOM; z <= ADV_TILE_MAX_ZOOM; z++) {
            adv_pt c = { (int32_t)(lats[k] * ADV_DEG), (int32_t)(-73.98 * ADV_DEG) };
            uint32_t tx, ty;
            adv_tile_of(c, z, &tx, &ty);
            adv_tile_frame f;
            adv_tile_frame_init(&f, z, tx, ty);
            adv_pt mn, mx;
            adv_tile_bounds(z, tx, ty, &mn, &mx);
            // The tile grown by a sixteenth and by 2 km (0.018 degrees of latitude, 0.05 of longitude at 70 N).
            double glat = (mx.lat - mn.lat) / 16.0 + 0.018 * ADV_DEG, glon = (mx.lon - mn.lon) / 16.0 + 0.053 * ADV_DEG;
            for (int i = 0; i < 2000; i++) {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                double u = (double)(rng & 0xFFFFFF) / 0xFFFFFF, v = (double)((rng >> 24) & 0xFFFFFF) / 0xFFFFFF;
                adv_pt p = { (int32_t)(mn.lat - glat + u * (mx.lat - mn.lat + 2 * glat)), (int32_t)(mn.lon - glon + v * (mx.lon - mn.lon + 2 * glon)) };
                double ex, ey, fx, fy;
                adv_tile_project(z, tx, ty, p, &ex, &ey);
                adv_tile_frame_project(&f, p, &fx, &fy);
                double e = hypot(fx - ex, fy - ey);
                if (e > worst[k]) worst[k] = e;
            }
        }
    T_CHECK_MSG(worst[0] < bound[0], "41 N: %f units", worst[0]);
    T_CHECK_MSG(worst[1] < bound[1], "70 N: %f units", worst[1]);
    printf("tile frame: worst %.5f units at 41 N, %.5f at 70 N (z%d..z%d)\n", worst[0], worst[1], ADV_TILE_FRAME_MIN_ZOOM, ADV_TILE_MAX_ZOOM);
}

// BELOW THE FRAME'S ZOOM the end points are projected exactly: a grid at 60 N far from the
// middle latitude of its z2 tile, where the frame's series would be off by tens of units.
static void test_low_zoom_exact(d_tile *t) {
    fg_opts o = { .cols = 3, .rows = 3, .block_m = 1900, .shape_m = -1, .sw = { 60 * ADV_DEG, 10 * ADV_DEG } };
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_mvt_low");
    adv_graph g;
    T_CHECK(fg_build(&o, path, &g) == 0);
    adv_tile_scratch s;
    T_CHECK(adv_tile_scratch_init(&s, &g) == CB_OK);
    adv_tile_req r = { .z = 2, .type = ADV_T_ALL, .roads = true, .ungrouped = true };
    adv_pt p0 = { g.segs[0].lat, g.segs[0].lon };
    adv_tile_of(p0, 2, &r.x, &r.y);
    const uint8_t *out;
    int len = adv_tile_render(&g, &r, 0, NULL, NULL, &s, &out);
    T_CHECK(len > 0);
    d_decode(out, (size_t)len, t);
    const d_layer *roads = d_find(t, "roads");
    int checked = 0;
    double worst = 0;
    for (int i = 0; roads && i < roads->n_features; i++) {
        const d_feat *f = &roads->f[i];
        if (!f->has_id || f->id >= g.h->n_segs || f->n_pts != 2) continue;
        adv_pt pts[ADV_SEG_MAX_PTS];
        int n = adv_seg_points(&g, (uint32_t)f->id, pts, ADV_SEG_MAX_PTS);
        double ax, ay, bx, by;
        adv_tile_project(2, r.x, r.y, pts[0], &ax, &ay);
        adv_tile_project(2, r.x, r.y, pts[n - 1], &bx, &by);
        double e = fmax(hypot(f->xy[0] - ax, f->xy[1] - ay), hypot(f->xy[2] - bx, f->xy[3] - by));
        if (e > worst) worst = e;
        checked++;
    }
    T_CHECK_MSG(checked > 0 && worst <= 0.75, "z2: %d lines checked, worst %f units from exact", checked, worst);
    adv_tile_scratch_free(&s);
    adv_graph_close(&g);
    unlink(path);
}

// A GROUPED LAYER'S LIMITS: lines past the line or point room, tag tuples past ADV_MVT_GROUPS
// and a group whose geometry could pass its buffer are dropped and counted, and what fits is
// still a valid tile.
static void test_grouped_caps(d_tile *t) {
    static uint8_t buf[1 << 20];
    static const char *const keys[] = { "c", "n" };
    adv_mvt_lines l;
    T_CHECK(adv_mvt_lines_init(&l, 4, 6, 128) == CB_OK);
    adv_mvt w;
    adv_mvt_init(&w, buf, sizeof buf);
    adv_mvt_layer_begin_grouped(&w, "g", keys, 2, &l);
    int32_t xy[6] = { 0, 0, 10, 0, 10, 10 };
    int tags[2] = { 1, 2 };
    adv_mvt_feature_line(&w, 1, tags, xy, 2);   // 2 points
    adv_mvt_feature_line(&w, 2, tags, xy, 3);   // 3: 5 of 6
    adv_mvt_feature_line(&w, 3, tags, xy, 2);   // 7 points: past the point room
    T_CHECK_MSG((w.dropped) == (1), "%u", (unsigned)(w.dropped));
    adv_mvt_feature_line(&w, 4, tags, xy, 1 + 0);   // one point: no line, nothing kept, nothing dropped
    T_CHECK_MSG((w.dropped) == (1), "%u", (unsigned)(w.dropped));
    adv_mvt_layer_end(&w);
    const uint8_t *out;
    int len = adv_mvt_finish(&w, &out);
    T_CHECK(len > 0);
    d_decode(out, (size_t)len, t);
    T_CHECK_MSG(!t->err && t->n_layers == 1 && t->l[0].n_features == 1 && t->l[0].n_lines == 2, "err %d layers %d features %d lines %d", t->err, t->n_layers, t->l[0].n_features, t->l[0].n_lines);
    check_layer_sane(&t->l[0]);

    // The lines' room and the points' room; then a geometry over its buffer.
    adv_mvt_init(&w, buf, sizeof buf);
    adv_mvt_layer_begin_grouped(&w, "g", keys, 2, &l);
    for (int i = 0; i < 5; i++) adv_mvt_feature_line(&w, (uint64_t)i, tags, xy, 1 + 1);
    T_CHECK_MSG((w.dropped) == (2), "%u", (unsigned)(w.dropped));   // the fourth line passes the 6 points, the fifth the 4 lines
    adv_mvt_layer_end(&w);
    T_CHECK(adv_mvt_finish(&w, &out) > 0);
    adv_mvt_lines_free(&l);
    T_CHECK(adv_mvt_lines_init(&l, 64, 128, 40) == CB_OK);
    adv_mvt_init(&w, buf, sizeof buf);
    adv_mvt_layer_begin_grouped(&w, "g", keys, 2, &l);
    for (int i = 0; i < 3; i++) adv_mvt_feature_line(&w, (uint64_t)i, tags, xy, 2);   // 3 x (2 x 10 + 10) = 90 > 40
    tags[0] = 5;
    adv_mvt_feature_line(&w, 9, tags, xy, 2);   // another group, 30 <= 40: kept
    adv_mvt_layer_end(&w);
    T_CHECK_MSG((w.dropped) == (3), "%u", (unsigned)(w.dropped));
    len = adv_mvt_finish(&w, &out);
    T_CHECK(len > 0);
    d_decode(out, (size_t)len, t);
    T_CHECK(!t->err && t->l[0].n_features == 1 && t->l[0].n_lines == 1);
    adv_mvt_lines_free(&l);
    // Groups past ADV_MVT_GROUPS.
    T_CHECK(adv_mvt_lines_init(&l, ADV_MVT_GROUPS + 8, 2 * (ADV_MVT_GROUPS + 8), 1 << 16) == CB_OK);
    adv_mvt_init(&w, buf, sizeof buf);
    adv_mvt_layer_begin_grouped(&w, "g", keys, 2, &l);
    for (int i = 0; i < ADV_MVT_GROUPS + 5; i++) {
        int tg[2] = { i % 256, i / 256 };
        adv_mvt_feature_line(&w, (uint64_t)i, tg, xy, 2);
    }
    T_CHECK_MSG((w.dropped) == (5), "%u", (unsigned)(w.dropped));
    adv_mvt_layer_end(&w);
    len = adv_mvt_finish(&w, &out);
    T_CHECK(len > 0);
    d_decode(out, (size_t)len, t);
    T_CHECK(!t->err && t->l[0].n_features == ADV_MVT_GROUPS);
    adv_mvt_lines_free(&l);
}

static void test_renderer(d_tile *t) {
    fg_opts o = { .cols = 8, .rows = 8, .block_m = 120, .shape_m = 40, .regions = true };
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_mvt");
    adv_graph g;
    T_CHECK(fg_build(&o, path, &g) == 0);
    T_CHECK(g.h->n_segs == 144);
    adv_cov cov;
    build_coverage(&g, &cov);
    uint32_t *owners = calloc(g.h->n_segs, sizeof *owners);
    for (uint32_t s = 0; s < g.h->n_segs; s++) {
        if (s % 9 == 0) owners[s] = 42;        // ours
        else if (s % 9 == 1) owners[s] = 43;   // someone else's
        else if (s % 9 == 2) owners[s] = ADV_OWNER_MANY;
    }
    ctx c = { .g = &g, .cov = &cov, .owners = owners, .user = 42, .t = t };
    T_CHECK(adv_tile_scratch_init(&c.s, &g) == CB_OK);

    // Every z16 tile over the grid, for each coverage view.
    uint32_t x0, y0, x1, y1;
    tile_range(&g, 16, &x0, &y0, &x1, &y1);
    T_CHECK((x1 - x0 + 1) * (y1 - y0 + 1) >= 4);
    size_t max_len = 0;
    for (int type = 0; type < ADV_T_N; type++)
        for (uint32_t y = y0; y <= y1; y++)
            for (uint32_t x = x0; x <= x1; x++) check_z16_tile(&c, x, y, type);
    // The segment walked on foot: in the foot view its traveled is the foot half alone.
    {
        adv_pt mid = adv_seg_at(&g, FOOT_SEG, 0.5);
        uint32_t tx, ty;
        adv_tile_of(mid, 16, &tx, &ty);
        T_CHECK(render(&c, 16, tx, ty, ADV_T_FOOT, true) > 0);
        const d_layer *trav = d_find(t, "traveled");
        T_CHECK(trav && d_feature_by_id(trav, FOOT_SEG * 16 + 0) && !d_feature_by_id(trav, FOOT_SEG * 16 + 8));
        T_CHECK(render(&c, 16, tx, ty, ADV_T_BIKE, true) > 0);
        trav = d_find(t, "traveled");
        T_CHECK(trav && d_feature_by_id(trav, FOOT_SEG * 16 + 8) && !d_feature_by_id(trav, FOOT_SEG * 16 + 0));
        T_CHECK(render(&c, 16, tx, ty, ADV_T_ALL, true) > 0);
        trav = d_find(t, "traveled");
        T_CHECK(trav && d_feature_by_id(trav, FOOT_SEG * 16 + 0) && d_feature_by_id(trav, FOOT_SEG * 16 + 0)->n_pts == 4);
        const d_layer *un = d_find(t, "untraveled");
        T_CHECK(un && d_feature_by_id(un, FOOT_SEG * 16 + 12) && !d_feature_by_id(un, FOOT_SEG * 16 + 0));
    }
    // No coverage: traveled empty, untraveled whole.
    {
        int len = render(&c, 16, (x0 + x1) / 2, (y0 + y1) / 2, ADV_T_ALL, false);
        T_CHECK(len > 0 && !t->err);
        const d_layer *trav = d_find(t, "traveled"), *un = d_find(t, "untraveled"), *roads = d_find(t, "roads");
        T_CHECK(trav && trav->n_features == 0);
        T_CHECK(un && roads && un->n_features == roads->n_features);
        for (int i = 0; i < un->n_features; i++) T_CHECK(un->f[i].id % 16 == 0 && un->f[i].n_pts == 4);
    }
    // A tile off the grid: zero bytes, no error.
    {
        int len = render(&c, 16, x0 + 1000, y0, ADV_T_ALL, true);
        T_CHECK_MSG(len == 0, "off-grid len=%d", len);
    }
    // Refusals.
    {
        adv_tile_req r = { .z = 19, .x = 0, .y = 0, .type = 0, .roads = true };
        const uint8_t *out;
        T_CHECK(adv_tile_render(&g, &r, 42, &cov, owners, &c.s, &out) == CB_E_INVALID);
        r.z = 3;
        r.x = 8;
        T_CHECK(adv_tile_render(&g, &r, 42, &cov, owners, &c.s, &out) == CB_E_INVALID);
        r.x = 7;
        r.y = 8;
        T_CHECK(adv_tile_render(&g, &r, 42, &cov, owners, &c.s, &out) == CB_E_INVALID);
        r.y = 7;
        r.type = 3;
        T_CHECK(adv_tile_render(&g, &r, 42, &cov, owners, &c.s, &out) == CB_E_INVALID);
        r.type = 0;
        r.z = -1;
        T_CHECK(adv_tile_render(&g, &r, 42, &cov, owners, &c.s, &out) == CB_E_INVALID);
        r.z = 3;
        T_CHECK(adv_tile_render(&g, &r, 42, &cov, owners, &c.s, &out) >= 0);
    }
    // z12: end points only, in every layer; the whole grid is a handful of tiles.
    {
        tile_range(&g, 12, &x0, &y0, &x1, &y1);
        int features = 0;
        for (uint32_t y = y0; y <= y1; y++)
            for (uint32_t x = x0; x <= x1; x++) {
                int len = render_grouped(&c, 12, x, y, ADV_T_ALL, true);
                T_CHECK(len > 0 && !t->err && t->n_layers == 4);
                for (int i = 0; i < t->n_layers; i++) {
                    check_layer_sane(&t->l[i]);
                    for (int k = 0; k < t->l[i].n_features; k++) {
                        // Grouped: no ids, and every line of the feature its two end points.
                        T_CHECK(!t->l[i].f[k].has_id);
                        T_CHECK_MSG(t->l[i].f[k].n_pts == 2 * t->l[i].f[k].n_lines, "z12 %s pts %d lines %d", t->l[i].name,
                                    t->l[i].f[k].n_pts, t->l[i].f[k].n_lines);
                        features++;
                    }
                }
                const d_layer *un = d_find(t, "untraveled");
                T_CHECK(un && un->n_features > 0);   // z12 is above the floor
            }
        T_CHECK(features > 0);
    }
    // z10: majors only (the grid's x=0 street is PRIMARY, the rest RESIDENTIAL); untraveled
    // present and empty; traveled still drawn.
    {
        tile_range(&g, 10, &x0, &y0, &x1, &y1);
        int primaries = 0, traveled = 0;
        for (uint32_t y = y0; y <= y1; y++)
            for (uint32_t x = x0; x <= x1; x++) {
                int len = render_u(&c, 10, x, y, ADV_T_ALL, true);
                T_CHECK(len > 0 && !t->err && t->n_layers == 4);
                const d_layer *roads = d_find(t, "roads"), *un = d_find(t, "untraveled"), *trav = d_find(t, "traveled");
                T_CHECK(roads && un && trav);
                if (!roads || !un || !trav) continue;
                for (int k = 0; k < roads->n_features; k++) {
                    T_CHECK(tag_value(roads, &roads->f[k], "c") == ADV_HC_PRIMARY);
                    T_CHECK(g.segs[roads->f[k].id].hclass == ADV_HC_PRIMARY);
                    primaries++;
                }
                T_CHECK_MSG(un->n_features == 0, "z10 untraveled %d", un->n_features);
                traveled += trav->n_features;
            }
        T_CHECK_MSG(primaries == o.rows, "primaries=%d", primaries);
        T_CHECK(traveled > 0);
    }
    // z14 over the whole grid: every segment in some tile's roads layer.
    {
        tile_range(&g, 14, &x0, &y0, &x1, &y1);
        uint8_t *seen = calloc(g.h->n_segs, 1);
        for (uint32_t y = y0; y <= y1; y++)
            for (uint32_t x = x0; x <= x1; x++) {
                int len = render(&c, 14, x, y, ADV_T_ALL, true);
                T_CHECK(len > 0 && !t->err);
                if ((size_t)len > max_len) max_len = (size_t)len;
                const d_layer *roads = d_find(t, "roads");
                T_CHECK(roads != NULL);
                if (!roads) continue;
                check_layer_sane(roads);
                for (int k = 0; k < roads->n_features; k++)
                    if (roads->f[k].id < g.h->n_segs) seen[roads->f[k].id] = 1;
            }
        int missing = 0;
        for (uint32_t s = 0; s < g.h->n_segs; s++) missing += !seen[s];
        T_CHECK_MSG(missing == 0, "%d segments in no z14 tile", missing);
        free(seen);
        printf("z14 tile over the 8x8 grid: up to %zu bytes\n", max_len);
    }
    adv_tile_scratch_free(&c.s);
    adv_cov_free(&cov);
    free(owners);
    adv_graph_close(&g);
    unlink(path);
}

static void test_parse(void) {
    int z;
    uint32_t x, y;
    T_CHECK(adv_tile_parse_zxy("14/4823/6160", &z, &x, &y) && z == 14 && x == 4823 && y == 6160);
    T_CHECK(adv_tile_parse_zxy("0/0/0", &z, &x, &y) && z == 0 && x == 0 && y == 0);
    T_CHECK(adv_tile_parse_zxy("18/262143/262143", &z, &x, &y) && z == 18 && x == 262143 && y == 262143);
    T_CHECK(!adv_tile_parse_zxy("18/262144/0", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("18/0/262144", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("19/0/0", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("0/1/0", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("14/4823", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("14/4823/6160/1", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("14/4823/6160.mvt", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("14//6160", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("/14/4823/6160", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("/0/0", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("0//0", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("0/0/", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("//", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("14/4823/6160/", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("-1/0/0", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("+14/4823/6160", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("14/ 4823/6160", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("0x1/0/0", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("14/4823/61600000000000000000000000", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("14/4823/00000000000000000000000000000000000006160", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy("14/4823/18446744073709551616", &z, &x, &y));   // 2^64: wraps a naive reader
    T_CHECK(!adv_tile_parse_zxy("garbage", &z, &x, &y));
    T_CHECK(!adv_tile_parse_zxy(NULL, &z, &x, &y));
}

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

static void test_timing(void) {
    // A dense grid: 40x40 blocks of 60 m with a shape point every 20 m, 3280 segments of 4
    // points; a z14 tile at this latitude is 1.8 km wide and holds about 1800 of them.
    fg_opts o = { .cols = 40, .rows = 40, .block_m = 60, .shape_m = 20, .regions = true };
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_mvt_big");
    adv_graph g;
    T_CHECK(fg_build(&o, path, &g) == 0);
    T_CHECK_MSG(g.h->n_segs == 3280, "n_segs=%u", g.h->n_segs);
    for (uint32_t j = 0; j < g.h->cells_h; j++)
        for (uint32_t i = 0; i < g.h->cells_w; i++) {
            uint32_t n;
            adv_cell_list(&g, i, j, &n);
            T_CHECK(n <= ADV_SEG_MAX_PER_CELL);
        }
    adv_cov cov;
    adv_cov_init(&cov);
    adv_hit *h = calloc(2000, sizeof *h);
    for (uint32_t k = 0; k < 2000; k++) {
        h[k].seg = (k * 7919u) % g.h->n_segs;
        h[k].mask = (uint16_t)(k % 3 == 0 ? 0xFFFF : k % 3 == 1 ? 0x00FF : 0x0FF0);
    }
    uint32_t n = adv_hits_finish(h, 2000);
    float new_m[3] = { 0, 0, 0 };
    T_CHECK(adv_cov_merge(&cov, &g, h, n, ADV_T_BIKE, NULL, new_m) == CB_OK);
    uint32_t *owners = calloc(g.h->n_segs, sizeof *owners);
    for (uint32_t s = 0; s < g.h->n_segs; s += 3) owners[s] = 42;
    adv_tile_scratch s;
    T_CHECK(adv_tile_scratch_init(&s, &g) == CB_OK);
    adv_pt centre = { (g.h->min_lat + g.h->max_lat) / 2, (g.h->min_lon + g.h->max_lon) / 2 };
    adv_tile_req r = { .z = 14, .type = ADV_T_ALL, .roads = true, .traveled = true, .untraveled = true, .unique = true };
    adv_tile_of(centre, 14, &r.x, &r.y);
    enum { N = 200 };
    int64_t us[N], cpu[N];
    int len = 0, roads = 0, trav = 0, untrav = 0;
    for (int i = 0; i < N; i++) {
        const uint8_t *out;
        int64_t t0 = cb_mono_us(), c0 = cb_thread_cpu_us();
        len = adv_tile_render(&g, &r, 42, &cov, owners, &s, &out);
        cpu[i] = cb_thread_cpu_us() - c0;
        us[i] = cb_mono_us() - t0;
        T_CHECK(len > 0);
        if (i == 0) {
            d_tile *t = d_tile_new();
            d_decode(out, (size_t)len, t);
            T_CHECK(!t->err && t->n_layers == 4);
            roads = t->l[0].n_lines;   // lines: the served tile groups them into a feature per tag tuple
            trav = t->l[1].n_lines;
            untrav = t->l[2].n_lines;
            for (int k = 0; k < 4; k++) check_layer_sane(&t->l[k]);
            for (int k = 0; k < 4; k++) { free(t->l[k].f); free(t->l[k].lines); }
            free(t);
        }
    }
    double mean = 0, mean_cpu = 0;
    for (int i = 0; i < N; i++) {
        mean += (double)us[i];
        mean_cpu += (double)cpu[i];
    }
    mean /= N;
    mean_cpu /= N;
    qsort(us, N, sizeof us[0], cmp_i64);
    printf("z14 tile %d/%u/%u: %d bytes, %d roads %d traveled %d untraveled; %d renders mean %.0f us p99 %lld us (cpu mean %.0f us)\n",
           r.z, r.x, r.y, len, roads, trav, untrav, N, mean, (long long)us[N * 99 / 100], mean_cpu);
    T_CHECK_MSG(roads > 1000, "roads=%d", roads);
    // CPU time, not wall: a loaded build machine must not fail a correctness suite. The
    // sanitizers slow the render about 2.6x, so the instrumented lanes get a wider bound.
#ifdef ADV_INSTRUMENTED
    T_CHECK_MSG(mean_cpu < 8000, "cpu mean %.0f us", mean_cpu);
#else
    T_CHECK_MSG(mean_cpu < 2000, "cpu mean %.0f us", mean_cpu);
#endif
    adv_tile_scratch_free(&s);
    adv_cov_free(&cov);
    free(owners);
    free(h);
    adv_graph_close(&g);
    unlink(path);
}

int main(void) {
    d_tile *t = d_tile_new();
    test_tile_frame();
    test_low_zoom_exact(t);
    test_writer(t);
    test_renderer(t);
    test_everyone(t);
    test_grouped(t);
    test_grouped_caps(t);
    test_parse();
    test_timing();
    for (int i = 0; i < D_MAX_LAYERS; i++) { free(t->l[i].f); free(t->l[i].lines); }
    free(t);
    return t_done("unit_mvt");
}
