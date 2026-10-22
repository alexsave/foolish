// fz_tile.c - the tile path parser and the renderer on untrusted bytes, and the writer on
// arbitrary points. A tile request is the one thing a stranger types into this server (the URL),
// so every value a URL could carry reaches adv_tile_render here: in range, out of range, and
// as raw bytes. The renderer may answer CB_OK, CB_E_INVALID or CB_E_FULL and nothing else, and
// under ASan it may not touch a byte it does not own.
//
// The grid is built once through the builder (test/lib_fakegraph.c is not linked into fuzz
// builds, so the few lines of it that matter are here): 6x6 blocks of 100 m, a shape point
// every 25 m, every street bike and foot, the x=0 street PRIMARY.
#include "coverage.h"
#include "graph.h"
#include "mvt.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv);

#define FZ_CHECK(cond)                                                                      \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FZ_CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);   \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

static adv_graph        g;
static adv_cov          cov;
static uint32_t        *owners;
static adv_tile_scratch scratch;
static adv_pt           sw = { 418000000, -714000000 };
#define COLS 6
#define ROWS 6
#define BLOCK_M 100.0
#define SHAPE_M 25.0

static adv_pt crossing(int i, int j) {
    adv_frame f = adv_frame_at(sw.lat);
    adv_pt p = { (int32_t)(sw.lat + j * BLOCK_M / f.my), (int32_t)(sw.lon + i * BLOCK_M / f.mx) };
    return p;
}

static int street(adv_build *b, int i0, int j0, int i1, int j1, uint8_t hclass) {
    adv_pt a = crossing(i0, j0), c = crossing(i1, j1);
    int64_t ids[16];
    adv_pt pts[16];
    int interior = (int)(BLOCK_M / SHAPE_M) - 1, n = 0;
    ids[n] = (int64_t)j0 * 100000 + i0 + 1;
    pts[n++] = a;
    for (int k = 1; k <= interior; k++) {
        double t = (double)k / (interior + 1);
        adv_pt m = { (int32_t)(a.lat + t * ((double)c.lat - a.lat)), (int32_t)(a.lon + t * ((double)c.lon - a.lon)) };
        ids[n] = -(1000000 + (int64_t)i0 * 1000003 + (int64_t)j0 * 1009 + (int64_t)i1 * 31 + j1 * 7 + k * 1000000007LL);
        pts[n++] = m;
    }
    ids[n] = (int64_t)j1 * 100000 + i1 + 1;
    pts[n++] = c;
    return adv_build_add_way(b, ids, pts, n, ADV_ACC_BIKE | ADV_ACC_FOOT, hclass, 0);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    adv_build *b = adv_build_new("fz_tile");
    FZ_CHECK(b != NULL);
    for (int j = 0; j <= ROWS; j++)
        for (int i = 0; i < COLS; i++) FZ_CHECK(street(b, i, j, i + 1, j, ADV_HC_RESIDENTIAL) == CB_OK);
    for (int i = 0; i <= COLS; i++)
        for (int j = 0; j < ROWS; j++) FZ_CHECK(street(b, i, j, i, j + 1, i == 0 ? ADV_HC_PRIMARY : ADV_HC_RESIDENTIAL) == CB_OK);
    char path[256], err[256] = "";
    const char *dir = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/adv-fz_tile-%d.graph", dir && *dir ? dir : "/tmp", (int)getpid());
    FZ_CHECK(adv_build_finish(b, path, err, sizeof err) == CB_OK);
    adv_build_free(b);
    FZ_CHECK(adv_graph_open(path, &g, err, sizeof err) == CB_OK);
    unlink(path);   // the mapping outlives the name
    adv_cov_init(&cov);
    adv_hit h[64];
    for (uint32_t k = 0; k < 64; k++) {
        h[k].seg = (k * 13) % g.h->n_segs;
        h[k].mask = (uint16_t)(k % 4 == 0 ? 0xFFFF : k % 4 == 1 ? 0x00FF : k % 4 == 2 ? 0x0F0F : 0x4000);
        h[k].pad = 0;
    }
    uint32_t n = adv_hits_finish(h, 64);
    float new_m[3] = { 0, 0, 0 };
    FZ_CHECK(adv_cov_merge(&cov, &g, h, n, ADV_T_BIKE, NULL, new_m) == CB_OK);
    FZ_CHECK(adv_cov_merge(&cov, &g, h, n / 2, ADV_T_FOOT, NULL, new_m) == CB_OK);
    owners = calloc(g.h->n_segs, sizeof *owners);
    FZ_CHECK(owners != NULL);
    for (uint32_t s = 0; s < g.h->n_segs; s++) owners[s] = s % 3 == 0 ? 7 : s % 3 == 1 ? ADV_OWNER_MANY : 0;
    FZ_CHECK(adv_tile_scratch_init(&scratch, &g) == CB_OK);
    return 0;
}

static uint32_t u32_at(const uint8_t *p, size_t n, size_t at) {
    uint32_t v = 0;
    for (size_t i = 0; i < 4; i++) v |= (uint32_t)(at + i < n ? p[at + i] : 0) << (8 * i);
    return v;
}

static void render(const adv_tile_req *r, uint32_t user, bool with_cov, bool with_owners) {
    const uint8_t *out = NULL;
    int rc = adv_tile_render(&g, r, user, with_cov ? &cov : NULL, with_owners ? owners : NULL, &scratch, &out);
    FZ_CHECK(rc >= 0 || rc == CB_E_INVALID || rc == CB_E_FULL);
    if (rc >= 0) {
        FZ_CHECK(out == scratch.bytes);
        FZ_CHECK((size_t)rc <= scratch.cap);
        // The first byte of a non-empty tile is Tile.layers' tag.
        FZ_CHECK(rc == 0 || out[0] == 0x1A);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // The path parser on the bytes as a string, exact-size so one byte past is an ASan report.
    char *s = malloc(size + 1);
    FZ_CHECK(s != NULL);
    if (size) memcpy(s, data, size);
    s[size] = 0;
    int z;
    uint32_t x, y;
    if (adv_tile_parse_zxy(s, &z, &x, &y)) {
        FZ_CHECK(z >= 0 && z <= ADV_TILE_MAX_ZOOM && x < (1u << z) && y < (1u << z));
        adv_tile_req r = { .z = z, .x = x, .y = y, .type = ADV_T_ALL, .roads = true, .traveled = true, .untraveled = true, .unique = true };
        render(&r, 7, true, true);
    }
    free(s);
    if (size < 12) return 0;

    uint8_t flags = data[0], tb = data[1];
    bool with_cov = flags & 0x10, with_owners = flags & 0x20;
    uint32_t user = (flags & 0x40) ? 7 : (flags & 0x80) ? 0 : 9;
    adv_tile_req r = { .roads = flags & 1, .traveled = flags & 2, .untraveled = flags & 4, .unique = flags & 8 };

    // 1. A tile over the grid: a zoom from the byte, a point in the grid's box from two bytes,
    //    the tile holding it, nudged by up to one tile each way so the edges are seen.
    r.z = data[2] % (ADV_TILE_MAX_ZOOM + 1);
    r.type = tb % ADV_T_N;
    adv_pt p = { g.h->min_lat + (int32_t)(((int64_t)g.h->max_lat - g.h->min_lat) * data[3] / 255),
                 g.h->min_lon + (int32_t)(((int64_t)g.h->max_lon - g.h->min_lon) * data[4] / 255) };
    adv_tile_of(p, r.z, &r.x, &r.y);
    uint32_t side = 1u << r.z;
    r.x = (uint32_t)((int64_t)r.x + (data[5] % 3) - 1 + side) % side;
    r.y = (uint32_t)((int64_t)r.y + (data[6] % 3) - 1 + side) % side;
    render(&r, user, with_cov, with_owners);

    // 2. A tile anywhere in range.
    r.x = u32_at(data, size, 7) % side;
    r.y = u32_at(data, size, 11) % side;
    render(&r, user, with_cov, with_owners);

    // 3. Raw: every field straight from the bytes, in range or not.
    r.z = (int8_t)data[2];
    r.type = (int8_t)tb;
    r.x = u32_at(data, size, 7);
    r.y = u32_at(data, size, 11);
    render(&r, user, with_cov, with_owners);

    // 4. The writer on the rest of the bytes as points, in a scratch of a size the input picks.
    if (size > 16) {
        size_t cap = (size_t)data[15] * 64;
        uint8_t *sc = malloc(cap ? cap : 1);
        FZ_CHECK(sc != NULL);
        adv_mvt w;
        adv_mvt_init(&w, sc, cap);
        const char *const keys[] = { "c", "t", "u", "v" };
        adv_mvt_layer_begin(&w, "fz", keys, data[16] % 6);   // more keys than ADV_MVT_MAX_TAGS too
        int tags[ADV_MVT_MAX_TAGS] = { data[16] - 2, (int8_t)data[0], 300, -1 };
        size_t n_pts = (size - 17) / 8;
        if (n_pts > ADV_SEG_MAX_PTS + 4) n_pts = ADV_SEG_MAX_PTS + 4;   // past the cap is a case too
        int32_t *xy = malloc((n_pts ? n_pts : 1) * 2 * sizeof *xy);
        FZ_CHECK(xy != NULL);
        for (size_t i = 0; i < n_pts; i++) {
            xy[2 * i] = (int32_t)u32_at(data, size, 17 + 8 * i);
            xy[2 * i + 1] = (int32_t)u32_at(data, size, 21 + 8 * i);
        }
        adv_mvt_feature_line(&w, u32_at(data, size, 7), tags, xy, (int)n_pts);
        adv_mvt_feature_line(&w, 1, NULL, xy, (int)(n_pts > 2 ? 2 : n_pts));
        adv_mvt_layer_end(&w);
        adv_mvt_layer_begin(&w, "second", keys, 1);
        adv_mvt_layer_end(&w);
        const uint8_t *out;
        int rc = adv_mvt_finish(&w, &out);
        FZ_CHECK(rc >= 0 || rc == CB_E_FULL);
        if (rc >= 0) FZ_CHECK((size_t)rc <= cap / 2);
        free(xy);
        free(sc);
    }
    return 0;
}
