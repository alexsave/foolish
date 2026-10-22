// fz_match.c - the matcher on arbitrary bytes: as an upload (adv_trace_parse, then adv_match on
// the points it yields) and as raw points (the bytes reinterpreted as adv_trace_pt, so the matcher
// itself is fuzzed and not only the parser). The graph is a small street grid built once through
// the real builder; the test library that does this for the unit tests is not linked into the
// fuzz build, so the grid is built here.
#include "graph.h"
#include "match.h"
#include "trace.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FZ_CHECK(cond)                                                                      \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FZ_CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);   \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static adv_graph          G;
static adv_match_scratch *S;
static adv_trace_pt       PTS[ADV_TRACE_MAX_PTS + 1];

#define COLS 6
#define ROWS 6
#define BLOCK_M 120.0
#define SHAPE_M 40.0

static adv_pt crossing(int i, int j) {
    adv_pt sw = { (int32_t)(41.8 * ADV_DEG), (int32_t)(-71.4 * ADV_DEG) };
    adv_frame f = adv_frame_at(sw.lat);
    adv_pt p = { (int32_t)(sw.lat + j * BLOCK_M / f.my), (int32_t)(sw.lon + i * BLOCK_M / f.mx) };
    return p;
}

static int street(adv_build *b, int i0, int j0, int i1, int j1) {
    adv_pt a = crossing(i0, j0), c = crossing(i1, j1);
    int64_t ids[8];
    adv_pt pts[8];
    int n = 0, interior = (int)(BLOCK_M / SHAPE_M) - 1;
    ids[n] = (int64_t)j0 * 100000 + i0 + 1;
    pts[n++] = a;
    for (int k = 1; k <= interior; k++) {
        double t = (double)k / (interior + 1);
        adv_pt m = { (int32_t)(a.lat + t * ((double)c.lat - a.lat)), (int32_t)(a.lon + t * ((double)c.lon - a.lon)) };
        ids[n] = -(int64_t)(1000000 + (int64_t)i0 * 1000003 + (int64_t)j0 * 1009 + (int64_t)i1 * 31 + j1 * 7 + k * 1000000007LL);
        pts[n++] = m;
    }
    ids[n] = (int64_t)j1 * 100000 + i1 + 1;
    pts[n++] = c;
    return adv_build_add_way(b, ids, pts, n, ADV_ACC_BIKE | ADV_ACC_FOOT, ADV_HC_RESIDENTIAL, 0, NULL);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    adv_build *b = adv_build_new("fz_match");
    FZ_CHECK(b != NULL);
    int rc = CB_OK;
    for (int j = 0; j <= ROWS && rc == CB_OK; j++)
        for (int i = 0; i < COLS && rc == CB_OK; i++) rc = street(b, i, j, i + 1, j);
    for (int i = 0; i <= COLS && rc == CB_OK; i++)
        for (int j = 0; j < ROWS && rc == CB_OK; j++) rc = street(b, i, j, i, j + 1);
    FZ_CHECK(rc == CB_OK);
    char path[512], err[256] = "";
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(path, sizeof path, "%s/adv-fz_match-%d.graph", dir, (int)getpid());
    FZ_CHECK(adv_build_finish(b, path, err, sizeof err) == CB_OK);
    adv_build_free(b);
    FZ_CHECK(adv_graph_open(path, &G, err, sizeof err) == CB_OK);
    unlink(path);   // the mapping outlives the name
    S = adv_match_scratch_new();
    FZ_CHECK(S != NULL);
    return 0;
}

// What every result must satisfy, whatever went in.
static void check(const adv_match_result *r, uint32_t n) {
    FZ_CHECK(r->rc >= ADV_MATCH_OK && r->rc < ADV_MATCH_RJ_N);
    FZ_CHECK(r->n_points == n);
    FZ_CHECK(r->n_samples <= ADV_MATCH_MAX_SAMPLES && r->n_samples <= n);
    FZ_CHECK(r->n_hits <= ADV_MATCH_MAX_HITS);
    if (r->rc != ADV_MATCH_OK) FZ_CHECK(r->n_hits == 0);
    for (uint32_t i = 0; i < r->n_hits; i++) {
        FZ_CHECK(r->hits[i].seg < G.h->n_segs);
        FZ_CHECK(r->hits[i].mask != 0);
        if (i > 0) FZ_CHECK(r->hits[i - 1].seg < r->hits[i].seg);
    }
    FZ_CHECK(r->matched_m >= 0 && r->trace_m >= 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    adv_match_opts o = { .cpu_budget_us = 50000 };
    adv_match_result r;
    // As an upload.
    adv_trace t;
    if (adv_trace_parse(data, size, &t) == CB_OK) {
        uint32_t n = t.hdr.n_points;
        for (uint32_t i = 0; i < n; i++) PTS[i] = adv_trace_pt_at(&t, i);
        adv_match(&G, PTS, n, t.hdr.type, &o, S, &r);
        check(&r, n);
    }
    // As raw points, the type and the options from the first bytes.
    uint32_t n = (uint32_t)(size / sizeof(adv_trace_pt));
    if (n > ADV_TRACE_MAX_PTS) n = ADV_TRACE_MAX_PTS;
    memcpy(PTS, data, (size_t)n * sizeof(adv_trace_pt));
    uint8_t type = size ? (uint8_t)(1 + data[0] % 3) : ADV_ACT_BIKE;
    if (size > 1) {
        o.cand_radius_m = 5 + data[1] % 200;
        o.sample_m = 1 + data[1] / 16;
    }
    adv_match(&G, PTS, n, type, &o, S, &r);
    check(&r, n);
    return 0;
}
