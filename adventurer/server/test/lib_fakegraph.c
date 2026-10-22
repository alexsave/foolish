// lib_fakegraph.c - lib_fakegraph.h.
#include "lib_fakegraph.h"

#include "util/err.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void opts_fill(fg_opts *o) {
    if (o->block_m <= 0) o->block_m = 120;
    if (o->shape_m == 0) o->shape_m = 40;
    if (o->sw.lat == 0 && o->sw.lon == 0) {
        o->sw.lat = (int32_t)(41.8 * ADV_DEG);
        o->sw.lon = (int32_t)(-71.4 * ADV_DEG);
    }
}

adv_pt fg_crossing(const fg_opts *o_in, int i, int j) {
    fg_opts o = *o_in;
    opts_fill(&o);
    adv_frame f = adv_frame_at(o.sw.lat);
    adv_pt p;
    p.lat = (int32_t)(o.sw.lat + j * o.block_m / f.my);
    p.lon = (int32_t)(o.sw.lon + i * o.block_m / f.mx);
    return p;
}

void fg_tmp_path(char *out, size_t cap, const char *tag) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(out, cap, "%s/adv-%s-%d-%ld.graph", dir, tag, (int)getpid(), (long)random());
}

static int64_t node_id(int i, int j) { return (int64_t)j * 100000 + i + 1; }

// One street between two crossings, with interior shape points every shape_m.
static int add_street(adv_build *b, const fg_opts *o, int i0, int j0, int i1, int j1, uint8_t access, uint8_t hclass) {
    adv_pt a = fg_crossing(o, i0, j0), c = fg_crossing(o, i1, j1);
    int64_t ids[256];
    adv_pt pts[256];
    int n = 0;
    int interior = o->shape_m > 0 ? (int)(o->block_m / o->shape_m) - 1 : 0;
    if (interior > 250) interior = 250;
    ids[n] = node_id(i0, j0);
    pts[n++] = a;
    for (int k = 1; k <= interior; k++) {
        double t = (double)k / (interior + 1);
        adv_pt m;
        m.lat = (int32_t)(a.lat + t * ((double)c.lat - a.lat));
        m.lon = (int32_t)(a.lon + t * ((double)c.lon - a.lon));
        ids[n] = -(int64_t)(1000000 + (int64_t)i0 * 1000003 + (int64_t)j0 * 1009 + (int64_t)i1 * 31 + j1 * 7 + k * 1000000007LL);
        pts[n++] = m;
    }
    ids[n] = node_id(i1, j1);
    pts[n++] = c;
    return adv_build_add_way(b, ids, pts, n, access, hclass, 0);
}

int fg_build(const fg_opts *o_in, const char *path, adv_graph *g) {
    fg_opts o = *o_in;
    opts_fill(&o);
    adv_build *b = adv_build_new("fakegraph");
    if (!b) return -1;
    int rc = CB_OK;
    for (int j = 0; j <= o.rows && rc == CB_OK; j++)
        for (int i = 0; i < o.cols && rc == CB_OK; i++) {
            bool foot = o.foot_only_rows && (j % 2 == 1);
            rc = add_street(b, &o, i, j, i + 1, j, foot ? ADV_ACC_FOOT : (ADV_ACC_BIKE | ADV_ACC_FOOT),
                            foot ? ADV_HC_FOOTWAY : ADV_HC_RESIDENTIAL);
        }
    for (int i = 0; i <= o.cols && rc == CB_OK; i++)
        for (int j = 0; j < o.rows && rc == CB_OK; j++)
            rc = add_street(b, &o, i, j, i, j + 1, ADV_ACC_BIKE | ADV_ACC_FOOT, i == 0 ? ADV_HC_PRIMARY : ADV_HC_RESIDENTIAL);
    if (rc == CB_OK && o.regions) {
        adv_pt sw = fg_crossing(&o, 0, 0), ne = fg_crossing(&o, o.cols, o.rows);
        adv_frame f = adv_frame_at(sw.lat);
        int32_t pad_lat = (int32_t)(50 / f.my), pad_lon = (int32_t)(50 / f.mx);
        int32_t mid_lon = (sw.lon + ne.lon) / 2;
        adv_pt west[4] = { { sw.lat - pad_lat, sw.lon - pad_lon }, { sw.lat - pad_lat, mid_lon }, { ne.lat + pad_lat, mid_lon }, { ne.lat + pad_lat, sw.lon - pad_lon } };
        adv_pt east[4] = { { sw.lat - pad_lat, mid_lon }, { sw.lat - pad_lat, ne.lon + pad_lon }, { ne.lat + pad_lat, ne.lon + pad_lon }, { ne.lat + pad_lat, mid_lon } };
        int w = adv_build_region(b, "West", 8), e = adv_build_region(b, "East", 8);
        if (w < 0 || e < 0) rc = CB_E_STATE;
        if (rc == CB_OK) rc = adv_build_region_ring(b, w, west, 4);
        if (rc == CB_OK) rc = adv_build_region_ring(b, e, east, 4);
    }
    char err[256] = "";
    if (rc == CB_OK) rc = adv_build_finish(b, path, err, sizeof err);
    adv_build_free(b);
    if (rc != CB_OK) {
        fprintf(stderr, "fakegraph: build failed: %s (%s)\n", cb_strerror(rc), err);
        return -1;
    }
    if ((rc = adv_graph_open(path, g, err, sizeof err)) != CB_OK) {
        fprintf(stderr, "fakegraph: open failed: %s (%s)\n", cb_strerror(rc), err);
        return -1;
    }
    return 0;
}
