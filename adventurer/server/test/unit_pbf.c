// unit_pbf.c - the importer end to end on a PBF this test writes itself (a protobuf encoder and
// zlib, forty lines), then the reader's refusals and the tag rules one case at a time.
#include "graph.h"
#include "import.h"
#include "lib_fakegraph.h"
#include "osm_tags.h"
#include "pbf.h"
#include "t.h"
#include "util/err.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

// ---- a protobuf encoder ---------------------------------------------------------------------------

typedef struct {
    uint8_t *p;
    size_t   n, cap;
} buf;

static void b_put(buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->cap ? b->cap * 2 : 1024) + n;
        b->p = realloc(b->p, b->cap);
        if (!b->p) abort();
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}
static void b_varint(buf *b, uint64_t v) {
    do {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        if (v) byte |= 0x80;
        b_put(b, &byte, 1);
    } while (v);
}
static uint64_t zz(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63); }
static void b_tag(buf *b, uint32_t num, uint32_t wire) { b_varint(b, (uint64_t)num << 3 | wire); }
static void b_u(buf *b, uint32_t num, uint64_t v) { b_tag(b, num, 0); b_varint(b, v); }
static void b_s(buf *b, uint32_t num, int64_t v) { b_tag(b, num, 0); b_varint(b, zz(v)); }
static void b_bytes(buf *b, uint32_t num, const void *p, size_t n) { b_tag(b, num, 2); b_varint(b, n); b_put(b, p, n); }
static void b_str(buf *b, uint32_t num, const char *s) { b_bytes(b, num, s, strlen(s)); }
static void b_msg(buf *b, uint32_t num, const buf *m) { b_bytes(b, num, m->p, m->n); }
// A packed sint64 field, delta-coded when `delta` (what the OSM columns are).
static void b_packed_s64(buf *b, uint32_t num, const int64_t *v, int n, bool delta) {
    buf t = { 0 };
    int64_t prev = 0;
    for (int i = 0; i < n; i++) {
        b_varint(&t, zz(delta ? v[i] - prev : v[i]));
        prev = v[i];
    }
    b_msg(b, num, &t);
    free(t.p);
}
static void b_packed_u32(buf *b, uint32_t num, const uint32_t *v, int n) {
    buf t = { 0 };
    for (int i = 0; i < n; i++) b_varint(&t, v[i]);
    b_msg(b, num, &t);
    free(t.p);
}

// ---- the test world ---------------------------------------------------------------------------------

typedef struct {
    const char *s[64];
    int n;
} strtab;
static uint32_t sid(strtab *t, const char *s) {
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->s[i], s) == 0) return (uint32_t)i;
    if (t->n >= 64) abort();
    t->s[t->n] = s;
    return (uint32_t)t->n++;
}
static void b_strtab(buf *block, const strtab *t) {
    buf st = { 0 };
    for (int i = 0; i < t->n; i++) b_str(&st, 1, t->s[i]);
    b_msg(block, 1, &st);
    free(st.p);
}

typedef struct {
    int64_t id;
    int32_t lat, lon;
} tnode;
typedef struct {
    int64_t     id;
    const char *kv[12];   // k, v pairs, NULL-ended
    int64_t     refs[8];
    int         n_refs;
} tway;
typedef struct {
    int64_t     id;
    const char *kv[12];
    int64_t     ways[4];
    const char *roles[4];
    int         n;
} trel;

#define LAT0 418000000
#define LON0 (-714100000)
static const tnode dense_nodes[] = {
    { 100, LAT0, LON0 }, { 101, LAT0, LON0 + 100000 }, { 102, LAT0 + 100000, LON0 + 100000 }, { 103, LAT0 + 100000, LON0 },
    { 1, LAT0 + 20000, LON0 + 20000 }, { 2, LAT0 + 20000, LON0 + 40000 }, { 3, LAT0 + 20000, LON0 + 60000 },
    { 4, LAT0 + 50000, LON0 + 20000 }, { 5, LAT0 + 50000, LON0 + 40000 },
    { 8, LAT0 + 70000, LON0 + 20000 }, { 9, LAT0 + 70000, LON0 + 40000 },
    { 20, LAT0 + 10000, LON0 + 20000 },
    { 200, LAT0 + 500000, LON0 }, { 201, LAT0 + 500000, LON0 + 100000 }, { 202, LAT0 + 600000, LON0 + 100000 }, { 203, LAT0 + 600000, LON0 },
};
// Outside the ring, written as plain Nodes in a block with granularity 1 and raw a half unit
// off, so the reader's rounding is checked too.
static const tnode plain_nodes[] = { { 6, LAT0 + 300000, LON0 + 20000 }, { 7, LAT0 + 300000, LON0 + 40000 } };
static const tway ways[] = {
    { 10, { "highway", "residential", "oneway", "yes", NULL }, { 1, 2, 3 }, 3 },
    { 11, { "highway", "footway", "bicycle", "yes", NULL }, { 4, 5 }, 2 },
    { 12, { "highway", "cycleway", NULL }, { 6, 7 }, 2 },
    { 13, { "highway", "motorway", NULL }, { 8, 9 }, 2 },
    { 14, { "highway", "service", "service", "driveway", NULL }, { 1, 20 }, 2 },
    { 15, { "highway", "residential", NULL }, { 3, 999 }, 2 },
    { 21, { "boundary", "administrative", NULL }, { 100, 101, 102 }, 3 },
    { 22, { "boundary", "administrative", NULL }, { 100, 103, 102 }, 3 },   // reversed against 21
    { 23, { NULL }, { 200, 201 }, 2 },
    { 24, { NULL }, { 202, 203 }, 2 },
    { 25, { NULL }, { 201, 202 }, 2 },
    { 26, { NULL }, { 203, 200 }, 2 },
};
static const trel rels[] = {
    { 1, { "type", "boundary", "boundary", "administrative", "admin_level", "8", "name", "Testville", NULL }, { 21, 22 }, { "outer", "outer" }, 2 },
    { 2, { "boundary", "administrative", "admin_level", "8", "name", "Nowhere", NULL }, { 23, 24 }, { "outer", "" }, 2 },
    { 3, { "boundary", "administrative", "admin_level", "6", "name", "Testshire", NULL }, { 21, 22 }, { "outer", "outer" }, 2 },
    // The same ring again one level up (a town coterminous with its village), and a closed ring
    // far away (another county's town): what --admin-levels 7,8 and --within sort out.
    { 4, { "boundary", "administrative", "admin_level", "7", "name", "Testtown", NULL }, { 21, 22 }, { "outer", "outer" }, 2 },
    { 5, { "boundary", "administrative", "admin_level", "8", "name", "Elsewhere", NULL }, { 23, 25, 24, 26 }, { "outer", "outer", "outer", "outer" }, 4 },
};
#define N_DENSE ((int)(sizeof dense_nodes / sizeof dense_nodes[0]))
#define N_PLAIN ((int)(sizeof plain_nodes / sizeof plain_nodes[0]))
#define N_WAYS ((int)(sizeof ways / sizeof ways[0]))
#define N_RELS ((int)(sizeof rels / sizeof rels[0]))

static void b_kv(buf *m, strtab *t, const char *const *kv) {
    uint32_t keys[8], vals[8];
    int n = 0;
    for (int i = 0; kv[i]; i += 2) {
        keys[n] = sid(t, kv[i]);
        vals[n] = sid(t, kv[i + 1]);
        n++;
    }
    if (n) {
        b_packed_u32(m, 2, keys, n);
        b_packed_u32(m, 3, vals, n);
    }
}

// Blob framing: [u32 BE len][BlobHeader][Blob]. zlib when `zip`, else raw.
static void b_blob(buf *file, const char *type, const buf *block, bool zip, size_t *zoff) {
    buf blob = { 0 };
    if (zip) {
        uLongf clen = compressBound((uLong)block->n);
        uint8_t *c = malloc(clen);
        if (!c || compress(c, &clen, block->p, (uLong)block->n) != Z_OK) abort();
        b_u(&blob, 2, block->n);
        b_bytes(&blob, 3, c, clen);
        free(c);
    } else {
        b_msg(&blob, 1, block);
    }
    buf hdr = { 0 };
    b_str(&hdr, 1, type);
    b_u(&hdr, 3, blob.n);
    uint8_t len[4] = { (uint8_t)(hdr.n >> 24), (uint8_t)(hdr.n >> 16), (uint8_t)(hdr.n >> 8), (uint8_t)hdr.n };
    b_put(file, len, 4);
    b_put(file, hdr.p, hdr.n);
    if (zoff) *zoff = file->n + blob.n / 2;   // somewhere inside the compressed bytes
    b_put(file, blob.p, blob.n);
    free(hdr.p);
    free(blob.p);
}

// The whole file: a header blob, dense nodes (zlib), plain nodes (raw, another frame), ways and
// relations (zlib). *zoff: an offset inside a zlib stream, for the corruption test.
static void make_pbf(buf *file, size_t *zoff) {
    buf hb = { 0 };
    b_str(&hb, 4, "OsmSchema-V0.6");
    b_str(&hb, 4, "DenseNodes");
    b_blob(file, "OSMHeader", &hb, false, NULL);
    free(hb.p);
    {
        strtab t = { { "" }, 1 };
        buf block = { 0 }, group = { 0 }, dense = { 0 };
        int64_t ids[N_DENSE], lats[N_DENSE], lons[N_DENSE];
        uint32_t kv[N_DENSE * 3];
        int nkv = 0;
        for (int i = 0; i < N_DENSE; i++) {
            ids[i] = dense_nodes[i].id;
            lats[i] = dense_nodes[i].lat;   // granularity 100 nanodeg = 1e-7 deg: raw is our unit
            lons[i] = dense_nodes[i].lon;
            if (i == 0) { kv[nkv++] = sid(&t, "place"); kv[nkv++] = sid(&t, "corner"); }   // one tagged node
            kv[nkv++] = 0;
        }
        b_packed_s64(&dense, 1, ids, N_DENSE, true);
        b_packed_s64(&dense, 8, lats, N_DENSE, true);
        b_packed_s64(&dense, 9, lons, N_DENSE, true);
        b_packed_u32(&dense, 10, kv, nkv);
        b_msg(&group, 2, &dense);
        b_strtab(&block, &t);
        b_msg(&block, 2, &group);
        b_u(&block, 17, 100);
        b_blob(file, "OSMData", &block, true, zoff);
        free(block.p); free(group.p); free(dense.p);
    }
    {
        strtab t = { { "" }, 1 };
        buf block = { 0 }, group = { 0 };
        for (int i = 0; i < N_PLAIN; i++) {
            buf node = { 0 };
            b_s(&node, 1, plain_nodes[i].id);
            b_s(&node, 8, (int64_t)plain_nodes[i].lat * 100 + 51);   // rounds up one unit
            b_s(&node, 9, (int64_t)plain_nodes[i].lon * 100 - 51);   // rounds down one
            b_msg(&group, 1, &node);
            free(node.p);
        }
        b_strtab(&block, &t);
        b_msg(&block, 2, &group);
        b_u(&block, 17, 1);
        b_blob(file, "OSMData", &block, false, NULL);
        free(block.p); free(group.p);
    }
    {
        strtab t = { { "" }, 1 };
        buf block = { 0 }, wgroup = { 0 }, rgroup = { 0 };
        for (int i = 0; i < N_WAYS; i++) {
            buf w = { 0 };
            b_u(&w, 1, (uint64_t)ways[i].id);
            b_kv(&w, &t, ways[i].kv);
            b_packed_s64(&w, 8, ways[i].refs, ways[i].n_refs, true);
            b_msg(&wgroup, 3, &w);
            free(w.p);
        }
        for (int i = 0; i < N_RELS; i++) {
            buf r = { 0 };
            uint32_t roles[4], types[4];
            for (int k = 0; k < rels[i].n; k++) { roles[k] = sid(&t, rels[i].roles[k]); types[k] = PBF_M_WAY; }
            b_u(&r, 1, (uint64_t)rels[i].id);
            b_kv(&r, &t, rels[i].kv);
            b_packed_u32(&r, 8, roles, rels[i].n);
            b_packed_s64(&r, 9, rels[i].ways, rels[i].n, true);
            b_packed_u32(&r, 10, types, rels[i].n);
            b_msg(&rgroup, 4, &r);
            free(r.p);
        }
        b_strtab(&block, &t);
        b_msg(&block, 2, &wgroup);
        b_msg(&block, 2, &rgroup);
        b_blob(file, "OSMData", &block, true, NULL);
        free(block.p); free(wgroup.p); free(rgroup.p);
    }
}

static void write_file(const char *path, const uint8_t *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n || fclose(f) != 0) abort();
}

static const adv_seg *seg_of_class(const adv_graph *g, uint8_t hclass) {
    for (uint32_t i = 0; i < g->h->n_segs; i++)
        if (g->segs[i].hclass == hclass) return &g->segs[i];
    return NULL;
}

static void test_import(const char *pbf_path) {
    char graph_path[512];
    fg_tmp_path(graph_path, sizeof graph_path, "unit_pbf");
    adv_import_stats st;
    char err[256] = "";
    adv_import_opts o = { 0 };
    int rc = adv_import(pbf_path, graph_path, &o, &st, err, sizeof err);
    T_CHECK_MSG(rc == CB_OK, "rc=%d err=%s", rc, err);
    if (rc != CB_OK) return;
    T_CHECK_MSG(st.ways_read == N_WAYS, "%llu", (unsigned long long)st.ways_read);
    T_CHECK_MSG(st.roads_kept == 4, "%llu", (unsigned long long)st.roads_kept);
    T_CHECK_MSG(st.member_ways == 6, "%llu", (unsigned long long)st.member_ways);
    T_CHECK_MSG(st.ways_dropped == 1, "%llu", (unsigned long long)st.ways_dropped);
    T_CHECK_MSG(st.nodes_read == N_DENSE + N_PLAIN, "%llu", (unsigned long long)st.nodes_read);
    T_CHECK_MSG(st.nodes_needed == 16, "%llu", (unsigned long long)st.nodes_needed);
    T_CHECK_MSG(st.nodes_kept == 15, "%llu", (unsigned long long)st.nodes_kept);
    T_CHECK(st.relations_read == N_RELS);
    T_CHECK_MSG(st.relations_wanted == 3, "%llu", (unsigned long long)st.relations_wanted);
    T_CHECK_MSG(st.rings_closed == 2, "%llu", (unsigned long long)st.rings_closed);
    // "Nowhere": two member ways that never meet, so each starts a chain that stays open.
    T_CHECK_MSG(st.rings_dropped == 2, "%llu", (unsigned long long)st.rings_dropped);
    T_CHECK(!st.fallback_used);
    T_CHECK(st.build.segs == 3);
    adv_graph g;
    rc = adv_graph_open(graph_path, &g, err, sizeof err);
    T_CHECK_MSG(rc == CB_OK, "open: %s", err);
    if (rc != CB_OK) return;
    T_CHECK(g.h->n_segs == 3);
    T_CHECK(g.h->n_regions == 2);
    T_CHECK(strcmp(g.regions[0].name, "Testville") == 0 && g.regions[0].level == 8);
    T_CHECK(strcmp(g.regions[1].name, "Elsewhere") == 0 && g.regions[1].n_segs == 0);
    T_CHECK_MSG(g.regions[0].n_segs == 2, "n_segs=%u", g.regions[0].n_segs);
    const adv_seg *res = seg_of_class(&g, ADV_HC_RESIDENTIAL), *foot = seg_of_class(&g, ADV_HC_FOOTWAY), *cyc = seg_of_class(&g, ADV_HC_CYCLEWAY);
    T_CHECK(res && foot && cyc);
    T_CHECK(!seg_of_class(&g, ADV_HC_MOTORWAY) && !seg_of_class(&g, ADV_HC_SERVICE));
    if (res) {
        T_CHECK(res->access == (ADV_ACC_BIKE | ADV_ACC_FOOT));
        T_CHECK(res->flags == ADV_SEGF_ONEWAY);
        T_CHECK(res->region == 0);
        T_CHECK_MSG(res->n_pts == 3, "n_pts=%u", res->n_pts);
        // 2 x 0.002 deg of longitude at 41.8 N: 2 x 166 m.
        T_CHECK_MSG(fabs(res->len_m - 331.6) < 2, "len=%f", res->len_m);
        adv_pt pts[8];
        T_CHECK(adv_seg_points(&g, (uint32_t)(res - g.segs), pts, 8) == 3);
        T_CHECK(pts[0].lat == LAT0 + 20000 && pts[0].lon == LON0 + 20000);
        T_CHECK(pts[2].lat == LAT0 + 20000 && pts[2].lon == LON0 + 60000);
    }
    if (foot) {
        T_CHECK_MSG(foot->access == (ADV_ACC_BIKE | ADV_ACC_FOOT), "access=%u", foot->access);
        T_CHECK(foot->flags == 0);
        T_CHECK(foot->region == 0);
    }
    if (cyc) {
        T_CHECK(cyc->access == (ADV_ACC_BIKE | ADV_ACC_FOOT));
        T_CHECK_MSG(cyc->region == ADV_NO_REGION, "region=%u", cyc->region);
        // The plain nodes' rounding: unit*100+51 nanodegrees is the next unit up, -51 the one
        // down (truncation would give the unit itself).
        T_CHECK_MSG(cyc->lat == LAT0 + 300000 + 1 && cyc->lon == LON0 + 20000 - 1, "lat=%d lon=%d", cyc->lat, cyc->lon);
    }
    if (res && foot && cyc) {
        T_CHECK(fabs(g.regions[0].len_m[ADV_T_ALL] - (res->len_m + foot->len_m)) < 0.01);
        T_CHECK(fabs(g.h->total_len_m[ADV_T_ALL] - (res->len_m + foot->len_m + cyc->len_m)) < 0.01);
    }
    adv_graph_close(&g);
    // admin_level 6: the county relation, built from the same two ways.
    o.admin_levels = 1u << 6;
    rc = adv_import(pbf_path, graph_path, &o, &st, err, sizeof err);
    T_CHECK_MSG(rc == CB_OK, "rc=%d err=%s", rc, err);
    T_CHECK(st.relations_wanted == 1 && st.rings_closed == 1 && st.rings_dropped == 0);
    T_CHECK(st.member_ways == 2);
    T_CHECK(adv_graph_open(graph_path, &g, err, sizeof err) == CB_OK);
    T_CHECK(g.h->n_regions == 1 && strcmp(g.regions[0].name, "Testshire") == 0 && g.regions[0].n_segs == 2 && g.regions[0].level == 6);
    adv_graph_close(&g);
    // Levels 7 and 8 together: the town and the village share one ring, and the village (the
    // innermost) gets the roads; the town is there with none.
    o.admin_levels = (1u << 7) | (1u << 8);
    rc = adv_import(pbf_path, graph_path, &o, &st, err, sizeof err);
    T_CHECK_MSG(rc == CB_OK, "rc=%d err=%s", rc, err);
    T_CHECK_MSG(st.relations_wanted == 4, "%llu", (unsigned long long)st.relations_wanted);
    T_CHECK(adv_graph_open(graph_path, &g, err, sizeof err) == CB_OK);
    T_CHECK_MSG(g.h->n_regions == 3, "n_regions=%u", g.h->n_regions);
    {
        const adv_region *town = NULL, *village = NULL;
        for (uint32_t i = 0; i < g.h->n_regions; i++) {
            if (strcmp(g.regions[i].name, "Testtown") == 0) town = &g.regions[i];
            if (strcmp(g.regions[i].name, "Testville") == 0) village = &g.regions[i];
        }
        T_CHECK(town && village);
        if (town && village) {
            T_CHECK_MSG(town->level == 7 && town->n_segs == 0, "town level %u n_segs %u", town->level, town->n_segs);
            T_CHECK_MSG(village->level == 8 && village->n_segs == 2, "village level %u n_segs %u", village->level, village->n_segs);
        }
        T_CHECK(g.h->n_segs == 3);   // the cycleway outside every region is still in the graph
    }
    adv_graph_close(&g);
    // Level 7 alone: the town gets them.
    o.admin_levels = 1u << 7;
    rc = adv_import(pbf_path, graph_path, &o, &st, err, sizeof err);
    T_CHECK_MSG(rc == CB_OK, "rc=%d err=%s", rc, err);
    T_CHECK(adv_graph_open(graph_path, &g, err, sizeof err) == CB_OK);
    T_CHECK(g.h->n_regions == 1 && strcmp(g.regions[0].name, "Testtown") == 0 && g.regions[0].n_segs == 2 && g.regions[0].level == 7);
    adv_graph_close(&g);
    // The fence: level-8 regions within the level-6 "Testshire". Testville is inside it,
    // Elsewhere (a closed ring far away) is not and is dropped, and the cycleway outside every
    // kept region leaves the graph with it.
    o.admin_levels = 1u << 8;
    o.within_level = 6;
    o.within_names = "Nosuch, Testshire ,Other";
    rc = adv_import(pbf_path, graph_path, &o, &st, err, sizeof err);
    T_CHECK_MSG(rc == CB_OK, "rc=%d err=%s", rc, err);
    T_CHECK_MSG(st.fences == 1, "fences=%llu", (unsigned long long)st.fences);
    T_CHECK_MSG(st.regions_outside == 1, "outside=%llu", (unsigned long long)st.regions_outside);
    T_CHECK_MSG(st.rings_closed == 2 && st.rings_dropped == 2, "closed %llu dropped %llu", (unsigned long long)st.rings_closed, (unsigned long long)st.rings_dropped);
    T_CHECK_MSG(st.build.segs == 2 && st.build.segs_unregioned == 1, "segs %u unregioned %u", st.build.segs, st.build.segs_unregioned);
    T_CHECK(adv_graph_open(graph_path, &g, err, sizeof err) == CB_OK);
    T_CHECK(g.h->n_segs == 2 && g.h->n_regions == 1 && strcmp(g.regions[0].name, "Testville") == 0 && g.regions[0].n_segs == 2);
    T_CHECK(!seg_of_class(&g, ADV_HC_CYCLEWAY));
    adv_graph_close(&g);
    // A fence that names no boundary of the extract is refused, not silently everything.
    o.within_names = "Nosuch";
    rc = adv_import(pbf_path, graph_path, &o, &st, err, sizeof err);
    T_CHECK_MSG(rc == CB_E_NOT_FOUND, "rc=%d err=%s", rc, err);
    o.within_level = 0;
    o.within_names = NULL;
    // admin_level 5: nothing, so the fallback region holds every road.
    o.admin_levels = 1u << 5;
    o.fallback_region = "Everywhere";
    rc = adv_import(pbf_path, graph_path, &o, &st, err, sizeof err);
    T_CHECK_MSG(rc == CB_OK, "rc=%d err=%s", rc, err);
    T_CHECK(st.relations_wanted == 0 && st.fallback_used);
    T_CHECK(adv_graph_open(graph_path, &g, err, sizeof err) == CB_OK);
    T_CHECK(g.h->n_regions == 1 && strcmp(g.regions[0].name, "Everywhere") == 0);
    T_CHECK_MSG(g.regions[0].n_segs == 3, "n_segs=%u", g.regions[0].n_segs);
    adv_graph_close(&g);
    unlink(graph_path);
}

static int count_cb_node(void *ud, int64_t id, int32_t lat, int32_t lon, const pbf_tag *tags, int n_tags) {
    (void)id; (void)lat; (void)lon; (void)tags; (void)n_tags;
    (*(int *)ud)++;
    return CB_OK;
}
static int stop_cb_way(void *ud, int64_t id, const pbf_tag *tags, int n_tags, const int64_t *refs, int n_refs) {
    (void)ud; (void)id; (void)tags; (void)n_tags; (void)refs; (void)n_refs;
    return CB_E_BUSY;   // a callback's code comes straight out
}

static void test_refusals(const uint8_t *good, size_t n, size_t zoff) {
    char err[128];
    pbf_cb cb = { 0 };
    uint8_t *bad = malloc(n);
    if (!bad) abort();
    // The good file reads clean; a node pass sees every node.
    int nodes = 0;
    cb.node = count_cb_node;
    T_CHECK(pbf_read_mem(good, n, PBF_NODES, &cb, &nodes, err, sizeof err) == CB_OK);
    T_CHECK_MSG(nodes == N_DENSE + N_PLAIN, "nodes=%d", nodes);
    nodes = 0;
    T_CHECK(pbf_read_mem(good, n, PBF_WAYS, &cb, &nodes, err, sizeof err) == CB_OK);
    T_CHECK(nodes == 0);   // a ways pass never decodes a node
    cb.way = stop_cb_way;
    T_CHECK(pbf_read_mem(good, n, PBF_WAYS, &cb, &nodes, err, sizeof err) == CB_E_BUSY);
    cb.way = NULL;
    // Truncated at every cut that lands inside a frame or a header.
    T_CHECK(pbf_read_mem(good, n / 2, PBF_NODES, &cb, &nodes, err, sizeof err) == CB_E_INVALID);
    T_CHECK(pbf_read_mem(good, 3, PBF_NODES, &cb, &nodes, err, sizeof err) == CB_E_INVALID);
    T_CHECK(pbf_read_mem(good, n - 1, PBF_NODES, &cb, &nodes, err, sizeof err) == CB_E_INVALID);
    // A blob header length past the cap.
    memcpy(bad, good, n);
    bad[0] = 0; bad[1] = 0x10; bad[2] = 0; bad[3] = 0;
    T_CHECK(pbf_read_mem(bad, n, PBF_NODES, &cb, &nodes, err, sizeof err) == CB_E_INVALID);
    T_CHECK(strstr(err, "header length") != NULL);
    // A zlib stream that does not inflate.
    memcpy(bad, good, n);
    for (int i = 0; i < 8; i++) bad[zoff + i] ^= 0xA5;
    T_CHECK(pbf_read_mem(bad, n, PBF_NODES, &cb, &nodes, err, sizeof err) == CB_E_INVALID);
    T_CHECK_MSG(strstr(err, "zlib") != NULL, "err=%s", err);
    // A ways pass inflates it too (the group kinds are inside), so it is refused there as well.
    T_CHECK(pbf_read_mem(bad, n, PBF_WAYS, &cb, &nodes, err, sizeof err) == CB_E_INVALID);
    // An empty file is a clean nothing; a lone zero byte is truncated.
    T_CHECK(pbf_read_mem(good, 0, PBF_NODES, &cb, &nodes, err, sizeof err) == CB_OK);
    T_CHECK(pbf_read_mem(good, 1, PBF_NODES, &cb, &nodes, err, sizeof err) == CB_E_INVALID);
    free(bad);
    // A file that is not there, and a graph that cannot be built from a header-only file.
    T_CHECK(pbf_read_file("/nonexistent/x.osm.pbf", PBF_NODES, &cb, NULL, err, sizeof err) == CB_E_IO);
}

// ---- the tag rules ---------------------------------------------------------------------------------------

static bool road(const char *const *kv, uint8_t *acc, uint8_t *hc, uint8_t *fl) {
    pbf_tag tags[8];
    int n = 0;
    for (int i = 0; kv[i]; i += 2) { tags[n].k = kv[i]; tags[n].v = kv[i + 1]; n++; }
    *acc = *hc = *fl = 0xFF;
    return osm_way_road(tags, n, acc, hc, fl);
}
#define ROAD(...) road((const char *const[]){ __VA_ARGS__, NULL }, &acc, &hc, &fl)

static void test_tags(void) {
    uint8_t acc, hc, fl;
    T_CHECK(!ROAD("highway", "motorway"));
    // Sidewalks and crossings duplicate the street beside them: not roads of their own.
    T_CHECK(!ROAD("highway", "footway", "footway", "sidewalk"));
    T_CHECK(!ROAD("highway", "footway", "footway", "crossing"));
    T_CHECK(ROAD("highway", "footway", "footway", "yes") && acc == ADV_ACC_FOOT && hc == ADV_HC_FOOTWAY);
    T_CHECK(!ROAD("highway", "motorway_link", "bicycle", "yes"));
    T_CHECK(ROAD("highway", "trunk") && acc == ADV_ACC_BIKE && hc == ADV_HC_TRUNK && fl == 0);
    T_CHECK(!ROAD("highway", "trunk", "bicycle", "no"));
    T_CHECK(ROAD("highway", "residential") && acc == (ADV_ACC_BIKE | ADV_ACC_FOOT) && hc == ADV_HC_RESIDENTIAL);
    T_CHECK(ROAD("highway", "residential", "oneway", "yes") && fl == ADV_SEGF_ONEWAY);
    T_CHECK(ROAD("highway", "residential", "oneway", "-1") && fl == ADV_SEGF_ONEWAY);
    T_CHECK(ROAD("highway", "residential", "oneway", "no") && fl == 0);
    T_CHECK(ROAD("highway", "primary_link") && hc == ADV_HC_PRIMARY);
    T_CHECK(ROAD("highway", "living_street") && hc == ADV_HC_LIVING);
    T_CHECK(ROAD("highway", "road") && hc == ADV_HC_OTHER);
    T_CHECK(ROAD("highway", "service") && acc == (ADV_ACC_BIKE | ADV_ACC_FOOT) && hc == ADV_HC_SERVICE);
    T_CHECK(!ROAD("highway", "service", "service", "driveway"));
    T_CHECK(!ROAD("highway", "service", "service", "parking_aisle"));
    T_CHECK(ROAD("highway", "service", "service", "alley"));
    T_CHECK(ROAD("highway", "track") && acc == (ADV_ACC_BIKE | ADV_ACC_FOOT) && hc == ADV_HC_TRACK);
    T_CHECK(ROAD("highway", "path") && acc == ADV_ACC_FOOT && hc == ADV_HC_PATH);
    T_CHECK(ROAD("highway", "path", "bicycle", "designated") && acc == (ADV_ACC_BIKE | ADV_ACC_FOOT));
    T_CHECK(ROAD("highway", "bridleway", "bicycle", "yes") && acc == (ADV_ACC_BIKE | ADV_ACC_FOOT) && hc == ADV_HC_PATH);
    T_CHECK(ROAD("highway", "footway") && acc == ADV_ACC_FOOT && hc == ADV_HC_FOOTWAY);
    T_CHECK(ROAD("highway", "footway", "bicycle", "yes") && acc == (ADV_ACC_BIKE | ADV_ACC_FOOT));
    T_CHECK(ROAD("highway", "footway", "bicycle", "dismount") && acc == ADV_ACC_FOOT);
    T_CHECK(ROAD("highway", "pedestrian") && acc == ADV_ACC_FOOT && hc == ADV_HC_PEDESTRIAN);
    T_CHECK(ROAD("highway", "steps", "bicycle", "yes") && acc == ADV_ACC_FOOT && hc == ADV_HC_STEPS);
    T_CHECK(ROAD("highway", "cycleway") && acc == (ADV_ACC_BIKE | ADV_ACC_FOOT) && hc == ADV_HC_CYCLEWAY);
    T_CHECK(ROAD("highway", "cycleway", "foot", "no") && acc == ADV_ACC_BIKE);
    T_CHECK(!ROAD("highway", "residential", "access", "private"));
    T_CHECK(!ROAD("highway", "residential", "access", "no"));
    T_CHECK(ROAD("highway", "residential", "access", "private", "foot", "yes") && acc == ADV_ACC_FOOT);
    T_CHECK(ROAD("highway", "residential", "access", "no", "bicycle", "permissive") && acc == ADV_ACC_BIKE);
    T_CHECK(ROAD("highway", "residential", "access", "destination") && acc == (ADV_ACC_BIKE | ADV_ACC_FOOT));
    T_CHECK(ROAD("highway", "residential", "bicycle", "no") && acc == ADV_ACC_FOOT);
    T_CHECK(ROAD("highway", "residential", "foot", "no") && acc == ADV_ACC_BIKE);
    T_CHECK(!ROAD("highway", "residential", "bicycle", "no", "foot", "no"));
    T_CHECK(!ROAD("highway", "primary", "motorroad", "yes"));
    T_CHECK(!ROAD("highway", "pedestrian", "area", "yes"));
    T_CHECK(!ROAD("highway", "busway"));
    T_CHECK(!ROAD("highway", "proposed"));
    T_CHECK(!ROAD("highway", "construction"));
    T_CHECK(!ROAD("highway", "bus_stop"));
    T_CHECK(!ROAD("building", "yes"));
    // Relations.
    char name[ADV_REGION_NAME];
    pbf_tag r1[] = { { "boundary", "administrative" }, { "admin_level", "8" }, { "name", "Warwick" }, { "name:en", "Warwick (en)" } };
    T_CHECK(osm_relation_region(r1, 4, 1u << 8, name) == 8 && strcmp(name, "Warwick (en)") == 0);
    T_CHECK(osm_relation_region(r1, 3, 1u << 8, name) == 8 && strcmp(name, "Warwick") == 0);
    T_CHECK(osm_relation_region(r1, 3, (1u << 7) | (1u << 8), name) == 8);
    T_CHECK(!osm_relation_region(r1, 4, 1u << 6, name));
    T_CHECK(!osm_relation_region(r1, 4, (1u << 6) | (1u << 7), name));
    T_CHECK(!osm_relation_region(r1, 2, 1u << 8, name));   // no name
    pbf_tag r2[] = { { "boundary", "postal_code" }, { "admin_level", "8" }, { "name", "02886" } };
    T_CHECK(!osm_relation_region(r2, 3, 1u << 8, name));
    pbf_tag r3[] = { { "boundary", "administrative" }, { "admin_level", "8x" }, { "name", "X" } };
    T_CHECK(!osm_relation_region(r3, 3, 1u << 8, name));
    pbf_tag r5[] = { { "boundary", "administrative" }, { "admin_level", "40" }, { "name", "X" } };
    T_CHECK(!osm_relation_region(r5, 3, 0xFFFFFFFFu, name));   // past ADV_REGION_MAX_LEVEL
    // A long name is cut to fit, on a character boundary: 46 ASCII bytes then a 2-byte char.
    char longname[80];
    memset(longname, 'a', 46);
    memcpy(longname + 46, "\xC3\xA9\xC3\xA9zzz", 7);
    longname[53] = 0;
    pbf_tag r4[] = { { "boundary", "administrative" }, { "admin_level", "8" }, { "name", longname } };
    T_CHECK(osm_relation_region(r4, 3, 1u << 8, name) == 8);
    T_CHECK_MSG(strlen(name) == 46, "len=%zu", strlen(name));   // the 2-byte char at 46..47 would not fit whole
}

int main(void) {
    buf file = { 0 };
    size_t zoff = 0;
    make_pbf(&file, &zoff);
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    char pbf_path[512];
    snprintf(pbf_path, sizeof pbf_path, "%s/adv-unit_pbf-%d.osm.pbf", dir, (int)getpid());
    write_file(pbf_path, file.p, file.n);
    const char *keep = getenv("UNIT_PBF_OUT");   // a seed for the fuzzer's corpus
    if (keep && *keep) write_file(keep, file.p, file.n);
    test_import(pbf_path);
    test_refusals(file.p, file.n, zoff);
    test_tags();
    unlink(pbf_path);
    free(file.p);
    return t_done("unit_pbf");
}
