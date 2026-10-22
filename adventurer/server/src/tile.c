// tile.c - mvt.h's renderer: the tile's cells of the graph, walked once per layer, into the
// writer.
//
// THE WALK is the cell range the grown tile covers, every segment each cell lists, each segment
// once (the stamp: a segment listed by several cells is stamped on first sight and skipped
// after) and only if its bounding box meets the grown tile. Each layer walks again rather than
// keeping a list of the segments seen: the writer builds one layer at a time, a list would need
// scratch sized by cells times the cell cap, and the cell lists are a few hundred ids at the
// zooms that matter, so the second walk costs less than the memory it would save.
//
// THE GROWN TILE is the tile plus a sixteenth on each side: a line that crosses the border is
// drawn whole in both tiles and the renderer clips it, so no seam shows where a segment leaves
// one tile and enters the next.
//
// SHAPES are projected once per segment into extent coordinates (doubles, with the cumulative
// projected length at each point); every feature of the segment, whole or a run of sixteenths,
// is cut from that. Mercator is conformal, so a fraction of the projected length is the same
// fraction of the ground length to well under the width of a line at any zoom drawn here.
#include "mvt.h"

#include "util/err.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TILE_BUFFER_DIV 16     // the grown tile: 1/16 of the tile each side
#define SPECK_UNITS     1.5    // below ADV_TILE_SHAPE_ZOOM a shorter road is a sub-pixel speck

static const char *const KEYS_C[] = { "c" };
static const char *const KEYS_CT[] = { "c", "t" };
static const char *const KEYS_CTN[] = { "c", "t", "n" };

int adv_tile_scratch_init(adv_tile_scratch *s, const adv_graph *g) {
    memset(s, 0, sizeof *s);
    s->cap = ADV_TILE_SCRATCH;
    s->n_segs = g->h->n_segs;
    s->bytes = malloc(s->cap);
    // calloc(0) may return NULL, which would read as no memory: a graph with no segment still
    // gets a scratch it can render empty tiles from.
    s->stamp = calloc(s->n_segs ? s->n_segs : 1, sizeof *s->stamp);
    if (!s->bytes || !s->stamp || adv_mvt_lines_init(&s->lines, ADV_TILE_GROUP_LINES, ADV_TILE_GROUP_PTS, ADV_TILE_SCRATCH / 2) != CB_OK) {
        adv_tile_scratch_free(s);
        return CB_E_NOMEM;
    }
    return CB_OK;
}

void adv_tile_scratch_free(adv_tile_scratch *s) {
    free(s->bytes);
    free(s->stamp);
    adv_mvt_lines_free(&s->lines);
    memset(s, 0, sizeof *s);
}

// ---- the walk --------------------------------------------------------------------------------

typedef struct {
    const adv_graph  *g;
    adv_tile_scratch *s;
    int64_t  min_lat, min_lon, max_lat, max_lon;   // the grown tile (int64: a z0 tile grown overflows int32)
    uint32_t i0, j0, i1, j1;                       // the cell range
    uint32_t i, j, k, n;                           // the cell in hand and the index into its list
    const uint32_t *list;
    bool     done;
} walk;

static void walk_cell(walk *w) {
    w->list = adv_cell_list(w->g, w->i, w->j, &w->n);
    // open checked every cell against the file, so this cap is a second line, not a hope.
    if (w->n > ADV_SEG_MAX_PER_CELL) w->n = ADV_SEG_MAX_PER_CELL;
    w->k = 0;
}

static void walk_begin(walk *w) {
    adv_tile_scratch *s = w->s;
    // A stamp equal to gen means "seen in this walk". gen wraps once in 4G walks; every stamp is
    // then cleared so no stale stamp can equal the new gen.
    if (++s->gen == 0) {
        memset(s->stamp, 0, (size_t)s->n_segs * sizeof *s->stamp);
        s->gen = 1;
    }
    w->i = w->i0;
    w->j = w->j0;
    w->done = false;
    walk_cell(w);
}

// The next segment of the walk into *seg: false at the end. Bounded by the cell range times the
// cell cap: every cell is visited once, every listed id read once.
static bool walk_next(walk *w, uint32_t *seg) {
    while (!w->done) {
        if (w->k >= w->n) {
            if (w->i < w->i1) w->i++;
            else if (w->j < w->j1) {
                w->i = w->i0;
                w->j++;
            } else {
                w->done = true;
                break;
            }
            walk_cell(w);
            continue;
        }
        uint32_t id = w->list[w->k++];
        if (id >= w->s->n_segs) continue;   // the file's indexes were checked at open; a reader still never faults
        if (w->s->stamp[id] == w->s->gen) continue;
        w->s->stamp[id] = w->s->gen;
        const adv_seg *sg = &w->g->segs[id];
        if (sg->max_lat < w->min_lat || sg->min_lat > w->max_lat || sg->max_lon < w->min_lon || sg->min_lon > w->max_lon) continue;
        *seg = id;
        return true;
    }
    return false;
}

// ---- shapes ----------------------------------------------------------------------------------

typedef struct {
    int    n;
    double px[ADV_SEG_MAX_PTS], py[ADV_SEG_MAX_PTS];
    double cum[ADV_SEG_MAX_PTS];   // projected length from the first point to this one
} shape;

// The shape projected: the first point exactly (geo.h), the rest through a frame at it.
// Mercator x is linear in longitude, so dx is exact. Mercator y(lat) is taken to its third
// derivative at the first point: a segment spans at most ADV_SEG_MAX_LEN_M = 2 km, 3.2e-4 rad
// of latitude, and the fourth-order remainder is below 1e-3 extent units at z18 up to 80 N
// (the fourth derivative grows as sec^5; at 85, Mercator's own edge, it would reach 0.3
// units). One sin and one log per segment instead of one per point: the exact projection
// was half of a tile's time before this.
//
// BELOW THE SHAPE ZOOM a line is drawn as its two end points, so its runs (a mask's sixteenths)
// are placed along that chord too, and nothing past the ends is needed: the last point is the
// node the segment ends at (the importer puts node_b at the last shape point; graph.c checks it
// at open), and both project through the tile's frame (geo.h adv_tile_frame: no transcendental
// per segment; exactly below its zoom).
static void shape_load(const adv_graph *g, const adv_tile_req *r, adv_tile_scratch *s, const adv_tile_frame *tf, bool ends_only,
                       uint32_t seg, shape *sh) {
    if (ends_only) {
        const adv_seg *sg = &g->segs[seg];
        if (sg->n_pts < 2) { sh->n = 0; return; }
        adv_pt a = { sg->lat, sg->lon }, b = { g->nodes[sg->node_b].lat, g->nodes[sg->node_b].lon };
        if (r->z >= ADV_TILE_FRAME_MIN_ZOOM) {
            adv_tile_frame_project(tf, a, &sh->px[0], &sh->py[0]);
            adv_tile_frame_project(tf, b, &sh->px[1], &sh->py[1]);
        } else {
            adv_tile_project(r->z, r->x, r->y, a, &sh->px[0], &sh->py[0]);
            adv_tile_project(r->z, r->x, r->y, b, &sh->px[1], &sh->py[1]);
        }
        sh->n = 2;
        sh->cum[0] = 0;
        sh->cum[1] = hypot(sh->px[1] - sh->px[0], sh->py[1] - sh->py[0]);
        return;
    }
    int n = adv_seg_points(g, seg, s->pts, ADV_SEG_MAX_PTS);
    sh->n = n < 2 ? 0 : n;   // the file holds 2..ADV_SEG_MAX_PTS; anything else draws nothing
    if (sh->n == 0) return;
    adv_tile_project(r->z, r->x, r->y, s->pts[0], &sh->px[0], &sh->py[0]);
    sh->cum[0] = 0;
    double scale = (double)(1u << r->z) * ADV_MVT_EXTENT;   // extent units across the world
    double ux = scale / (360.0 * ADV_DEG);                   // per int32 unit of longitude
    double rad = M_PI / 180.0 / ADV_DEG;                     // radians per int32 unit of latitude
    double phi = (double)s->pts[0].lat * rad;
    double sec = 1.0 / cos(phi), tn = tan(phi);
    // y = 1/2 - asinh(tan phi) / 2pi: y' = -sec / 2pi, y'' = y' tan, y''' = y' (1 + 2 tan^2).
    double k = -scale / (2.0 * M_PI) * sec;
    double c1 = k, c2 = k * tn / 2.0, c3 = k * (1.0 + 2.0 * tn * tn) / 6.0;
    for (int i = 1; i < sh->n; i++) {
        double d = ((double)s->pts[i].lat - s->pts[0].lat) * rad;
        sh->px[i] = sh->px[0] + ((double)s->pts[i].lon - s->pts[0].lon) * ux;
        sh->py[i] = sh->py[0] + d * (c1 + d * (c2 + d * c3));
        double ex = sh->px[i] - sh->px[i - 1], ey = sh->py[i] - sh->py[i - 1];
        sh->cum[i] = sh->cum[i - 1] + sqrt(ex * ex + ey * ey);
    }
}

static inline void put_xy(adv_tile_scratch *s, int *m, double x, double y) {
    s->xy[2 * *m] = (int32_t)lround(x);
    s->xy[2 * *m + 1] = (int32_t)lround(y);
    (*m)++;
}

// The whole shape, or its two end points below the shape zoom.
static void emit_whole(adv_mvt *w, adv_tile_scratch *s, const shape *sh, bool ends_only, uint64_t id, const int *tags) {
    int m = 0;
    if (ends_only) {
        put_xy(s, &m, sh->px[0], sh->py[0]);
        put_xy(s, &m, sh->px[sh->n - 1], sh->py[sh->n - 1]);
    } else {
        for (int i = 0; i < sh->n; i++) put_xy(s, &m, sh->px[i], sh->py[i]);
    }
    adv_mvt_feature_line(w, id, tags, s->xy, m);
}

// The point `want` along the shape's projected length, on the edge ending at e+1.
static void put_along(adv_tile_scratch *s, int *m, const shape *sh, int e, double want) {
    double d = sh->cum[e + 1] - sh->cum[e];
    double t = d > 0 ? (want - sh->cum[e]) / d : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    put_xy(s, m, sh->px[e] + t * (sh->px[e + 1] - sh->px[e]), sh->py[e] + t * (sh->py[e + 1] - sh->py[e]));
}

// The sub-polyline from fraction a to fraction b of the shape's length: the entry point, the
// shape points strictly inside, the exit point; or the two ends alone below the shape zoom.
static void emit_run(adv_mvt *w, adv_tile_scratch *s, const shape *sh, double a, double b, bool ends_only, uint64_t id, const int *tags) {
    double total = sh->cum[sh->n - 1];
    if (total <= 0) return;   // every point projects onto one: the writer would drop it too
    double wa = a * total, wb = b * total;
    int m = 0, e = 0;
    while (e < sh->n - 2 && sh->cum[e + 1] <= wa) e++;
    put_along(s, &m, sh, e, wa);
    if (!ends_only)
        for (int i = e + 1; i < sh->n - 1 && sh->cum[i] < wb; i++) put_xy(s, &m, sh->px[i], sh->py[i]);
    int f = e;
    while (f < sh->n - 2 && sh->cum[f + 1] < wb) f++;
    put_along(s, &m, sh, f, wb);
    adv_mvt_feature_line(w, id, tags, s->xy, m);
}

// One linestring per run of set bits; a full mask is the shape itself, uncut. Ids are
// seg * 16 + first bit, unique within the layer since a segment's runs start at distinct bits.
static void emit_mask(adv_mvt *w, adv_tile_scratch *s, const shape *sh, uint32_t seg, uint16_t mask, bool ends_only, const int *tags) {
    uint64_t base = (uint64_t)seg * ADV_MASK_BITS;
    if (mask == 0xFFFF) {
        emit_whole(w, s, sh, ends_only, base, tags);
        return;
    }
    for (int bit = 0; bit < ADV_MASK_BITS;) {
        if (!((mask >> bit) & 1)) {
            bit++;
            continue;
        }
        int first = bit;
        while (bit < ADV_MASK_BITS && ((mask >> bit) & 1)) bit++;
        emit_run(w, s, sh, (double)first / ADV_MASK_BITS, (double)bit / ADV_MASK_BITS, ends_only, base + (uint64_t)first, tags);
    }
}

static bool type_allows(uint8_t access, int type) {
    if (type == ADV_T_ALL) return access != 0;
    return (access & (type == ADV_T_BIKE ? ADV_ACC_BIKE : ADV_ACC_FOOT)) != 0;
}

// ---- the tile --------------------------------------------------------------------------------

static int32_t clamp32(int64_t v) { return v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : (int32_t)v; }

// Which classes a zoom draws (mvt.h): the majors below the mid zoom, the streets below the full one.
static bool class_drawn(int z, uint8_t hclass) {
    if (z >= ADV_TILE_FULL_ZOOM) return true;
    if (z >= ADV_TILE_MID_ZOOM) return (ADV_TILE_MID_CLASSES >> hclass) & 1u;
    return hclass <= ADV_HC_TERTIARY;
}

// A LOW-ZOOM USER TILE WALKS THE COVERAGE, NOT THE CELLS: a z8 tile of the state is every cell
// of the grid (30,000 lists) for a handful of travelled segments, 25 ms; the user's sorted
// entries are a few thousand bbox tests. The cell walk stays for the zooms where a tile is a
// few cells and the untraveled layer (every segment) needs them anyway.
typedef struct {
    const adv_cov *cov;
    const walk    *wk;
    uint32_t       i;
} cov_walk;

static bool cov_walk_next(cov_walk *c, uint32_t *seg) {
    const adv_graph *g = c->wk->g;
    while (c->i < c->cov->n) {
        uint32_t id = c->cov->e[c->i++].seg;
        if (id >= g->h->n_segs) continue;
        const adv_seg *sg = &g->segs[id];
        if (sg->max_lat < c->wk->min_lat || sg->min_lat > c->wk->max_lat || sg->max_lon < c->wk->min_lon || sg->min_lon > c->wk->max_lon) continue;
        *seg = id;
        return true;
    }
    return false;
}

int adv_tile_render(const adv_graph *g, const adv_tile_req *req, uint32_t user, const adv_cov *cov,
                    const uint32_t *owners, adv_tile_scratch *s, const uint8_t **out) {
    if (!g || !req || !s || !out || !s->bytes) return CB_E_INVALID;
    if (req->z < 0 || req->z > ADV_TILE_MAX_ZOOM) return CB_E_INVALID;
    uint32_t n = 1u << req->z;
    if (req->x >= n || req->y >= n) return CB_E_INVALID;
    if (req->type < 0 || req->type >= ADV_T_N) return CB_E_INVALID;
    // The stamp array is the graph's size: a scratch made for another graph would index past it.
    if (s->n_segs != g->h->n_segs) return CB_E_INVALID;

    adv_mvt w;
    adv_mvt_init(&w, s->bytes, s->cap);

    adv_pt mn, mx;
    adv_tile_bounds(req->z, req->x, req->y, &mn, &mx);
    int64_t dlat = ((int64_t)mx.lat - mn.lat) / TILE_BUFFER_DIV, dlon = ((int64_t)mx.lon - mn.lon) / TILE_BUFFER_DIV;
    walk wk = { .g = g, .s = s };
    wk.min_lat = (int64_t)mn.lat - dlat;
    wk.max_lat = (int64_t)mx.lat + dlat;
    wk.min_lon = (int64_t)mn.lon - dlon;
    wk.max_lon = (int64_t)mx.lon + dlon;
    adv_pt gmin = { clamp32(wk.min_lat), clamp32(wk.min_lon) }, gmax = { clamp32(wk.max_lat), clamp32(wk.max_lon) };
    // A tile off the grid is empty: no layers, zero bytes, which MapLibre takes as "nothing here".
    if (!adv_cells_of_box(g, gmin, gmax, &wk.i0, &wk.j0, &wk.i1, &wk.j1)) return adv_mvt_finish(&w, out);

    bool ends_only = req->z < ADV_TILE_SHAPE_ZOOM;
    adv_tile_frame tf;
    adv_tile_frame_init(&tf, req->z, req->x, req->y);
    // A layer is thousands of lines (tens of thousands below the shape zoom), and a feature's
    // header, id and tags were most of its bytes: one MULTILINESTRING per tag tuple (mvt.h,
    // grouped) instead of a feature each, at every zoom.
    adv_mvt_lines *grp = req->ungrouped ? NULL : &s->lines;
    shape sh;
    uint32_t seg;
    int tags[ADV_MVT_MAX_TAGS] = { -1, -1, -1, -1 };

    if (req->roads) {
        adv_mvt_layer_begin_grouped(&w, "roads", KEYS_C, 1, grp);
        walk_begin(&wk);
        while (walk_next(&wk, &seg)) {
            const adv_seg *sg = &g->segs[seg];
            if (!class_drawn(req->z, sg->hclass)) continue;
            shape_load(g, req, s, &tf, ends_only, seg, &sh);
            if (sh.n < 2) continue;
            if (ends_only && sh.cum[sh.n - 1] < SPECK_UNITS) continue;
            tags[0] = sg->hclass;
            emit_whole(&w, s, &sh, ends_only, seg, tags);
        }
        adv_mvt_layer_end(&w);
    }
    // Everyone's tile walks the cells at every zoom (the union is every segment anyone has, not
    // a user's short list) and, below the untraveled floor, draws the classes the roads layer
    // draws two zooms higher (streets from z9, the majors below), as a region-wide tile of
    // every alley would be the whole city painted; from z11 up everything, like a user's.
    const adv_all_seg *all = req->all;
    bool by_cov = cov && !all && req->z < ADV_TILE_UNTRAVELED_ZOOM;
    bool majors = all && req->z < ADV_TILE_UNTRAVELED_ZOOM;
    int all_z = req->z + 2;
    if (req->traveled) {
        adv_mvt_layer_begin_grouped(&w, "traveled", all ? KEYS_CTN : KEYS_CT, all ? 3 : 2, grp);
        cov_walk cw = { cov, &wk, 0 };
        if (!by_cov) walk_begin(&wk);
        while (by_cov ? cov_walk_next(&cw, &seg) : walk_next(&wk, &seg)) {
            uint16_t mask = all ? atomic_load_explicit(&all[seg].mask[req->type], memory_order_relaxed) : cov ? adv_cov_mask(cov, seg, req->type) : 0;
            if (!mask) continue;
            if (majors && !class_drawn(all_z, g->segs[seg].hclass)) continue;
            shape_load(g, req, s, &tf, ends_only, seg, &sh);
            if (sh.n < 2) continue;
            tags[0] = g->segs[seg].hclass;
            tags[1] = req->type;
            if (all) {
                uint16_t n_users = atomic_load_explicit(&all[seg].users[req->type], memory_order_relaxed);
                tags[2] = n_users > ADV_MVT_MAX_VALUE ? ADV_MVT_MAX_VALUE : n_users;
            }
            emit_mask(&w, s, &sh, seg, mask, ends_only, tags);
        }
        tags[2] = -1;
        adv_mvt_layer_end(&w);
    }
    if (req->untraveled) {
        adv_mvt_layer_begin_grouped(&w, "untraveled", KEYS_CT, 2, grp);
        // Below the floor the layer is present and empty: the style still finds it, and a
        // region-wide tile is not painted red whole.
        if (req->z >= ADV_TILE_UNTRAVELED_ZOOM) {
            walk_begin(&wk);
            while (walk_next(&wk, &seg)) {
                const adv_seg *sg = &g->segs[seg];
                if (!type_allows(sg->access, req->type)) continue;
                if (!class_drawn(req->z, sg->hclass)) continue;   // the red of a city at z11 is its streets
                uint16_t have = all ? atomic_load_explicit(&all[seg].mask[req->type], memory_order_relaxed) : cov ? adv_cov_mask(cov, seg, req->type) : 0;
                uint16_t mask = (uint16_t)(~have & 0xFFFF);
                if (!mask) continue;
                shape_load(g, req, s, &tf, ends_only, seg, &sh);
                if (sh.n < 2) continue;
                tags[0] = sg->hclass;
                tags[1] = req->type;
                emit_mask(&w, s, &sh, seg, mask, ends_only, tags);
            }
        }
        adv_mvt_layer_end(&w);
    }
    if (req->unique) {
        adv_mvt_layer_begin_grouped(&w, "unique", KEYS_C, 1, grp);
        if (all) {   // what exactly one person has
            walk_begin(&wk);
            while (walk_next(&wk, &seg)) {
                if (atomic_load_explicit(&all[seg].users[req->type], memory_order_relaxed) != 1) continue;
                if (majors && !class_drawn(all_z, g->segs[seg].hclass)) continue;
                shape_load(g, req, s, &tf, ends_only, seg, &sh);
                if (sh.n < 2) continue;
                tags[0] = g->segs[seg].hclass;
                emit_whole(&w, s, &sh, ends_only, seg, tags);
            }
        } else if (owners && user != 0) {
            cov_walk cw = { cov, &wk, 0 };
            if (!by_cov) walk_begin(&wk);
            while (by_cov ? cov_walk_next(&cw, &seg) : walk_next(&wk, &seg)) {
                if (owners[seg] != user) continue;
                shape_load(g, req, s, &tf, ends_only, seg, &sh);
                if (sh.n < 2) continue;
                tags[0] = g->segs[seg].hclass;
                emit_whole(&w, s, &sh, ends_only, seg, tags);
            }
        }
        adv_mvt_layer_end(&w);
    }
    s->dropped = w.dropped;
    return adv_mvt_finish(&w, out);
}

bool adv_tile_parse_zxy(const char *s, int *z, uint32_t *x, uint32_t *y) {
    if (!s) return false;
    size_t n = strnlen(s, 33);   // "18/262143/262143" is 16; 32 leaves room and bounds the scan
    if (n == 0 || n > 32) return false;
    uint64_t v[3];
    size_t i = 0;
    for (int part = 0; part < 3; part++) {
        size_t start = i;
        uint64_t acc = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            acc = acc * 10 + (uint64_t)(s[i] - '0');
            if (i - start >= 10) return false;   // 10 digits already hold 2^32; more cannot be a tile
            i++;
        }
        if (i == start) return false;
        v[part] = acc;
        if (part < 2) {
            if (i >= n || s[i] != '/') return false;
            i++;
        }
    }
    if (i != n) return false;
    if (v[0] > ADV_TILE_MAX_ZOOM) return false;
    uint64_t side = 1ull << v[0];
    if (v[1] >= side || v[2] >= side) return false;
    *z = (int)v[0];
    *x = (uint32_t)v[1];
    *y = (uint32_t)v[2];
    return true;
}
