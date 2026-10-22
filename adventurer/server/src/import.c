// import.c - import.h.
#include "import.h"

#include "osm_tags.h"
#include "pbf.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { KEEP_ROAD = 1, KEEP_MEMBER = 2 };

typedef struct {
    int64_t  id;
    uint64_t ref_off;
    uint32_t n_refs;
    uint32_t name;   // into imp.names; 0 for none
    uint8_t  keep;   // KEEP_* bits
    uint8_t  access, hclass, flags;
} way_rec;

typedef struct {
    char     name[ADV_REGION_NAME];
    uint64_t mem_off;
    uint32_t n_mem;
    uint8_t  level;   // the region's admin_level, or 0 for a fence (import.h within_level)
} rel_rec;

typedef struct {
    int64_t way_id;
    uint8_t inner;
} mem_rec;

typedef struct {
    way_rec  *ways;  uint64_t n_ways, cap_ways;
    int64_t  *refs;  uint64_t n_refs, cap_refs;
    rel_rec  *rels;  uint64_t n_rels, cap_rels;
    mem_rec  *mems;  uint64_t n_mems, cap_mems;
    char     *names; uint64_t n_names, cap_names;   // every road's name, NUL-terminated, in order ([0] is "")
    uint32_t *order;         // way indices sorted by id, for the member lookup
    int64_t  *need;          // sorted, unique node ids the kept ways name
    adv_pt   *pts;           // the coordinates of need[i]
    uint8_t  *have;          // whether pass 2 saw need[i]
    uint64_t  n_need;
    uint32_t  admin_levels;
    int       within_level;
    const char *within_names;
    adv_import_stats *st;
    char   *err;
    size_t  cap;
} imp;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int fail(imp *m, int rc, const char *what) {
    if (m->cap) snprintf(m->err, m->cap, "import: %s", what);
    return rc;
}

// Grows an array to hold n + need, doubling, never past max.
static int grow(void **arr, uint64_t n, uint64_t *cap, uint64_t need, uint64_t max, size_t each) {
    if (n + need <= *cap) return CB_OK;
    if (n + need > max) return CB_E_FULL;
    uint64_t nc = *cap ? *cap : 4096;
    while (nc < n + need) nc *= 2;
    if (nc > max) nc = max;
    void *np = realloc(*arr, (size_t)nc * each);
    if (!np) return CB_E_NOMEM;
    *arr = np;
    *cap = nc;
    return CB_OK;
}
#define GROW(m, field, need, max) grow((void **)&(m)->field, (m)->n_##field, &(m)->cap_##field, (need), (max), sizeof *(m)->field)

// ---- pass 1: ways and relations ------------------------------------------------------------------

static int on_way(void *ud, int64_t id, const pbf_tag *tags, int n_tags, const int64_t *refs, int n_refs) {
    imp *m = ud;
    m->st->ways_read++;
    if (n_refs < 1) return CB_OK;   // nothing to connect, nothing to need
    uint8_t access = 0, hclass = 0, flags = 0;
    bool road = osm_way_road(tags, n_tags, &access, &hclass, &flags);
    if (road) m->st->roads_kept++;
    uint32_t name = 0;
    const char *nm = road ? osm_way_name(tags, n_tags) : NULL;
    if (nm) {
        size_t nl = strlen(nm);
        if (nl > ADV_NAME_MAX) nl = ADV_NAME_MAX;   // the builder cuts it on a character boundary
        if (m->n_names == 0) {   // [0] is the empty name
            int rc0 = GROW(m, names, 1, ADV_IMPORT_MAX_REFS);
            if (rc0 != CB_OK) return rc0;
            m->names[m->n_names++] = 0;
        }
        int rc1 = GROW(m, names, nl + 1, ADV_IMPORT_MAX_REFS);
        if (rc1 != CB_OK) return rc1 == CB_E_FULL ? fail(m, rc1, "too many road names") : rc1;
        if (m->n_names + nl + 1 > UINT32_MAX) return fail(m, CB_E_FULL, "too many road names");
        name = (uint32_t)m->n_names;
        memcpy(m->names + m->n_names, nm, nl);
        m->names[m->n_names + nl] = 0;
        m->n_names += nl + 1;
    }
    // Every way is remembered (id, where its refs are) until the relations have been read,
    // since a boundary's member ways are not roads and are only known as members afterwards.
    int rc;
    if ((rc = GROW(m, ways, 1, ADV_IMPORT_MAX_WAYS)) != CB_OK) return rc == CB_E_FULL ? fail(m, rc, "too many ways") : rc;
    if ((rc = GROW(m, refs, (uint64_t)n_refs, ADV_IMPORT_MAX_REFS)) != CB_OK) return rc == CB_E_FULL ? fail(m, rc, "too many node refs") : rc;
    way_rec *w = &m->ways[m->n_ways++];
    w->id = id;
    w->ref_off = m->n_refs;
    w->n_refs = (uint32_t)n_refs;
    w->keep = road ? KEEP_ROAD : 0;
    w->name = name;
    w->access = access;
    w->hclass = hclass;
    w->flags = flags;
    memcpy(m->refs + m->n_refs, refs, (size_t)n_refs * sizeof *refs);
    m->n_refs += (uint64_t)n_refs;
    return CB_OK;
}

// Is name one of the comma-separated names in list (spaces around a comma ignored)?
static bool name_listed(const char *list, const char *name) {
    size_t nl = strlen(name);
    for (const char *p = list ? list : ""; *p;) {
        while (*p == ' ') p++;
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        while (n > 0 && p[n - 1] == ' ') n--;
        if (n == nl && memcmp(p, name, n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static int on_relation(void *ud, int64_t id, const pbf_tag *tags, int n_tags, const pbf_member *members, int n_members) {
    imp *m = ud;
    (void)id;
    m->st->relations_read++;
    char name[ADV_REGION_NAME];
    int level = osm_relation_region(tags, n_tags, m->admin_levels, name);
    if (level) m->st->relations_wanted++;
    else if (m->within_level && osm_relation_region(tags, n_tags, 1u << m->within_level, name) && name_listed(m->within_names, name)) level = 0;
    else return CB_OK;
    int rc;
    if ((rc = GROW(m, rels, 1, ADV_IMPORT_MAX_MEMBERS)) != CB_OK) return rc == CB_E_FULL ? fail(m, rc, "too many relations") : rc;
    rel_rec *r = &m->rels[m->n_rels++];
    memcpy(r->name, name, sizeof name);
    r->mem_off = m->n_mems;
    r->n_mem = 0;
    r->level = (uint8_t)level;
    for (int i = 0; i < n_members; i++) {
        if (members[i].type != PBF_M_WAY) continue;
        const char *role = members[i].role;
        bool outer = role[0] == 0 || strcmp(role, "outer") == 0, inner = strcmp(role, "inner") == 0;
        if (!outer && !inner) continue;   // admin_centre, label, subarea: not the shape
        if ((rc = GROW(m, mems, 1, ADV_IMPORT_MAX_MEMBERS)) != CB_OK) return rc == CB_E_FULL ? fail(m, rc, "too many relation members") : rc;
        m->mems[m->n_mems].way_id = members[i].id;
        m->mems[m->n_mems].inner = inner;
        m->n_mems++;
        r->n_mem++;
    }
    return CB_OK;
}

// ---- lookups ------------------------------------------------------------------------------------

static const imp *sort_ctx;   // qsort has no context argument; sorting is single-threaded here
static int order_cmp(const void *a, const void *b) {
    int64_t x = sort_ctx->ways[*(const uint32_t *)a].id, y = sort_ctx->ways[*(const uint32_t *)b].id;
    return x < y ? -1 : x > y;
}
static int i64_cmp(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

static int sort_ways(imp *m) {
    free(m->order);
    m->order = malloc((size_t)(m->n_ways ? m->n_ways : 1) * sizeof *m->order);
    if (!m->order) return CB_E_NOMEM;
    for (uint64_t i = 0; i < m->n_ways; i++) m->order[i] = (uint32_t)i;
    sort_ctx = m;
    qsort(m->order, (size_t)m->n_ways, sizeof *m->order, order_cmp);
    return CB_OK;
}

// The way with this id, or -1.
static int64_t way_find(const imp *m, int64_t id) {
    uint64_t lo = 0, hi = m->n_ways;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        int64_t v = m->ways[m->order[mid]].id;
        if (v < id) lo = mid + 1;
        else if (v > id) hi = mid;
        else return (int64_t)m->order[mid];
    }
    return -1;
}

// The index of a needed node id, or -1.
static int64_t need_find(const imp *m, int64_t id) {
    uint64_t lo = 0, hi = m->n_need;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        int64_t v = m->need[mid];
        if (v < id) lo = mid + 1;
        else if (v > id) hi = mid;
        else return (int64_t)mid;
    }
    return -1;
}

// After pass 1: mark member ways, drop the rest, list the node ids the kept ways name.
static int settle_pass1(imp *m) {
    int rc = sort_ways(m);
    if (rc != CB_OK) return rc;
    for (uint64_t i = 0; i < m->n_mems; i++) {
        int64_t w = way_find(m, m->mems[i].way_id);
        if (w >= 0) {
            if (!m->ways[w].keep) m->st->member_ways++;
            m->ways[w].keep |= KEEP_MEMBER;
        }
    }
    // Compact ways and refs in place: insertion order is ref_off order, so a forward walk
    // never overwrites refs it has yet to read.
    uint64_t nw = 0, nr = 0;
    for (uint64_t i = 0; i < m->n_ways; i++) {
        way_rec w = m->ways[i];
        if (!w.keep) continue;
        if (nr != w.ref_off) memmove(m->refs + nr, m->refs + w.ref_off, (size_t)w.n_refs * sizeof *m->refs);
        w.ref_off = nr;
        nr += w.n_refs;
        m->ways[nw++] = w;
    }
    m->n_ways = nw;
    m->n_refs = nr;
    if ((rc = sort_ways(m)) != CB_OK) return rc;
    // The needed node ids: every kept ref, sorted, unique.
    if (nr > ADV_IMPORT_MAX_NODES) return fail(m, CB_E_FULL, "too many nodes needed");
    m->need = malloc((size_t)(nr ? nr : 1) * sizeof *m->need);
    if (!m->need) return CB_E_NOMEM;
    memcpy(m->need, m->refs, (size_t)nr * sizeof *m->need);
    qsort(m->need, (size_t)nr, sizeof *m->need, i64_cmp);
    uint64_t u = 0;
    for (uint64_t i = 0; i < nr; i++)
        if (u == 0 || m->need[u - 1] != m->need[i]) m->need[u++] = m->need[i];
    m->n_need = u;
    m->st->nodes_needed = u;
    m->pts = calloc(u ? u : 1, sizeof *m->pts);
    m->have = calloc(u ? u : 1, 1);
    if (!m->pts || !m->have) return CB_E_NOMEM;
    return CB_OK;
}

// ---- pass 2: nodes ---------------------------------------------------------------------------------

static int on_node(void *ud, int64_t id, int32_t lat, int32_t lon, const pbf_tag *tags, int n_tags) {
    imp *m = ud;
    (void)tags;
    (void)n_tags;
    m->st->nodes_read++;
    int64_t i = need_find(m, id);
    if (i < 0) return CB_OK;
    if (!m->have[i]) m->st->nodes_kept++;
    m->have[i] = 1;
    m->pts[i].lat = lat;
    m->pts[i].lon = lon;
    return CB_OK;
}

// ---- the build ---------------------------------------------------------------------------------------

// A relation's member ways, as the ring assembler sees them.
typedef struct {
    int64_t way;     // index into m->ways, or -1 when the file lacks it
    int64_t a, b;    // its first and last node id
    uint8_t inner, used;
} rw;

// One assembled ring: its points lie in ring_scratch.pts[off..off+n).
typedef struct {
    uint64_t off, n;
    uint8_t  inner;
} aring;

typedef struct {
    int64_t *ids;  uint64_t n_ids, cap_ids;    // the ring's node ids as it is assembled
    adv_pt  *pts;  uint64_t n_pts, cap_pts;    // then every ring's points
    rw      *ws;   uint64_t n_ws, cap_ws;
    aring   *rings; uint64_t n_rings, cap_rings;
} ring_scratch;

static int push_way_ids(ring_scratch *s, const imp *m, const way_rec *w, bool reverse, bool skip_first) {
    uint64_t n = w->n_refs;
    int rc = grow((void **)&s->ids, s->n_ids, &s->cap_ids, n, ADV_IMPORT_MAX_REFS, sizeof *s->ids);
    if (rc != CB_OK) return rc;
    const int64_t *r = m->refs + w->ref_off;
    for (uint64_t i = skip_first ? 1 : 0; i < n; i++) s->ids[s->n_ids++] = reverse ? r[n - 1 - i] : r[i];
    return CB_OK;
}

// Assembles this relation's member ways into closed rings (outer and inner separately: a hole's
// way never continues a shell's) into s->rings, each ring's points in s->pts. Open rings and
// rings through a node the file lacks are counted and dropped. The loop over members is bounded
// by the member count, since each turn consumes one member or stops.
static int assemble_rings(imp *m, const rel_rec *r, ring_scratch *s, uint64_t *closed, uint64_t *dropped) {
    int rc;
    s->n_ws = 0;
    s->n_pts = 0;
    s->n_rings = 0;
    if ((rc = grow((void **)&s->ws, 0, &s->cap_ws, r->n_mem, ADV_IMPORT_MAX_MEMBERS, sizeof *s->ws)) != CB_OK) return rc;
    for (uint32_t i = 0; i < r->n_mem; i++) {
        const mem_rec *mr = &m->mems[r->mem_off + i];
        rw *w = &s->ws[s->n_ws++];
        w->way = way_find(m, mr->way_id);
        w->inner = mr->inner;
        w->used = 0;
        if (w->way >= 0 && m->ways[w->way].n_refs >= 2) {
            const way_rec *wr = &m->ways[w->way];
            w->a = m->refs[wr->ref_off];
            w->b = m->refs[wr->ref_off + wr->n_refs - 1];
        } else {
            w->way = -1;   // missing from the extract: every ring through it stays open
            w->used = 1;
        }
    }
    for (uint64_t i = 0; i < s->n_ws; i++) {
        if (s->ws[i].used) continue;
        rw *start = &s->ws[i];
        start->used = 1;
        s->n_ids = 0;
        if ((rc = push_way_ids(s, m, &m->ways[start->way], false, false)) != CB_OK) return rc;
        int64_t first = start->a, cur = start->b;
        bool ring_closed = first == cur;
        for (uint64_t turn = 0; turn < s->n_ws && !ring_closed; turn++) {
            uint64_t j;
            for (j = 0; j < s->n_ws; j++) {
                rw *w = &s->ws[j];
                if (w->used || w->inner != start->inner) continue;
                if (w->a == cur || w->b == cur) break;
            }
            if (j == s->n_ws) break;   // nothing continues from here: open
            rw *w = &s->ws[j];
            w->used = 1;
            bool reverse = w->b == cur;
            if ((rc = push_way_ids(s, m, &m->ways[w->way], reverse, true)) != CB_OK) return rc;
            cur = reverse ? w->a : w->b;
            ring_closed = cur == first;
        }
        if (!ring_closed) { (*dropped)++; continue; }
        // Ids to points; the closing repeat of the first node is dropped (the builder closes).
        uint64_t n = s->n_ids;
        if (n >= 2 && s->ids[n - 1] == s->ids[0]) n--;
        if ((rc = grow((void **)&s->pts, s->n_pts, &s->cap_pts, n, ADV_IMPORT_MAX_REFS, sizeof *s->pts)) != CB_OK) return rc;
        bool complete = n >= 3;
        for (uint64_t k = 0; k < n && complete; k++) {
            int64_t ni = need_find(m, s->ids[k]);
            if (ni < 0 || !m->have[ni]) complete = false;
            else s->pts[s->n_pts + k] = m->pts[ni];
        }
        if (!complete) { (*dropped)++; continue; }
        if ((rc = grow((void **)&s->rings, s->n_rings, &s->cap_rings, 1, ADV_IMPORT_MAX_MEMBERS, sizeof *s->rings)) != CB_OK) return rc;
        aring g = { s->n_pts, n, start->inner };
        s->rings[s->n_rings++] = g;
        s->n_pts += n;
        (*closed)++;
    }
    return CB_OK;
}

// ---- the fence (import.h within_level) ---------------------------------------------------------

typedef struct {
    adv_pt  *pts;  uint64_t n_pts, cap_pts;
    aring   *rings; uint64_t n_rings, cap_rings;
    int32_t  min_lat, min_lon, max_lat, max_lon;   // of every ring
} fence;

// Even-odd over one ring (int64 arithmetic: no overflow at int32 coordinates); the builder's rule.
static bool ring_has(const adv_pt *v, uint64_t n, adv_pt p) {
    bool in = false;
    for (uint64_t i = 0, j = n - 1; i < n; j = i++) {
        int64_t yi = v[i].lat, yj = v[j].lat, xi = v[i].lon, xj = v[j].lon;
        if ((yi > p.lat) != (yj > p.lat)) {
            int64_t num = (xj - xi) * ((int64_t)p.lat - yi), den = yj - yi;
            int64_t lhs = ((int64_t)p.lon - xi) * den;
            if (den > 0 ? lhs < num : lhs > num) in = !in;
        }
    }
    return in;
}

static bool fence_has(const fence *f, adv_pt p) {
    if (p.lat < f->min_lat || p.lat > f->max_lat || p.lon < f->min_lon || p.lon > f->max_lon) return false;
    int crossings = 0;
    for (uint64_t k = 0; k < f->n_rings; k++)
        if (ring_has(f->pts + f->rings[k].off, f->rings[k].n, p)) crossings++;
    return crossings & 1;
}

// The assembled rings of one fence relation, appended (every fence is one area: a point is inside
// the fence when inside an odd number of all their rings, which holds since counties never overlap).
static int fence_add(fence *f, const ring_scratch *s) {
    int rc;
    if ((rc = grow((void **)&f->pts, f->n_pts, &f->cap_pts, s->n_pts, ADV_IMPORT_MAX_REFS, sizeof *f->pts)) != CB_OK) return rc;
    if ((rc = grow((void **)&f->rings, f->n_rings, &f->cap_rings, s->n_rings, ADV_IMPORT_MAX_MEMBERS, sizeof *f->rings)) != CB_OK) return rc;
    for (uint64_t k = 0; k < s->n_rings; k++) {
        aring g = { f->n_pts + s->rings[k].off, s->rings[k].n, s->rings[k].inner };
        f->rings[f->n_rings++] = g;
    }
    for (uint64_t i = 0; i < s->n_pts; i++) {
        adv_pt p = s->pts[i];
        f->pts[f->n_pts++] = p;
        if (p.lat < f->min_lat) f->min_lat = p.lat;
        if (p.lat > f->max_lat) f->max_lat = p.lat;
        if (p.lon < f->min_lon) f->min_lon = p.lon;
        if (p.lon > f->max_lon) f->max_lon = p.lon;
    }
    return CB_OK;
}

// Points strictly inside a ring: on each of K horizontal lines through its box, the midpoint of
// the first pair of edge crossings (even-odd: between the first and the second crossing is
// inside). Returns how many were written to out (at most K).
#define FENCE_LINES 7
static int ring_interior(const adv_pt *v, uint64_t n, adv_pt *out) {
    int32_t min_lat = INT32_MAX, max_lat = INT32_MIN;
    for (uint64_t i = 0; i < n; i++) {
        if (v[i].lat < min_lat) min_lat = v[i].lat;
        if (v[i].lat > max_lat) max_lat = v[i].lat;
    }
    int k = 0;
    for (int line = 1; line <= FENCE_LINES; line++) {
        int32_t lat = (int32_t)(min_lat + ((int64_t)max_lat - min_lat) * line / (FENCE_LINES + 1));
        double x0 = 0, x1 = 0;   // the two smallest crossings
        int found = 0;
        for (uint64_t i = 0, j = n - 1; i < n; j = i++) {
            int64_t yi = v[i].lat, yj = v[j].lat;
            if ((yi > lat) == (yj > lat)) continue;
            double x = v[i].lon + ((double)lat - yi) * ((double)v[j].lon - v[i].lon) / (double)(yj - yi);
            if (found == 0 || x < x0) { x1 = found ? x0 : x1; x0 = x; }
            else if (found == 1 || x < x1) x1 = x;
            if (found < 2) found++;
        }
        if (found < 2 || x1 - x0 < 2) continue;
        adv_pt p = { lat, (int32_t)((x0 + x1) / 2) };
        out[k++] = p;
    }
    return k;
}

// Is the region whose rings are in s inside the fence? When more of its outer rings' interior
// points are in than out. Interior points, never vertices: a borough coterminous with its county
// has every vertex ON the fence, where the even-odd test answers either way.
static bool fence_holds(const fence *f, const ring_scratch *s) {
    uint64_t in = 0, total = 0;
    adv_pt pts[FENCE_LINES];
    for (uint64_t k = 0; k < s->n_rings; k++) {
        if (s->rings[k].inner) continue;
        int n = ring_interior(s->pts + s->rings[k].off, s->rings[k].n, pts);
        for (int i = 0; i < n; i++, total++)
            if (fence_has(f, pts[i])) in++;
    }
    return total > 0 && in * 2 > total;
}

// Adds a region relation's rings to the builder (the region is made at its first ring), unless a
// fence is set and the region lies outside it.
static int add_relation(imp *m, adv_build *b, const rel_rec *r, ring_scratch *s, const fence *f) {
    int rc = assemble_rings(m, r, s, &m->st->rings_closed, &m->st->rings_dropped);
    if (rc != CB_OK || s->n_rings == 0) return rc;
    if (f && !fence_holds(f, s)) { m->st->regions_outside++; return CB_OK; }
    int region = adv_build_region(b, r->name, r->level);
    if (region < 0) return region;
    for (uint64_t k = 0; k < s->n_rings; k++) {
        if (s->rings[k].n > INT32_MAX) return CB_E_FULL;
        if ((rc = adv_build_region_ring(b, region, s->pts + s->rings[k].off, (int)s->rings[k].n)) != CB_OK) return rc;
    }
    return CB_OK;
}

// "dir/rhode-island-latest.osm.pbf" -> "rhode-island".
static void basename_region(const char *path, char *out, size_t cap) {
    const char *s = strrchr(path, '/');
    s = s ? s + 1 : path;
    size_t n = strcspn(s, ".");
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
    size_t l = strlen(out);
    if (l > 7 && strcmp(out + l - 7, "-latest") == 0) out[l - 7] = 0;
    if (!out[0]) snprintf(out, cap, "region");
}

static int build(imp *m, adv_build *b, const char *pbf_path, const adv_import_opts *o) {
    int rc = CB_OK;
    adv_pt *pts = NULL;
    uint64_t cap_pts = 0;
    int32_t min_lat = INT32_MAX, min_lon = INT32_MAX, max_lat = INT32_MIN, max_lon = INT32_MIN;
    ring_scratch s;
    memset(&s, 0, sizeof s);
    fence f = { 0 };
    f.min_lat = f.min_lon = INT32_MAX;
    f.max_lat = f.max_lon = INT32_MIN;
    for (uint64_t i = 0; i < m->n_ways; i++) {
        const way_rec *w = &m->ways[i];
        if (!(w->keep & KEEP_ROAD)) continue;
        if (w->n_refs < 2) { m->st->ways_dropped++; continue; }
        if ((rc = grow((void **)&pts, 0, &cap_pts, w->n_refs, ADV_IMPORT_MAX_REFS, sizeof *pts)) != CB_OK) goto out;
        const int64_t *refs = m->refs + w->ref_off;
        bool complete = true;
        for (uint32_t k = 0; k < w->n_refs && complete; k++) {
            int64_t ni = need_find(m, refs[k]);
            if (ni < 0 || !m->have[ni]) complete = false;
            else pts[k] = m->pts[ni];
        }
        if (!complete) { m->st->ways_dropped++; continue; }
        for (uint32_t k = 0; k < w->n_refs; k++) {
            if (pts[k].lat < min_lat) min_lat = pts[k].lat;
            if (pts[k].lat > max_lat) max_lat = pts[k].lat;
            if (pts[k].lon < min_lon) min_lon = pts[k].lon;
            if (pts[k].lon > max_lon) max_lon = pts[k].lon;
        }
        if ((rc = adv_build_add_way(b, refs, pts, (int)w->n_refs, w->access, w->hclass, w->flags, w->name ? m->names + w->name : NULL)) != CB_OK) {
            fail(m, rc, "the builder refused a way");
            goto out;
        }
    }
    // The fence first: every named boundary at within_level, assembled like a region's rings.
    if (m->within_level) {
        uint64_t closed = 0, dropped = 0;   // a fence's rings are not a region's
        for (uint64_t i = 0; i < m->n_rels; i++) {
            if (m->rels[i].level) continue;
            if ((rc = assemble_rings(m, &m->rels[i], &s, &closed, &dropped)) != CB_OK) { fail(m, rc, "assembling a fence"); goto out; }
            if (s.n_rings == 0) continue;
            if ((rc = fence_add(&f, &s)) != CB_OK) { fail(m, rc, "the fence"); goto out; }
            m->st->fences++;
        }
        if (m->st->fences == 0) { rc = fail(m, CB_E_NOT_FOUND, "no fence closed: none of the within names is a boundary at that level in this extract"); goto out; }
        adv_build_only_regioned(b, true);
    }
    for (uint64_t i = 0; i < m->n_rels; i++) {
        if (!m->rels[i].level) continue;
        if ((rc = add_relation(m, b, &m->rels[i], &s, m->within_level ? &f : NULL)) != CB_OK) { fail(m, rc, "adding a region"); goto out; }
    }
    if (m->st->rings_closed == 0 && min_lat <= max_lat) {
        // Nothing to belong to: one region over every road, so the achievements have a
        // denominator and the page has a name.
        char name[ADV_REGION_NAME];
        if (o->fallback_region && *o->fallback_region) snprintf(name, sizeof name, "%s", o->fallback_region);
        else basename_region(pbf_path, name, sizeof name);
        int region = adv_build_region(b, name, 1);
        if (region < 0) { rc = fail(m, region, "the fallback region"); goto out; }
        adv_pt ring[4] = { { min_lat - 1, min_lon - 1 }, { min_lat - 1, max_lon + 1 }, { max_lat + 1, max_lon + 1 }, { max_lat + 1, min_lon - 1 } };
        if ((rc = adv_build_region_ring(b, region, ring, 4)) != CB_OK) { fail(m, rc, "the fallback ring"); goto out; }
        m->st->fallback_used = true;
    }
out:
    free(pts);
    free(s.ids);
    free(s.pts);
    free(s.ws);
    free(s.rings);
    free(f.pts);
    free(f.rings);
    return rc;
}

int adv_import(const char *pbf_path, const char *graph_path, const adv_import_opts *o_in, adv_import_stats *stats, char *err, size_t cap) {
    adv_import_opts o = { 0 };
    if (o_in) o = *o_in;
    if (o.admin_levels == 0) o.admin_levels = 1u << 8;
    o.admin_levels &= (2u << ADV_REGION_MAX_LEVEL) - 2;   // bits 1..ADV_REGION_MAX_LEVEL
    if (o.within_level < 0 || o.within_level > ADV_REGION_MAX_LEVEL) return CB_E_INVALID;
    adv_import_stats st;
    memset(&st, 0, sizeof st);
    imp m;
    memset(&m, 0, sizeof m);
    m.admin_levels = o.admin_levels;
    m.within_level = o.within_level;
    m.within_names = o.within_names;
    m.st = &st;
    m.err = err;
    m.cap = cap;
    if (cap) err[0] = 0;
    adv_build *b = NULL;
    int rc;
    double t0 = now_s();
    {
        pbf_cb cb = { 0 };
        cb.way = on_way;
        cb.relation = on_relation;
        if ((rc = pbf_read_file(pbf_path, PBF_WAYS | PBF_RELATIONS, &cb, &m, err, cap)) != CB_OK) goto out;
    }
    if ((rc = settle_pass1(&m)) != CB_OK) goto out;
    st.s_pass1 = now_s() - t0;
    t0 = now_s();
    {
        pbf_cb cb = { 0 };
        cb.node = on_node;
        if ((rc = pbf_read_file(pbf_path, PBF_NODES | PBF_SKIP_TAGS, &cb, &m, err, cap)) != CB_OK) goto out;
    }
    st.s_pass2 = now_s() - t0;
    t0 = now_s();
    {
        char source[64];
        const char *base = strrchr(pbf_path, '/');
        snprintf(source, sizeof source, "%s", base ? base + 1 : pbf_path);
        b = adv_build_new(source);
        if (!b) { rc = CB_E_NOMEM; goto out; }
    }
    if ((rc = build(&m, b, pbf_path, &o)) != CB_OK) goto out;
    if ((rc = adv_build_finish(b, graph_path, err, cap)) != CB_OK) goto out;
    adv_build_stats_get(b, &st.build);
    st.s_build = now_s() - t0;
out:
    adv_build_free(b);
    free(m.ways); free(m.refs); free(m.rels); free(m.mems); free(m.order); free(m.need); free(m.pts); free(m.have); free(m.names);
    if (rc == CB_E_NOMEM && cap && !err[0]) snprintf(err, cap, "import: out of memory");
    if (rc != CB_OK && cap && !err[0]) snprintf(err, cap, "import: %s", cb_strerror(rc));
    if (stats) *stats = st;
    return rc;
}
