// graph.c - graph.h: the reader (mmap and check), the shape and cell helpers, and the builder.
#include "graph.h"

#include "crc32c/crc32c.h"
#include "util/err.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ALIGN8(x) (((x) + 7u) & ~(uint64_t)7u)

// ---- the reader ---------------------------------------------------------------------------

static int fail(char *err, size_t cap, const char *what) {
    if (cap) snprintf(err, cap, "graph: %s", what);
    return CB_E_INVALID;
}

static bool section_ok(uint64_t off, uint64_t count, uint64_t each, uint64_t size) {
    if (off % 8 != 0 || off > size) return false;
    if (count > (size - off) / (each ? each : 1)) return false;
    return off + count * each <= size;
}

int adv_graph_check(const void *p, size_t n, char *err, size_t cap) {
    if (n < sizeof(adv_graph_hdr)) return fail(err, cap, "file shorter than its header");
    adv_graph_hdr h;
    memcpy(&h, p, sizeof h);
    if (h.magic != ADV_GRAPH_MAGIC) return fail(err, cap, "not a graph file (magic)");
    if (h.version != ADV_GRAPH_VERSION) return fail(err, cap, "another version of the graph format");
    if (h.file_size != n) return fail(err, cap, "file size differs from the header's");
    if (h.cell_zoom != ADV_CELL_ZOOM) return fail(err, cap, "another cell zoom");
    if (h.n_segs > ADV_MAX_SEGS || h.n_regions > ADV_MAX_REGIONS) return fail(err, cap, "too many segments or regions");
    if (h.cells_w == 0 || h.cells_h == 0 || h.cells_w > (1u << ADV_CELL_ZOOM) || h.cells_h > (1u << ADV_CELL_ZOOM))
        return fail(err, cap, "an empty or oversized cell grid");
    if (h.cell_x0 + h.cells_w > (1u << ADV_CELL_ZOOM) || h.cell_y0 + h.cells_h > (1u << ADV_CELL_ZOOM))
        return fail(err, cap, "the cell grid runs off the world");
    uint64_t n_cells = (uint64_t)h.cells_w * h.cells_h;
    if (n_cells > (1u << 26)) return fail(err, cap, "too many cells");
    if (!section_ok(h.off_segs, h.n_segs, sizeof(adv_seg), n) || !section_ok(h.off_deltas, h.n_deltas, sizeof(adv_delta), n) ||
        !section_ok(h.off_nodes, h.n_nodes, sizeof(adv_node), n) || !section_ok(h.off_adj, h.n_adj, 4, n) ||
        !section_ok(h.off_cells, n_cells, sizeof(adv_cell), n) || !section_ok(h.off_cell_segs, h.n_cell_segs, 4, n) ||
        !section_ok(h.off_regions, h.n_regions, sizeof(adv_region), n))
        return fail(err, cap, "a section runs past the file");
    if (h.off_segs < sizeof h) return fail(err, cap, "a section overlaps the header");
    uint32_t crc = crc32c(0, (const uint8_t *)p + sizeof h, n - sizeof h);
    if (crc != h.crc) return fail(err, cap, "checksum mismatch");
    const uint8_t *b = p;
    const adv_seg *segs = (const adv_seg *)(b + h.off_segs);
    const adv_node *nodes = (const adv_node *)(b + h.off_nodes);
    const uint32_t *adj = (const uint32_t *)(b + h.off_adj);
    const adv_cell *cells = (const adv_cell *)(b + h.off_cells);
    const uint32_t *cell_segs = (const uint32_t *)(b + h.off_cell_segs);
    for (uint32_t i = 0; i < h.n_segs; i++) {
        const adv_seg *s = &segs[i];
        if (s->n_pts < 2 || s->n_pts > ADV_SEG_MAX_PTS) return fail(err, cap, "a segment's point count");
        if ((uint64_t)s->shape_off + s->n_pts - 1 > h.n_deltas) return fail(err, cap, "a segment's shape runs past the deltas");
        if (s->node_a >= h.n_nodes || s->node_b >= h.n_nodes) return fail(err, cap, "a segment names a node that is not there");
        if (s->region != ADV_NO_REGION && s->region >= h.n_regions) return fail(err, cap, "a segment names a region that is not there");
        if (s->hclass >= ADV_HC_N) return fail(err, cap, "a segment's class");
        if (!(s->len_m >= 0 && s->len_m <= ADV_SEG_MAX_LEN_M * 1.01f)) return fail(err, cap, "a segment's length");
        if (s->min_lat > s->max_lat || s->min_lon > s->max_lon) return fail(err, cap, "a segment's box");
    }
    for (uint32_t i = 0; i < h.n_nodes; i++) {
        if (nodes[i].n_adj > ADV_NODE_MAX_ADJ || (uint64_t)nodes[i].adj_off + nodes[i].n_adj > h.n_adj)
            return fail(err, cap, "a node's adjacency");
        for (uint32_t k = 0; k < nodes[i].n_adj; k++)
            if (adj[nodes[i].adj_off + k] >= h.n_segs) return fail(err, cap, "an adjacency names a segment that is not there");
    }
    for (uint64_t c = 0; c < n_cells; c++) {
        if (cells[c].n > ADV_SEG_MAX_PER_CELL || (uint64_t)cells[c].off + cells[c].n > h.n_cell_segs)
            return fail(err, cap, "a cell's list");
        for (uint32_t k = 0; k < cells[c].n; k++) {
            uint32_t s = cell_segs[cells[c].off + k];
            if (s >= h.n_segs) return fail(err, cap, "a cell names a segment that is not there");
            if (k > 0 && s <= cell_segs[cells[c].off + k - 1]) return fail(err, cap, "a cell's list is not ascending");
        }
    }
    const adv_region *regions = (const adv_region *)(b + h.off_regions);
    for (uint32_t i = 0; i < h.n_regions; i++)
        if (!memchr(regions[i].name, 0, ADV_REGION_NAME)) return fail(err, cap, "a region's name is not terminated");
    return CB_OK;
}

static void graph_point(adv_graph *g, const void *p) {
    const uint8_t *b = p;
    g->h = (const adv_graph_hdr *)b;
    g->segs = (const adv_seg *)(b + g->h->off_segs);
    g->deltas = (const adv_delta *)(b + g->h->off_deltas);
    g->nodes = (const adv_node *)(b + g->h->off_nodes);
    g->adj = (const uint32_t *)(b + g->h->off_adj);
    g->cells = (const adv_cell *)(b + g->h->off_cells);
    g->cell_segs = (const uint32_t *)(b + g->h->off_cell_segs);
    g->regions = (const adv_region *)(b + g->h->off_regions);
}

int adv_graph_open(const char *path, adv_graph *g, char *err, size_t cap) {
    memset(g, 0, sizeof *g);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (cap) snprintf(err, cap, "graph: %s: %s", path, strerror(errno));
        return CB_E_IO;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(adv_graph_hdr)) {
        close(fd);
        if (cap) snprintf(err, cap, "graph: %s: not a graph file", path);
        return CB_E_INVALID;
    }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        if (cap) snprintf(err, cap, "graph: mmap: %s", strerror(errno));
        return CB_E_IO;
    }
    int rc = adv_graph_check(m, (size_t)st.st_size, err, cap);
    if (rc != CB_OK) {
        munmap(m, (size_t)st.st_size);
        return rc;
    }
    g->map = m;
    g->map_len = (size_t)st.st_size;
    graph_point(g, m);
    return CB_OK;
}

void adv_graph_close(adv_graph *g) {
    if (g->map) munmap(g->map, g->map_len);
    memset(g, 0, sizeof *g);
}

int adv_seg_points(const adv_graph *g, uint32_t seg, adv_pt *out, int cap) {
    const adv_seg *s = &g->segs[seg];
    int n = s->n_pts;
    if (cap < n) return CB_E_FULL;
    adv_pt p = { s->lat, s->lon };
    out[0] = p;
    const adv_delta *d = &g->deltas[s->shape_off];
    for (int i = 1; i < n; i++) {
        p.lat += d[i - 1].dlat;
        p.lon += d[i - 1].dlon;
        out[i] = p;
    }
    return n;
}

adv_pt adv_seg_at(const adv_graph *g, uint32_t seg, double frac) {
    adv_pt pts[ADV_SEG_MAX_PTS];
    int n = adv_seg_points(g, seg, pts, ADV_SEG_MAX_PTS);
    if (frac <= 0) return pts[0];
    if (frac >= 1) return pts[n - 1];
    adv_frame f = adv_frame_at(pts[0].lat);
    double want = frac * g->segs[seg].len_m, acc = 0;
    for (int i = 1; i < n; i++) {
        double d = adv_dist_m(&f, pts[i - 1], pts[i]);
        if (acc + d >= want || i == n - 1) {
            double t = d > 0 ? (want - acc) / d : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            adv_pt r;
            r.lat = (int32_t)(pts[i - 1].lat + t * ((double)pts[i].lat - pts[i - 1].lat));
            r.lon = (int32_t)(pts[i - 1].lon + t * ((double)pts[i].lon - pts[i - 1].lon));
            return r;
        }
        acc += d;
    }
    return pts[n - 1];
}

uint16_t adv_mask_of_run(double a, double b) {
    if (a > b) { double t = a; a = b; b = t; }
    if (a < 0) a = 0;
    if (b > 1) b = 1;
    uint16_t m = 0;
    for (int i = 0; i < ADV_MASK_BITS; i++) {
        double lo = (double)i / ADV_MASK_BITS, hi = (double)(i + 1) / ADV_MASK_BITS;
        double ov = (b < hi ? b : hi) - (a > lo ? a : lo);
        if (ov > ADV_MASK_SLOP / ADV_MASK_BITS) m |= (uint16_t)(1u << i);
    }
    if (!m) {
        int i = (int)((a + b) / 2 * ADV_MASK_BITS);
        if (i >= ADV_MASK_BITS) i = ADV_MASK_BITS - 1;
        if (i < 0) i = 0;
        m = (uint16_t)(1u << i);
    }
    return m;
}

bool adv_cell_of(const adv_graph *g, adv_pt p, uint32_t *i, uint32_t *j) {
    uint32_t x, y;
    adv_tile_of(p, ADV_CELL_ZOOM, &x, &y);
    const adv_graph_hdr *h = g->h;
    if (x < h->cell_x0 || y < h->cell_y0 || x - h->cell_x0 >= h->cells_w || y - h->cell_y0 >= h->cells_h) return false;
    *i = x - h->cell_x0;
    *j = y - h->cell_y0;
    return true;
}

const uint32_t *adv_cell_list(const adv_graph *g, uint32_t i, uint32_t j, uint32_t *n) {
    if (i >= g->h->cells_w || j >= g->h->cells_h) {
        *n = 0;
        return NULL;
    }
    const adv_cell *c = &g->cells[(size_t)j * g->h->cells_w + i];
    *n = c->n;
    return g->cell_segs + c->off;
}

bool adv_cells_of_box(const adv_graph *g, adv_pt min, adv_pt max, uint32_t *i0, uint32_t *j0, uint32_t *i1, uint32_t *j1) {
    uint32_t x0, y0, x1, y1;
    adv_pt nw = { max.lat, min.lon }, se = { min.lat, max.lon };
    adv_tile_of(nw, ADV_CELL_ZOOM, &x0, &y0);
    adv_tile_of(se, ADV_CELL_ZOOM, &x1, &y1);
    const adv_graph_hdr *h = g->h;
    uint32_t gx0 = h->cell_x0, gy0 = h->cell_y0, gx1 = h->cell_x0 + h->cells_w - 1, gy1 = h->cell_y0 + h->cells_h - 1;
    if (x1 < gx0 || x0 > gx1 || y1 < gy0 || y0 > gy1) return false;
    if (x0 < gx0) x0 = gx0;
    if (y0 < gy0) y0 = gy0;
    if (x1 > gx1) x1 = gx1;
    if (y1 > gy1) y1 = gy1;
    *i0 = x0 - gx0;
    *j0 = y0 - gy0;
    *i1 = x1 - gx0;
    *j1 = y1 - gy0;
    return true;
}

// ---- the builder --------------------------------------------------------------------------

typedef struct {
    uint32_t pt_off, n;
    uint8_t  access, hclass, flags;
} bway;

typedef struct {
    uint32_t pt_off, n;
    int32_t  min_lat, min_lon, max_lat, max_lon;
} bring;

typedef struct {
    char     name[ADV_REGION_NAME];
    uint32_t ring_off, n_rings;   // rings are appended in order, so a region's are contiguous only
                                  // when added together; the builder keeps a list instead
    uint32_t *rings;
    uint32_t n_ring_ids, cap_ring_ids;
} bregion;

struct adv_build {
    char      source[64];
    bway     *ways;
    uint32_t  n_ways, cap_ways;
    int64_t  *ids;
    adv_pt   *pts;
    uint32_t  n_pts, cap_pts;
    bring    *rings;
    uint32_t  n_rings, cap_rings;
    adv_pt   *ring_pts;
    uint32_t  n_ring_pts, cap_ring_pts;
    bregion  *regions;
    uint32_t  n_regions, cap_regions;
    int64_t   next_synthetic;   // node ids the densifier makes up (negative, never OSM's)
    adv_build_stats stats;
};

#define GROW(arr, n, cap, need)                                                   \
    do {                                                                          \
        if ((uint64_t)(n) + (need) > (cap)) {                                     \
            uint64_t nc = (cap) ? (cap) : 1024;                                   \
            while (nc < (uint64_t)(n) + (need)) nc *= 2;                          \
            if (nc > UINT32_MAX) return CB_E_FULL;                                \
            void *np = realloc((arr), (size_t)nc * sizeof *(arr));                \
            if (!np) return CB_E_NOMEM;                                           \
            (arr) = np;                                                           \
            (cap) = (uint32_t)nc;                                                 \
        }                                                                         \
    } while (0)

adv_build *adv_build_new(const char *source) {
    adv_build *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    snprintf(b->source, sizeof b->source, "%s", source ? source : "");
    b->next_synthetic = INT64_MIN + 1;
    return b;
}

void adv_build_free(adv_build *b) {
    if (!b) return;
    free(b->ways);
    free(b->ids);
    free(b->pts);
    free(b->rings);
    free(b->ring_pts);
    for (uint32_t i = 0; i < b->n_regions; i++) free(b->regions[i].rings);
    free(b->regions);
    free(b);
}

static int push_pt(adv_build *b, int64_t id, adv_pt p) {
    GROW(b->ids, b->n_pts, b->cap_pts, 1);
    if ((uint64_t)b->n_pts + 1 > b->cap_pts) return CB_E_NOMEM;
    // pts grows with ids (same cap).
    adv_pt *np = realloc(b->pts, (size_t)b->cap_pts * sizeof *np);
    if (!np) return CB_E_NOMEM;
    b->pts = np;
    b->ids[b->n_pts] = id;
    b->pts[b->n_pts] = p;
    b->n_pts++;
    return CB_OK;
}

int adv_build_add_way(adv_build *b, const int64_t *node_ids, const adv_pt *pts, int n, uint8_t access, uint8_t hclass, uint8_t flags) {
    if (n < 2 || hclass >= ADV_HC_N) return CB_E_INVALID;
    for (int i = 0; i < n; i++)
        if (!adv_pt_valid(pts[i])) return CB_E_INVALID;
    GROW(b->ways, b->n_ways, b->cap_ways, 1);
    uint32_t off = b->n_pts;
    adv_pt prev = pts[0];
    int rc = push_pt(b, node_ids[0], prev);
    if (rc != CB_OK) return rc;
    for (int i = 1; i < n; i++) {
        if (pts[i].lat == prev.lat && pts[i].lon == prev.lon) continue;   // a repeat
        // Densify so every delta fits an int16.
        int64_t dlat = (int64_t)pts[i].lat - prev.lat, dlon = (int64_t)pts[i].lon - prev.lon;
        int64_t big = llabs(dlat) > llabs(dlon) ? llabs(dlat) : llabs(dlon);
        int steps = (int)(big / 30000) + 1;
        for (int k = 1; k < steps; k++) {
            adv_pt m;
            m.lat = (int32_t)(prev.lat + dlat * k / steps);
            m.lon = (int32_t)(prev.lon + dlon * k / steps);
            if ((rc = push_pt(b, b->next_synthetic++, m)) != CB_OK) return rc;
        }
        if ((rc = push_pt(b, node_ids[i], pts[i])) != CB_OK) return rc;
        prev = pts[i];
    }
    uint32_t count = b->n_pts - off;
    if (count < 2) {   // every point was the same one
        b->n_pts = off;
        return CB_OK;
    }
    bway w = { off, count, access, hclass, flags };
    b->ways[b->n_ways++] = w;
    return CB_OK;
}

int adv_build_region(adv_build *b, const char *name) {
    if (!name || !*name || strlen(name) >= ADV_REGION_NAME) return CB_E_INVALID;
    for (uint32_t i = 0; i < b->n_regions; i++)
        if (strcmp(b->regions[i].name, name) == 0) return (int)i;
    if (b->n_regions >= ADV_MAX_REGIONS) return CB_E_FULL;
    GROW(b->regions, b->n_regions, b->cap_regions, 1);
    bregion *r = &b->regions[b->n_regions];
    memset(r, 0, sizeof *r);
    snprintf(r->name, sizeof r->name, "%s", name);
    return (int)b->n_regions++;
}

int adv_build_region_ring(adv_build *b, int region, const adv_pt *ring, int n) {
    if (region < 0 || (uint32_t)region >= b->n_regions || n < 3) return CB_E_INVALID;
    bregion *r = &b->regions[region];
    GROW(r->rings, r->n_ring_ids, r->cap_ring_ids, 1);
    GROW(b->rings, b->n_rings, b->cap_rings, 1);
    GROW(b->ring_pts, b->n_ring_pts, b->cap_ring_pts, (uint64_t)n);
    bring g = { b->n_ring_pts, (uint32_t)n, INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
    for (int i = 0; i < n; i++) {
        if (!adv_pt_valid(ring[i])) return CB_E_INVALID;
        b->ring_pts[b->n_ring_pts++] = ring[i];
        if (ring[i].lat < g.min_lat) g.min_lat = ring[i].lat;
        if (ring[i].lat > g.max_lat) g.max_lat = ring[i].lat;
        if (ring[i].lon < g.min_lon) g.min_lon = ring[i].lon;
        if (ring[i].lon > g.max_lon) g.max_lon = ring[i].lon;
    }
    r->rings[r->n_ring_ids++] = b->n_rings;
    b->rings[b->n_rings++] = g;
    return CB_OK;
}

// Even-odd over one ring (int64 arithmetic: no overflow at int32 coordinates).
static bool ring_contains(const adv_build *b, const bring *r, adv_pt p) {
    if (p.lat < r->min_lat || p.lat > r->max_lat || p.lon < r->min_lon || p.lon > r->max_lon) return false;
    bool in = false;
    const adv_pt *v = b->ring_pts + r->pt_off;
    for (uint32_t i = 0, j = r->n - 1; i < r->n; j = i++) {
        int64_t yi = v[i].lat, yj = v[j].lat, xi = v[i].lon, xj = v[j].lon;
        if ((yi > p.lat) != (yj > p.lat)) {
            // x of the edge at p.lat, compared without division: sign-aware cross-multiply.
            int64_t num = (xj - xi) * ((int64_t)p.lat - yi), den = yj - yi;
            // p.lon < xi + num/den  <=>  (p.lon - xi) * den < num  when den > 0 (else flipped)
            int64_t lhs = ((int64_t)p.lon - xi) * den;
            if (den > 0 ? lhs < num : lhs > num) in = !in;
        }
    }
    return in;
}

static int region_of(const adv_build *b, adv_pt p) {
    for (uint32_t i = 0; i < b->n_regions; i++) {
        const bregion *r = &b->regions[i];
        int crossings = 0;
        for (uint32_t k = 0; k < r->n_ring_ids; k++)
            if (ring_contains(b, &b->rings[r->rings[k]], p)) crossings++;
        if (crossings & 1) return (int)i;
    }
    return -1;
}

// A node id's uses across every way: sorted, so a run of equal ids is one node.
typedef struct {
    int64_t id;
    uint32_t pt;   // index into b->pts
} use;
static int use_cmp(const void *a, const void *c) {
    int64_t x = ((const use *)a)->id, y = ((const use *)c)->id;
    if (x != y) return x < y ? -1 : 1;
    uint32_t p = ((const use *)a)->pt, q = ((const use *)c)->pt;
    return p < q ? -1 : p > q;
}

typedef struct {
    uint32_t way;
    uint32_t from, to;      // point indices within b->pts (inclusive)
    uint32_t node_a, node_b;
    uint64_t hkey;
    float    len_m;
    int32_t  min_lat, min_lon, max_lat, max_lon;
    adv_pt   mid;
    uint16_t region;
} bseg;
static int bseg_cmp(const void *a, const void *c) {
    uint64_t x = ((const bseg *)a)->hkey, y = ((const bseg *)c)->hkey;
    if (x != y) return x < y ? -1 : 1;
    uint32_t p = ((const bseg *)a)->from, q = ((const bseg *)c)->from;
    return p < q ? -1 : p > q;
}

typedef struct {
    uint8_t *p;
    size_t   n, cap;
} obuf;
static int ob_put(obuf *o, const void *p, size_t n) {
    if (o->n + n > o->cap) {
        size_t nc = o->cap ? o->cap : 1 << 20;
        while (nc < o->n + n) nc *= 2;
        void *np = realloc(o->p, nc);
        if (!np) return CB_E_NOMEM;
        o->p = np;
        o->cap = nc;
    }
    memcpy(o->p + o->n, p, n);
    o->n += n;
    return CB_OK;
}
static int ob_pad(obuf *o) {
    static const uint8_t z[8] = { 0 };
    size_t r = o->n % 8;
    return r ? ob_put(o, z, 8 - r) : CB_OK;
}

static int finish_fail(char *err, size_t cap, const char *what, int rc) {
    if (cap) snprintf(err, cap, "graph build: %s", what);
    return rc;
}

int adv_build_finish(adv_build *b, const char *path, char *err, size_t cap) {
    int rc = CB_OK;
    if (b->n_ways == 0) return finish_fail(err, cap, "no ways", CB_E_INVALID);
    // 1. Which points are graph nodes: a way's ends, and any node id used twice.
    uint8_t *is_node = calloc(b->n_pts, 1);
    uint32_t *node_of_pt = malloc((size_t)b->n_pts * sizeof *node_of_pt);
    use *uses = malloc((size_t)b->n_pts * sizeof *uses);
    bseg *segs = NULL;
    uint32_t n_segs = 0, cap_segs = 0;
    uint32_t *node_pt = NULL;   // a node's representative point
    uint32_t n_nodes = 0;
    uint32_t *adj_count = NULL, *adj_off = NULL, *adj = NULL, *cell_count = NULL, *cell_off = NULL, *cell_segs = NULL;
    adv_cell *cells = NULL;
    adv_seg *out_segs = NULL;
    adv_node *out_nodes = NULL;
    adv_delta *deltas = NULL;
    adv_region *regions = NULL;
    uint32_t *node_renum = NULL;
    obuf ob = { 0 };
    if (!is_node || !node_of_pt || !uses) { rc = CB_E_NOMEM; goto out; }
    for (uint32_t i = 0; i < b->n_pts; i++) { uses[i].id = b->ids[i]; uses[i].pt = i; node_of_pt[i] = UINT32_MAX; }
    for (uint32_t w = 0; w < b->n_ways; w++) {
        is_node[b->ways[w].pt_off] = 1;
        is_node[b->ways[w].pt_off + b->ways[w].n - 1] = 1;
    }
    qsort(uses, b->n_pts, sizeof *uses, use_cmp);
    // A run of equal ids: one node (every use gets the same node index; the first use is its point).
    node_pt = malloc((size_t)b->n_pts * sizeof *node_pt);
    if (!node_pt) { rc = CB_E_NOMEM; goto out; }
    for (uint32_t i = 0; i < b->n_pts;) {
        uint32_t j = i;
        while (j < b->n_pts && uses[j].id == uses[i].id) j++;
        bool node = (j - i >= 2);
        for (uint32_t k = i; k < j; k++) if (is_node[uses[k].pt]) node = true;
        if (node) {
            for (uint32_t k = i; k < j; k++) { node_of_pt[uses[k].pt] = n_nodes; is_node[uses[k].pt] = 1; }
            node_pt[n_nodes++] = uses[i].pt;
        }
        i = j;
    }
    // 2. Split every way at its nodes and at the caps.
    for (uint32_t w = 0; w < b->n_ways; w++) {
        const bway *way = &b->ways[w];
        uint32_t start = way->pt_off, end = way->pt_off + way->n - 1;
        adv_frame f = adv_frame_at(b->pts[start].lat);
        double len = 0;
        for (uint32_t i = start + 1; i <= end; i++) {
            double d = adv_dist_m(&f, b->pts[i - 1], b->pts[i]);
            bool cut = is_node[i] || i == end;
            if (!cut && (len + d > ADV_SEG_MAX_LEN_M || i - start + 1 >= ADV_SEG_MAX_PTS)) {
                // Make this interior point a node of its own.
                if (node_of_pt[i] == UINT32_MAX) { node_of_pt[i] = n_nodes; node_pt[n_nodes++] = i; }
                is_node[i] = 1;
                cut = true;
            }
            len += d;
            if (cut) {
                if (len > ADV_SEG_MAX_LEN_M) {
                    // One edge longer than the cap (a densified straight line): the densifier
                    // kept every edge under 3.6 km, so split at the point before and accept.
                }
                if (n_segs == cap_segs) {
                    uint32_t nc = cap_segs ? cap_segs * 2 : 4096;
                    bseg *ns = realloc(segs, (size_t)nc * sizeof *ns);
                    if (!ns) { rc = CB_E_NOMEM; goto out; }
                    segs = ns;
                    cap_segs = nc;
                }
                bseg s;
                memset(&s, 0, sizeof s);
                s.way = w;
                s.from = start;
                s.to = i;
                s.node_a = node_of_pt[start];
                s.node_b = node_of_pt[i];
                s.len_m = (float)len;
                s.min_lat = s.max_lat = b->pts[start].lat;
                s.min_lon = s.max_lon = b->pts[start].lon;
                for (uint32_t k = start + 1; k <= i; k++) {
                    if (b->pts[k].lat < s.min_lat) s.min_lat = b->pts[k].lat;
                    if (b->pts[k].lat > s.max_lat) s.max_lat = b->pts[k].lat;
                    if (b->pts[k].lon < s.min_lon) s.min_lon = b->pts[k].lon;
                    if (b->pts[k].lon > s.max_lon) s.max_lon = b->pts[k].lon;
                }
                // The midpoint by length.
                double want = len / 2, acc = 0;
                s.mid = b->pts[i];
                for (uint32_t k = start + 1; k <= i; k++) {
                    double dd = adv_dist_m(&f, b->pts[k - 1], b->pts[k]);
                    if (acc + dd >= want) {
                        double t = dd > 0 ? (want - acc) / dd : 0;
                        s.mid.lat = (int32_t)(b->pts[k - 1].lat + t * ((double)b->pts[k].lat - b->pts[k - 1].lat));
                        s.mid.lon = (int32_t)(b->pts[k - 1].lon + t * ((double)b->pts[k].lon - b->pts[k - 1].lon));
                        break;
                    }
                    acc += dd;
                }
                uint32_t cx, cy;
                adv_tile_of(s.mid, ADV_CELL_ZOOM, &cx, &cy);
                s.hkey = adv_hilbert_xy2d(1u << ADV_CELL_ZOOM, cx, cy);
                int r = region_of(b, s.mid);
                s.region = r < 0 ? ADV_NO_REGION : (uint16_t)r;
                segs[n_segs++] = s;
                if (n_segs > ADV_MAX_SEGS) { rc = finish_fail(err, cap, "too many segments", CB_E_FULL); goto out; }
                start = i;
                len = 0;
            }
        }
    }
    if (n_segs == 0) { rc = finish_fail(err, cap, "no segments", CB_E_INVALID); goto out; }
    // 3. Hilbert order; nodes renumbered in order of first appearance.
    qsort(segs, n_segs, sizeof *segs, bseg_cmp);
    node_renum = malloc((size_t)n_nodes * sizeof *node_renum);
    if (!node_renum) { rc = CB_E_NOMEM; goto out; }
    for (uint32_t i = 0; i < n_nodes; i++) node_renum[i] = UINT32_MAX;
    uint32_t next_node = 0;
    for (uint32_t i = 0; i < n_segs; i++) {
        if (node_renum[segs[i].node_a] == UINT32_MAX) node_renum[segs[i].node_a] = next_node++;
        if (node_renum[segs[i].node_b] == UINT32_MAX) node_renum[segs[i].node_b] = next_node++;
    }
    // 4. Output segments and deltas.
    out_segs = calloc(n_segs, sizeof *out_segs);
    uint64_t n_deltas_cap = 0;
    for (uint32_t i = 0; i < n_segs; i++) n_deltas_cap += segs[i].to - segs[i].from;
    deltas = malloc((size_t)(n_deltas_cap ? n_deltas_cap : 1) * sizeof *deltas);
    out_nodes = calloc(next_node, sizeof *out_nodes);
    adj_count = calloc(next_node, sizeof *adj_count);
    adj_off = calloc(next_node + 1, sizeof *adj_off);
    if (!out_segs || !deltas || !out_nodes || !adj_count || !adj_off) { rc = CB_E_NOMEM; goto out; }
    uint32_t n_deltas = 0;
    int32_t gmin_lat = INT32_MAX, gmin_lon = INT32_MAX, gmax_lat = INT32_MIN, gmax_lon = INT32_MIN;
    double total[ADV_T_N] = { 0, 0, 0 };
    for (uint32_t i = 0; i < n_segs; i++) {
        const bseg *s = &segs[i];
        const bway *w = &b->ways[s->way];
        adv_seg *o = &out_segs[i];
        o->lat = b->pts[s->from].lat;
        o->lon = b->pts[s->from].lon;
        o->min_lat = s->min_lat; o->min_lon = s->min_lon; o->max_lat = s->max_lat; o->max_lon = s->max_lon;
        o->shape_off = n_deltas;
        o->node_a = node_renum[s->node_a];
        o->node_b = node_renum[s->node_b];
        o->len_m = s->len_m;
        o->n_pts = (uint16_t)(s->to - s->from + 1);
        o->region = s->region;
        o->access = w->access;
        o->hclass = w->hclass;
        o->flags = w->flags;
        for (uint32_t k = s->from + 1; k <= s->to; k++) {
            int64_t dlat = (int64_t)b->pts[k].lat - b->pts[k - 1].lat, dlon = (int64_t)b->pts[k].lon - b->pts[k - 1].lon;
            if (dlat < INT16_MIN || dlat > INT16_MAX || dlon < INT16_MIN || dlon > INT16_MAX) {
                rc = finish_fail(err, cap, "a delta does not fit (the densifier failed)", CB_E_INVALID);
                goto out;
            }
            deltas[n_deltas].dlat = (int16_t)dlat;
            deltas[n_deltas].dlon = (int16_t)dlon;
            n_deltas++;
        }
        adj_count[o->node_a]++;
        if (o->node_b != o->node_a) adj_count[o->node_b]++;
        if (s->min_lat < gmin_lat) gmin_lat = s->min_lat;
        if (s->max_lat > gmax_lat) gmax_lat = s->max_lat;
        if (s->min_lon < gmin_lon) gmin_lon = s->min_lon;
        if (s->max_lon > gmax_lon) gmax_lon = s->max_lon;
        if (w->access & ADV_ACC_BIKE) total[ADV_T_BIKE] += s->len_m;
        if (w->access & ADV_ACC_FOOT) total[ADV_T_FOOT] += s->len_m;
        if (w->access) total[ADV_T_ALL] += s->len_m;
    }
    // Nodes and adjacency (CSR), capped per node.
    uint32_t n_adj = 0;
    for (uint32_t i = 0; i < next_node; i++) {
        if (adj_count[i] > ADV_NODE_MAX_ADJ) adj_count[i] = ADV_NODE_MAX_ADJ;
        adj_off[i] = n_adj;
        n_adj += adj_count[i];
    }
    adj_off[next_node] = n_adj;
    adj = malloc((size_t)(n_adj ? n_adj : 1) * sizeof *adj);
    if (!adj) { rc = CB_E_NOMEM; goto out; }
    memset(adj_count, 0, (size_t)next_node * sizeof *adj_count);
    for (uint32_t i = 0; i < n_segs; i++) {
        uint32_t ends[2] = { out_segs[i].node_a, out_segs[i].node_b };
        for (int e = 0; e < (ends[0] == ends[1] ? 1 : 2); e++) {
            uint32_t nd = ends[e];
            if (adj_count[nd] < adj_off[nd + 1] - adj_off[nd]) adj[adj_off[nd] + adj_count[nd]++] = i;
        }
    }
    for (uint32_t i = 0; i < n_nodes; i++) {
        if (node_renum[i] == UINT32_MAX) continue;
        adv_node *o = &out_nodes[node_renum[i]];
        o->lat = b->pts[node_pt[i]].lat;
        o->lon = b->pts[node_pt[i]].lon;
        o->adj_off = adj_off[node_renum[i]];
        o->n_adj = (uint16_t)adj_count[node_renum[i]];
    }
    // 5. The cell grid over the bounding box.
    uint32_t cx0, cy0, cx1, cy1;
    {
        adv_pt nw = { gmax_lat, gmin_lon }, se = { gmin_lat, gmax_lon };
        adv_tile_of(nw, ADV_CELL_ZOOM, &cx0, &cy0);
        adv_tile_of(se, ADV_CELL_ZOOM, &cx1, &cy1);
    }
    uint32_t cells_w = cx1 - cx0 + 1, cells_h = cy1 - cy0 + 1;
    uint64_t n_cells = (uint64_t)cells_w * cells_h;
    if (n_cells > (1u << 26)) { rc = finish_fail(err, cap, "the extract is too wide for the cell grid", CB_E_FULL); goto out; }
    cell_count = calloc(n_cells, sizeof *cell_count);
    cell_off = calloc(n_cells + 1, sizeof *cell_off);
    cells = calloc(n_cells, sizeof *cells);
    if (!cell_count || !cell_off || !cells) { rc = CB_E_NOMEM; goto out; }
    uint64_t n_cell_segs = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            for (uint64_t c = 0; c < n_cells; c++) { cell_off[c] = (uint32_t)n_cell_segs; n_cell_segs += cell_count[c]; }
            if (n_cell_segs > UINT32_MAX) { rc = finish_fail(err, cap, "too many cell entries", CB_E_FULL); goto out; }
            cell_segs = malloc((size_t)(n_cell_segs ? n_cell_segs : 1) * sizeof *cell_segs);
            if (!cell_segs) { rc = CB_E_NOMEM; goto out; }
            memset(cell_count, 0, (size_t)n_cells * sizeof *cell_count);
        }
        for (uint32_t i = 0; i < n_segs; i++) {
            const adv_seg *o = &out_segs[i];
            uint32_t x0, y0, x1, y1;
            adv_pt nw = { o->max_lat, o->min_lon }, se = { o->min_lat, o->max_lon };
            adv_tile_of(nw, ADV_CELL_ZOOM, &x0, &y0);
            adv_tile_of(se, ADV_CELL_ZOOM, &x1, &y1);
            for (uint32_t y = y0; y <= y1; y++)
                for (uint32_t x = x0; x <= x1; x++) {
                    uint64_t c = (uint64_t)(y - cy0) * cells_w + (x - cx0);
                    if (pass == 0) cell_count[c]++;
                    else cell_segs[cell_off[c] + cell_count[c]++] = i;   // ascending: i rises
                }
        }
    }
    for (uint64_t c = 0; c < n_cells; c++) {
        if (cell_count[c] > ADV_SEG_MAX_PER_CELL) { rc = finish_fail(err, cap, "a cell lists too many segments", CB_E_FULL); goto out; }
        cells[c].off = cell_off[c];
        cells[c].n = cell_count[c];
    }
    // 6. Regions.
    regions = calloc(b->n_regions ? b->n_regions : 1, sizeof *regions);
    if (!regions) { rc = CB_E_NOMEM; goto out; }
    for (uint32_t i = 0; i < b->n_regions; i++) {
        adv_region *r = &regions[i];
        memcpy(r->name, b->regions[i].name, ADV_REGION_NAME);
        r->min_lat = r->min_lon = INT32_MAX;
        r->max_lat = r->max_lon = INT32_MIN;
        for (uint32_t k = 0; k < b->regions[i].n_ring_ids; k++) {
            const bring *g = &b->rings[b->regions[i].rings[k]];
            if (g->min_lat < r->min_lat) r->min_lat = g->min_lat;
            if (g->max_lat > r->max_lat) r->max_lat = g->max_lat;
            if (g->min_lon < r->min_lon) r->min_lon = g->min_lon;
            if (g->max_lon > r->max_lon) r->max_lon = g->max_lon;
        }
        if (r->min_lat > r->max_lat) r->min_lat = r->max_lat = r->min_lon = r->max_lon = 0;
    }
    {
        double *rlen = calloc((size_t)(b->n_regions ? b->n_regions : 1) * ADV_T_N, sizeof *rlen);
        double *clat = calloc(b->n_regions ? b->n_regions : 1, sizeof *clat), *clon = calloc(b->n_regions ? b->n_regions : 1, sizeof *clon);
        if (!rlen || !clat || !clon) { free(rlen); free(clat); free(clon); rc = CB_E_NOMEM; goto out; }
        for (uint32_t i = 0; i < n_segs; i++) {
            uint16_t r = out_segs[i].region;
            if (r == ADV_NO_REGION) continue;
            regions[r].n_segs++;
            if (out_segs[i].access & ADV_ACC_BIKE) rlen[r * ADV_T_N + ADV_T_BIKE] += out_segs[i].len_m;
            if (out_segs[i].access & ADV_ACC_FOOT) rlen[r * ADV_T_N + ADV_T_FOOT] += out_segs[i].len_m;
            if (out_segs[i].access) rlen[r * ADV_T_N + ADV_T_ALL] += out_segs[i].len_m;
            clat[r] += segs[i].mid.lat;
            clon[r] += segs[i].mid.lon;
        }
        for (uint32_t r = 0; r < b->n_regions; r++) {
            for (int t = 0; t < ADV_T_N; t++) regions[r].len_m[t] = (float)rlen[r * ADV_T_N + t];
            if (regions[r].n_segs) {
                regions[r].c_lat = (int32_t)(clat[r] / regions[r].n_segs);
                regions[r].c_lon = (int32_t)(clon[r] / regions[r].n_segs);
            } else {
                regions[r].c_lat = (regions[r].min_lat + regions[r].max_lat) / 2;
                regions[r].c_lon = (regions[r].min_lon + regions[r].max_lon) / 2;
            }
        }
        free(rlen);
        free(clat);
        free(clon);
    }
    // 7. Lay the file out and write it.
    adv_graph_hdr h;
    memset(&h, 0, sizeof h);
    h.magic = ADV_GRAPH_MAGIC;
    h.version = ADV_GRAPH_VERSION;
    h.n_segs = n_segs;
    h.n_deltas = n_deltas;
    h.n_nodes = next_node;
    h.n_adj = n_adj;
    h.n_cell_segs = (uint32_t)n_cell_segs;
    h.n_regions = b->n_regions;
    h.cell_zoom = ADV_CELL_ZOOM;
    h.cell_x0 = cx0;
    h.cell_y0 = cy0;
    h.cells_w = cells_w;
    h.cells_h = cells_h;
    h.min_lat = gmin_lat; h.min_lon = gmin_lon; h.max_lat = gmax_lat; h.max_lon = gmax_lon;
    for (int t = 0; t < ADV_T_N; t++) h.total_len_m[t] = total[t];
    snprintf(h.source, sizeof h.source, "%s", b->source);
    uint64_t at = ALIGN8(sizeof h);
    h.off_segs = at; at = ALIGN8(at + (uint64_t)n_segs * sizeof(adv_seg));
    h.off_deltas = at; at = ALIGN8(at + (uint64_t)n_deltas * sizeof(adv_delta));
    h.off_nodes = at; at = ALIGN8(at + (uint64_t)next_node * sizeof(adv_node));
    h.off_adj = at; at = ALIGN8(at + (uint64_t)n_adj * 4);
    h.off_cells = at; at = ALIGN8(at + n_cells * sizeof(adv_cell));
    h.off_cell_segs = at; at = ALIGN8(at + n_cell_segs * 4);
    h.off_regions = at; at = ALIGN8(at + (uint64_t)b->n_regions * sizeof(adv_region));
    h.file_size = at;
    if ((rc = ob_put(&ob, &h, sizeof h)) != CB_OK || (rc = ob_pad(&ob)) != CB_OK) goto out;
    if ((rc = ob_put(&ob, out_segs, (size_t)n_segs * sizeof(adv_seg))) != CB_OK || (rc = ob_pad(&ob)) != CB_OK) goto out;
    if ((rc = ob_put(&ob, deltas, (size_t)n_deltas * sizeof(adv_delta))) != CB_OK || (rc = ob_pad(&ob)) != CB_OK) goto out;
    if ((rc = ob_put(&ob, out_nodes, (size_t)next_node * sizeof(adv_node))) != CB_OK || (rc = ob_pad(&ob)) != CB_OK) goto out;
    if ((rc = ob_put(&ob, adj, (size_t)n_adj * 4)) != CB_OK || (rc = ob_pad(&ob)) != CB_OK) goto out;
    if ((rc = ob_put(&ob, cells, (size_t)n_cells * sizeof(adv_cell))) != CB_OK || (rc = ob_pad(&ob)) != CB_OK) goto out;
    if ((rc = ob_put(&ob, cell_segs, (size_t)n_cell_segs * 4)) != CB_OK || (rc = ob_pad(&ob)) != CB_OK) goto out;
    if ((rc = ob_put(&ob, regions, (size_t)b->n_regions * sizeof(adv_region))) != CB_OK || (rc = ob_pad(&ob)) != CB_OK) goto out;
    if (ob.n != at) { rc = finish_fail(err, cap, "layout arithmetic", CB_E_STATE); goto out; }
    h.crc = crc32c(0, ob.p + sizeof h, ob.n - sizeof h);
    memcpy(ob.p, &h, sizeof h);
    if ((rc = adv_graph_check(ob.p, ob.n, err, cap)) != CB_OK) goto out;
    {
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (!f || fwrite(ob.p, 1, ob.n, f) != ob.n || fclose(f) != 0) {
            if (f) fclose(f);
            if (cap) snprintf(err, cap, "graph build: writing %s: %s", tmp, strerror(errno));
            rc = CB_E_IO;
            goto out;
        }
        if (rename(tmp, path) != 0) {
            if (cap) snprintf(err, cap, "graph build: rename to %s: %s", path, strerror(errno));
            rc = CB_E_IO;
            goto out;
        }
    }
    b->stats.segs = n_segs;
    b->stats.nodes = next_node;
    b->stats.deltas = n_deltas;
    b->stats.cells = (uint32_t)n_cells;
    b->stats.regions = b->n_regions;
    b->stats.cell_segs = (uint32_t)n_cell_segs;
    for (int t = 0; t < ADV_T_N; t++) b->stats.total_len_m[t] = total[t];
out:
    free(is_node); free(node_of_pt); free(uses); free(segs); free(node_pt); free(node_renum);
    free(adj_count); free(adj_off); free(adj); free(cell_count); free(cell_off); free(cell_segs); free(cells);
    free(out_segs); free(out_nodes); free(deltas); free(regions); free(ob.p);
    if (rc == CB_E_NOMEM && cap) snprintf(err, cap, "graph build: out of memory");
    return rc;
}

void adv_build_stats_get(const adv_build *b, adv_build_stats *out) { *out = b->stats; }
