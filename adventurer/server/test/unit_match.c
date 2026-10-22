// unit_match.c - the matcher on a synthetic grid: random walks sampled like a GPS (1 Hz, AR(1)
// correlated noise) must come back as the segments and sixteenths they travelled; the hostile
// inputs must each be refused quickly with the right reason, or matched sanely; and a 5000-point
// trace must match well inside the budget. Everything is deterministic (xorshift seeded per case).
#include "lib_fakegraph.h"
#include "match.h"
#include "t.h"
#include "util/err.h"

#include <math.h>
#include <unistd.h>

// The ASan lane runs several times slower; the CPU limits scale, the quality ones do not.
#ifdef ADV_INSTRUMENTED
#define CPU_SCALE 6
#else
#define CPU_SCALE 1
#endif

// ---- a deterministic PRNG ---------------------------------------------------------------------

typedef struct { uint64_t s; } rng;

static uint64_t rng_u64(rng *r) {   // xorshift64*
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static double rng_unif(rng *r) { return (double)(rng_u64(r) >> 11) * (1.0 / 9007199254740992.0); }
static uint32_t rng_below(rng *r, uint32_t n) { return (uint32_t)(rng_unif(r) * n) % n; }
static double rng_gauss(rng *r) {   // Box-Muller, one of the pair
    double u1 = rng_unif(r), u2 = rng_unif(r);
    if (u1 < 1e-300) u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}
static rng rng_seed(uint64_t seed) {
    rng r = { seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL };
    for (int i = 0; i < 4; i++) rng_u64(&r);
    return r;
}

// ---- a walk through the grid and the trace it leaves -----------------------------------------

#define WALK_MAX 512
typedef struct {
    uint32_t seg[WALK_MAX];
    uint8_t  fwd[WALK_MAX];   // travelled node_a to node_b
    int      n;
    double   f_start, f_end;  // where on seg[0] it starts, on seg[n-1] it ends (the segment's own fraction)
} walk;

typedef struct { double x, y; } xy;

static adv_trace_pt TR[ADV_TRACE_MAX_PTS];
static xy           POLY[WALK_MAX * ADV_SEG_MAX_PTS];
static adv_hit      TRUTH[WALK_MAX * 2];

// A random walk of n_segs segments the type may use, never doubling straight back when it can
// turn, from a random node.
static void walk_make(const adv_graph *g, rng *r, uint8_t access, int n_segs, walk *w) {
    uint32_t node = 0;
    for (int tries = 0; tries < 1000; tries++) {
        node = rng_below(r, g->h->n_nodes);
        const adv_node *nd = &g->nodes[node];
        int ok = 0;
        for (uint32_t k = 0; k < nd->n_adj; k++) if (g->segs[g->adj[nd->adj_off + k]].access & access) ok++;
        if (ok) break;
    }
    uint32_t prev = 0xFFFFFFFFu;
    w->n = 0;
    for (int i = 0; i < n_segs && i < WALK_MAX; i++) {
        const adv_node *nd = &g->nodes[node];
        uint32_t opts[ADV_NODE_MAX_ADJ];
        int no = 0, no_fwd = 0;
        for (uint32_t k = 0; k < nd->n_adj; k++) {
            uint32_t e = g->adj[nd->adj_off + k];
            if (!(g->segs[e].access & access)) continue;
            opts[no++] = e;
            if (e != prev) no_fwd++;
        }
        if (no == 0) break;
        uint32_t e;
        do e = opts[rng_below(r, (uint32_t)no)]; while (no_fwd > 0 && e == prev);
        w->seg[w->n] = e;
        w->fwd[w->n] = g->segs[e].node_a == node;
        node = w->fwd[w->n] ? g->segs[e].node_b : g->segs[e].node_a;
        w->n++;
        prev = e;
    }
    w->f_start = 0.05 + 0.9 * rng_unif(r);
    w->f_end = 0.05 + 0.9 * rng_unif(r);
}

// The coverage the walk KNOWS it produced: whole segments, the two ends partial.
static uint32_t walk_truth(const walk *w, adv_hit *h) {
    for (int k = 0; k < w->n; k++) {
        uint16_t m = 0xFFFF;
        if (k == 0) m = adv_mask_of_run(w->f_start, w->fwd[0] ? 1.0 : 0.0);
        if (k == w->n - 1) {
            uint16_t me = adv_mask_of_run(w->fwd[k] ? 0.0 : 1.0, w->f_end);
            m = k == 0 ? adv_mask_of_run(w->f_start, w->f_end) : me;
        }
        h[k].seg = w->seg[k];
        h[k].mask = m;
        h[k].pad = 0;
    }
    return adv_hits_finish(h, (uint32_t)w->n);
}

// The walk as one polyline in frame metres (travel order), and where along it the ride starts
// and ends.
static int walk_poly(const adv_graph *g, const walk *w, const adv_frame *f, adv_pt origin, xy *out, double *d0, double *d1) {
    int np = 0;
    adv_pt pts[ADV_SEG_MAX_PTS];
    for (int k = 0; k < w->n; k++) {
        int n = adv_seg_points(g, w->seg[k], pts, ADV_SEG_MAX_PTS);
        for (int i = 0; i < n; i++) {
            adv_pt p = pts[w->fwd[k] ? i : n - 1 - i];
            xy q = { ((double)p.lon - origin.lon) * f->mx, ((double)p.lat - origin.lat) * f->my };
            if (np > 0 && out[np - 1].x == q.x && out[np - 1].y == q.y) continue;
            out[np++] = q;
        }
    }
    double total = 0;
    for (int i = 1; i < np; i++) total += hypot(out[i].x - out[i - 1].x, out[i].y - out[i - 1].y);
    double l0 = g->segs[w->seg[0]].len_m, l1 = g->segs[w->seg[w->n - 1]].len_m;
    *d0 = w->fwd[0] ? w->f_start * l0 : (1 - w->f_start) * l0;
    *d1 = total - (w->fwd[w->n - 1] ? (1 - w->f_end) * l1 : w->f_end * l1);
    return np;
}

// 1 Hz points along the walk at v m/s with AR(1) GPS error (rho 0.9, sigma 4 m): the error of
// one fix is most of the last one's plus fresh noise, so the trace wanders like a receiver's.
static uint32_t gen_trace(const adv_graph *g, const walk *w, rng *r, double v, adv_trace_pt *out, uint32_t cap) {
    adv_pt origin = { g->h->min_lat, g->h->min_lon };
    adv_frame f = adv_frame_at(origin.lat);
    double d0, d1;
    int np = walk_poly(g, w, &f, origin, POLY, &d0, &d1);
    const double rho = 0.9, sigma = 4.0, fresh = sqrt(1 - rho * rho) * sigma;
    double ex = sigma * rng_gauss(r), ey = sigma * rng_gauss(r);
    uint32_t n = 0;
    int seg = 1;
    double acc = 0;
    for (uint32_t t = 0; n < cap && np >= 2; t++) {
        double d = d0 + v * t;
        if (d > d1) break;
        double L = hypot(POLY[seg].x - POLY[seg - 1].x, POLY[seg].y - POLY[seg - 1].y);
        while (seg < np - 1 && acc + L < d) {
            acc += L;
            seg++;
            L = hypot(POLY[seg].x - POLY[seg - 1].x, POLY[seg].y - POLY[seg - 1].y);
        }
        double u = L > 0 ? (d - acc) / L : 0;
        if (u < 0) u = 0;
        if (u > 1) u = 1;
        double x = POLY[seg - 1].x + u * (POLY[seg].x - POLY[seg - 1].x), y = POLY[seg - 1].y + u * (POLY[seg].y - POLY[seg - 1].y);
        ex = rho * ex + fresh * rng_gauss(r);
        ey = rho * ey + fresh * rng_gauss(r);
        out[n].lat = (int32_t)(origin.lat + (y + ey) / f.my);
        out[n].lon = (int32_t)(origin.lon + (x + ex) / f.mx);
        out[n].t_ms = t * 1000;
        n++;
    }
    return n;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

typedef struct {
    double   recall_sum, prec_sum, recall_min, prec_min;
    uint32_t cpu[64];
    uint32_t n, fast, searches, jumps, points;
} walk_stats;

// n_walks random walks of n_segs on g at v m/s, each matched and scored against its truth.
static void run_walks(const adv_graph *g, adv_match_scratch *s, uint64_t seed, uint8_t type, uint8_t access,
                      int n_walks, int n_segs, double v, walk_stats *ws) {
    memset(ws, 0, sizeof *ws);
    ws->recall_min = ws->prec_min = 1;
    static walk w;
    for (int i = 0; i < n_walks && i < 64; i++) {
        rng r = rng_seed(seed + (uint64_t)i);
        walk_make(g, &r, access, n_segs, &w);
        uint32_t nt = walk_truth(&w, TRUTH);
        float truth_m = adv_hits_len_m(g, TRUTH, nt);
        uint32_t n = gen_trace(g, &w, &r, v, TR, ADV_TRACE_MAX_PTS);
        adv_match_result res;
        adv_match(g, TR, n, type, NULL, s, &res);
        T_CHECK_MSG(res.rc == ADV_MATCH_OK, "walk %d rc %s", i, adv_match_reason(res.rc));
        float common = adv_hits_common_m(g, TRUTH, nt, res.hits, res.n_hits);
        double recall = truth_m > 0 ? common / truth_m : 0, prec = res.matched_m > 0 ? common / res.matched_m : 0;
        // The brief's floors were 0.85 recall per walk, 0.95 mean, 0.9 mean precision; on a clean
        // grid the matcher does far better (0.998+ recall, 0.995+ precision), and a transition
        // cost that is merely wrong, not broken, shows only below these.
        T_CHECK_MSG(recall >= 0.99, "walk %d recall %.4f (truth %.0f m, matched %.0f m, common %.0f m, %u pts, %u samples, %u offroad)",
                    i, recall, truth_m, res.matched_m, common, n, res.n_samples, res.n_offroad);
        T_CHECK_MSG(prec >= 0.985, "walk %d precision %.4f (matched %.0f m, common %.0f m)", i, prec, res.matched_m, common);
        ws->recall_sum += recall;
        ws->prec_sum += prec;
        if (recall < ws->recall_min) ws->recall_min = recall;
        if (prec < ws->prec_min) ws->prec_min = prec;
        ws->cpu[ws->n++] = res.cpu_us;
        ws->fast += res.n_fast;
        ws->searches += res.n_searches;
        ws->jumps += res.n_jumps;
        ws->points += n;
        // Every hit is a real segment the type may use, with a mask.
        for (uint32_t k = 0; k < res.n_hits; k++) {
            T_CHECK(res.hits[k].seg < g->h->n_segs && res.hits[k].mask != 0);
            T_CHECK(g->segs[res.hits[k].seg].access & access);
            if (k > 0) T_CHECK(res.hits[k - 1].seg < res.hits[k].seg);
        }
    }
    qsort(ws->cpu, ws->n, sizeof ws->cpu[0], cmp_u32);
}

static void print_stats(const char *name, const walk_stats *ws) {
    double mean = 0;
    for (uint32_t i = 0; i < ws->n; i++) mean += ws->cpu[i];
    mean /= ws->n ? ws->n : 1;
    uint32_t total = ws->fast + ws->searches;
    printf("%s: %u walks, %u pts/walk: recall mean %.4f min %.4f, precision mean %.4f min %.4f\n", name, ws->n,
           ws->n ? ws->points / ws->n : 0, ws->recall_sum / ws->n, ws->recall_min, ws->prec_sum / ws->n, ws->prec_min);
    printf("%s: cpu_us mean %.0f p50 %u p99 %u max %u; transitions: fast %u (%.1f%%) searches %u (%.1f%%) jumps %u\n", name, mean,
           ws->cpu[ws->n / 2], ws->cpu[ws->n * 99 / 100], ws->cpu[ws->n - 1], ws->fast, total ? 100.0 * ws->fast / total : 0,
           ws->searches, total ? 100.0 * ws->searches / total : 0, ws->jumps);
}

// ---- the walks ------------------------------------------------------------------------------

static void test_walks(adv_match_scratch *s) {
    fg_opts o = { .cols = 12, .rows = 12, .block_m = 120, .shape_m = 40 };
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_match");
    adv_graph g;
    T_CHECK(fg_build(&o, path, &g) == 0);
    walk_stats ws;
    // 30 bike rides of about 5000 points (250 blocks at 6 m/s).
    run_walks(&g, s, 1000, ADV_ACT_BIKE, ADV_ACC_BIKE, 30, 250, 6.0, &ws);
    print_stats("bike", &ws);
    T_CHECK_MSG(ws.recall_sum / ws.n >= 0.995, "mean recall %.4f", ws.recall_sum / ws.n);
    T_CHECK_MSG(ws.prec_sum / ws.n >= 0.99, "mean precision %.4f", ws.prec_sum / ws.n);
    T_CHECK_MSG(ws.cpu[ws.n / 2] < 20000u * CPU_SCALE, "p50 cpu %u", ws.cpu[ws.n / 2]);
    T_CHECK_MSG(ws.cpu[ws.n - 1] < 40000u * CPU_SCALE, "max cpu %u", ws.cpu[ws.n - 1]);
    T_CHECK(ws.fast > ws.searches);   // the fast path is the majority
    adv_graph_close(&g);
    unlink(path);

    // The same grid with every other east-west street a footpath: on foot everything is open;
    // a bike is matched to bike roads only, whatever it did.
    fg_opts o2 = { .cols = 12, .rows = 12, .block_m = 120, .shape_m = 40, .foot_only_rows = true };
    T_CHECK(fg_build(&o2, path, &g) == 0);
    run_walks(&g, s, 2000, ADV_ACT_RUN, ADV_ACC_FOOT, 10, 125, 3.0, &ws);
    print_stats("foot", &ws);
    T_CHECK_MSG(ws.recall_sum / ws.n >= 0.995, "foot mean recall %.4f", ws.recall_sum / ws.n);
    T_CHECK_MSG(ws.prec_sum / ws.n >= 0.99, "foot mean precision %.4f", ws.prec_sum / ws.n);
    run_walks(&g, s, 3000, ADV_ACT_BIKE, ADV_ACC_BIKE, 10, 250, 6.0, &ws);
    print_stats("bike/footpaths", &ws);
    T_CHECK_MSG(ws.recall_sum / ws.n >= 0.995, "bike2 mean recall %.4f", ws.recall_sum / ws.n);
    T_CHECK_MSG(ws.prec_sum / ws.n >= 0.99, "bike2 mean precision %.4f", ws.prec_sum / ws.n);
    adv_graph_close(&g);
    unlink(path);
}

// ---- the hostile set -------------------------------------------------------------------------

static uint32_t node_at(const adv_graph *g, adv_pt p) {
    for (uint32_t i = 0; i < g->h->n_nodes; i++)
        if (g->nodes[i].lat == p.lat && g->nodes[i].lon == p.lon) return i;
    return 0xFFFFFFFFu;
}
static uint32_t seg_between(const adv_graph *g, uint32_t na, uint32_t nb) {
    const adv_node *n = &g->nodes[na];
    for (uint32_t k = 0; k < n->n_adj; k++) {
        uint32_t e = g->adj[n->adj_off + k];
        if ((g->segs[e].node_a == na && g->segs[e].node_b == nb) || (g->segs[e].node_a == nb && g->segs[e].node_b == na)) return e;
    }
    return 0xFFFFFFFFu;
}

static void test_hostile(adv_match_scratch *s) {
    fg_opts o = { .cols = 12, .rows = 12, .block_m = 120, .shape_m = 40 };
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_match_h");
    adv_graph g;
    T_CHECK(fg_build(&o, path, &g) == 0);
    adv_match_result res;
    adv_pt inside = fg_crossing(&o, 3, 3);
    rng r = rng_seed(77);

    // Nothing, and one point.
    adv_match(&g, TR, 0, ADV_ACT_BIKE, NULL, s, &res);
    T_CHECK(res.rc == ADV_MATCH_RJ_SHORT && res.n_hits == 0 && res.cpu_us < 50000u * CPU_SCALE);
    TR[0].lat = inside.lat; TR[0].lon = inside.lon; TR[0].t_ms = 0;
    adv_match(&g, TR, 1, ADV_ACT_BIKE, NULL, s, &res);
    T_CHECK(res.rc == ADV_MATCH_RJ_SHORT && res.n_hits == 0);
    // Standing still for an hour, inside the grid.
    for (uint32_t i = 0; i < 3600; i++) { TR[i].lat = inside.lat; TR[i].lon = inside.lon; TR[i].t_ms = i * 1000; }
    adv_match(&g, TR, 3600, ADV_ACT_BIKE, NULL, s, &res);
    T_CHECK_MSG(res.rc == ADV_MATCH_RJ_SHORT && res.n_hits == 0, "still: %s", adv_match_reason(res.rc));
    // 65535 points all at t=0 at one coordinate.
    for (uint32_t i = 0; i < ADV_TRACE_MAX_PTS; i++) { TR[i].lat = inside.lat; TR[i].lon = inside.lon; TR[i].t_ms = 0; }
    adv_match(&g, TR, ADV_TRACE_MAX_PTS, ADV_ACT_BIKE, NULL, s, &res);
    T_CHECK_MSG(res.rc == ADV_MATCH_RJ_SHORT && res.cpu_us < 50000u * CPU_SCALE, "t0: %s %u us", adv_match_reason(res.rc), res.cpu_us);
    // 65535 points random over the planet.
    for (uint32_t i = 0; i < ADV_TRACE_MAX_PTS; i++) {
        TR[i].lat = (int32_t)((rng_unif(&r) * 170 - 85) * ADV_DEG);
        TR[i].lon = (int32_t)((rng_unif(&r) * 360 - 180) * ADV_DEG);
        TR[i].t_ms = i * 1000;
    }
    adv_match(&g, TR, ADV_TRACE_MAX_PTS, ADV_ACT_BIKE, NULL, s, &res);
    T_CHECK_MSG((res.rc == ADV_MATCH_RJ_TELEPORT || res.rc == ADV_MATCH_RJ_COORDS) && res.n_hits == 0 && res.cpu_us < 50000u * CPU_SCALE,
                "planet: %s %u us", adv_match_reason(res.rc), res.cpu_us);
    // Random over the planet but starting inside: the rest are spikes.
    TR[0].lat = inside.lat; TR[0].lon = inside.lon;
    adv_match(&g, TR, ADV_TRACE_MAX_PTS, ADV_ACT_BIKE, NULL, s, &res);
    T_CHECK_MSG(res.rc == ADV_MATCH_RJ_TELEPORT && res.n_spikes > 60000, "planet2: %s spikes %u", adv_match_reason(res.rc), res.n_spikes);

    // A good ride, kept for the variations below.
    static walk w;
    walk_make(&g, &r, ADV_ACC_BIKE, 250, &w);
    uint32_t nt = walk_truth(&w, TRUTH);
    float truth_m = adv_hits_len_m(&g, TRUTH, nt);
    uint32_t n = gen_trace(&g, &w, &r, 6.0, TR, ADV_TRACE_MAX_PTS);
    T_CHECK(n > 4000);
    adv_match(&g, TR, n, ADV_ACT_BIKE, NULL, s, &res);
    T_CHECK(res.rc == ADV_MATCH_OK && res.n_spikes == 0);
    float base_recall = adv_hits_common_m(&g, TRUTH, nt, res.hits, res.n_hits) / truth_m;
    T_CHECK(base_recall >= 0.9);
    // Every point a degree east: outside the graph.
    for (uint32_t i = 0; i < n; i++) TR[i].lon += ADV_DEG;
    adv_match(&g, TR, n, ADV_ACT_BIKE, NULL, s, &res);
    T_CHECK_MSG(res.rc == ADV_MATCH_RJ_COORDS && res.n_hits == 0, "outside: %s", adv_match_reason(res.rc));
    for (uint32_t i = 0; i < n; i++) TR[i].lon -= ADV_DEG;
    // Time running backwards in the middle.
    uint32_t saved = TR[n / 2].t_ms;
    TR[n / 2].t_ms = TR[n / 2 - 1].t_ms - 5000;
    adv_match(&g, TR, n, ADV_ACT_BIKE, NULL, s, &res);
    T_CHECK_MSG(res.rc == ADV_MATCH_RJ_TIME && res.n_hits == 0, "backwards: %s", adv_match_reason(res.rc));
    TR[n / 2].t_ms = saved;
    // A point a week in.
    TR[n / 2].t_ms = 7 * 86400000u;
    adv_match(&g, TR, n, ADV_ACT_BIKE, NULL, s, &res);
    T_CHECK_MSG(res.rc == ADV_MATCH_RJ_TIME, "week: %s", adv_match_reason(res.rc));
    TR[n / 2].t_ms = saved;
    // A latitude of 2e9.
    int32_t saved_lat = TR[n / 3].lat;
    TR[n / 3].lat = 2000000000;
    adv_match(&g, TR, n, ADV_ACT_BIKE, NULL, s, &res);
    T_CHECK_MSG(res.rc == ADV_MATCH_RJ_COORDS && res.n_hits == 0, "lat2e9: %s", adv_match_reason(res.rc));
    TR[n / 3].lat = saved_lat;
    // The budget: one microsecond is never enough for a 5000-point trace.
    adv_match_opts tight = { .cpu_budget_us = 1 };
    adv_match(&g, TR, n, ADV_ACT_BIKE, &tight, s, &res);
    T_CHECK_MSG(res.rc == ADV_MATCH_RJ_BUDGET && res.n_hits == 0, "budget: %s", adv_match_reason(res.rc));
    // 5% spikes of 300 m, and a 90 s dropout in the middle: still a good match.
    {
        static adv_trace_pt TR2[ADV_TRACE_MAX_PTS];
        adv_frame f = adv_frame_at(TR[0].lat);
        uint32_t n2 = 0, injected = 0;
        uint32_t gap0 = TR[n / 2].t_ms, gap1 = gap0 + 90000;
        for (uint32_t i = 0; i < n; i++) {
            if (TR[i].t_ms >= gap0 && TR[i].t_ms < gap1) continue;
            TR2[n2] = TR[i];
            if (rng_unif(&r) < 0.05) {
                double ang = rng_unif(&r) * 2 * M_PI;
                TR2[n2].lat += (int32_t)(300 * sin(ang) / f.my);
                TR2[n2].lon += (int32_t)(300 * cos(ang) / f.mx);
                injected++;
            }
            n2++;
        }
        adv_match(&g, TR2, n2, ADV_ACT_BIKE, NULL, s, &res);
        T_CHECK_MSG(res.rc == ADV_MATCH_OK, "spiky: %s", adv_match_reason(res.rc));
        T_CHECK_MSG(res.n_spikes >= injected * 8 / 10, "spikes counted %u of %u", res.n_spikes, injected);
        float recall = adv_hits_common_m(&g, TRUTH, nt, res.hits, res.n_hits) / truth_m;
        float prec = res.matched_m > 0 ? adv_hits_common_m(&g, TRUTH, nt, res.hits, res.n_hits) / res.matched_m : 0;
        printf("spiky+dropout: recall %.3f precision %.3f spikes %u/%u searches %u jumps %u cpu %u us\n", recall, prec, res.n_spikes, injected,
               res.n_searches, res.n_jumps, res.cpu_us);
        T_CHECK_MSG(recall >= 0.85, "spiky recall %.3f", recall);
        T_CHECK_MSG(prec >= 0.85, "spiky precision %.3f", prec);
    }
    // A straight line along the middle of a row of blocks, 60 m from the streets either side: off
    // the roads but for a sixteenth at each street it crosses.
    {
        adv_pt a = fg_crossing(&o, 0, 5), b = fg_crossing(&o, 12, 5), a6 = fg_crossing(&o, 0, 6);
        int32_t mid_lat = (int32_t)(((int64_t)a.lat + a6.lat) / 2);
        uint32_t m = 0;
        for (uint32_t i = 0; i < 300; i++) {
            TR[m].lat = mid_lat;
            TR[m].lon = (int32_t)(a.lon - 0.05 * (b.lon - a.lon) + ((double)b.lon - a.lon) * 1.1 * i / 300);
            TR[m].t_ms = i * 1000;
            m++;
        }
        adv_match(&g, TR, m, ADV_ACT_BIKE, NULL, s, &res);
        T_CHECK_MSG(res.rc == ADV_MATCH_OK, "midblock: %s", adv_match_reason(res.rc));
        T_CHECK_MSG(res.n_offroad > res.n_samples / 3, "midblock offroad %u of %u", res.n_offroad, res.n_samples);
        T_CHECK_MSG(res.matched_m < 0.3 * res.trace_m, "midblock matched %.0f of %.0f", res.matched_m, res.trace_m);
        T_CHECK_MSG(res.n_hits <= 2u * (uint32_t)(o.cols + 1), "midblock hits %u", res.n_hits);
        printf("midblock: %u samples, %u offroad, %u hits, matched %.0f of %.0f m\n", res.n_samples, res.n_offroad, res.n_hits, res.matched_m, res.trace_m);
    }
    // Three blocks of one street, back and forth 50 times: those three, once.
    {
        uint32_t nodes[4], segs[3];
        for (int i = 0; i < 4; i++) nodes[i] = node_at(&g, fg_crossing(&o, 2 + i, 6));
        for (int i = 0; i < 3; i++) segs[i] = seg_between(&g, nodes[i], nodes[i + 1]);
        T_CHECK(segs[0] != 0xFFFFFFFFu && segs[1] != 0xFFFFFFFFu && segs[2] != 0xFFFFFFFFu);
        static walk bf;
        bf.n = 0;
        for (int rep = 0; rep < 50; rep++) {
            for (int i = 0; i < 3; i++) { bf.seg[bf.n] = segs[i]; bf.fwd[bf.n] = g.segs[segs[i]].node_a == nodes[i]; bf.n++; }
            for (int i = 2; i >= 0; i--) { bf.seg[bf.n] = segs[i]; bf.fwd[bf.n] = g.segs[segs[i]].node_a == nodes[i + 1]; bf.n++; }
        }
        bf.f_start = bf.fwd[0] ? 0.0 : 1.0;
        bf.f_end = bf.fwd[bf.n - 1] ? 1.0 : 0.0;
        uint32_t nb = gen_trace(&g, &bf, &r, 6.0, TR, ADV_TRACE_MAX_PTS);
        T_CHECK_MSG(nb > 5500, "back-forth %u pts", nb);
        adv_match(&g, TR, nb, ADV_ACT_BIKE, NULL, s, &res);
        T_CHECK_MSG(res.rc == ADV_MATCH_OK, "backforth: %s", adv_match_reason(res.rc));
        for (int i = 0; i < 3; i++) {
            bool whole = false;
            for (uint32_t k = 0; k < res.n_hits; k++) if (res.hits[k].seg == segs[i] && res.hits[k].mask == 0xFFFF) whole = true;
            T_CHECK_MSG(whole, "street %d not whole", i);
        }
        T_CHECK_MSG(res.matched_m >= 355 && res.matched_m < 450, "backforth matched %.0f m (trace %.0f m)", res.matched_m, res.trace_m);
        T_CHECK(res.trace_m > 30000);
        printf("back-forth: %u pts, %u samples, matched %.0f m of %.0f m, %u hits, cpu %u us\n", nb, res.n_samples, res.matched_m, res.trace_m,
               res.n_hits, res.cpu_us);
    }
    // Ten blocks of one street with a 90 s dropout a third of the way: the search bridges the
    // gap and the segments it passed whole are credited whole.
    {
        uint32_t nodes[11], segs[10];
        for (int i = 0; i < 11; i++) nodes[i] = node_at(&g, fg_crossing(&o, 1 + i, 3));
        static walk st;
        st.n = 10;
        for (int i = 0; i < 10; i++) {
            segs[i] = seg_between(&g, nodes[i], nodes[i + 1]);
            T_CHECK(segs[i] != 0xFFFFFFFFu);
            st.seg[i] = segs[i];
            st.fwd[i] = g.segs[segs[i]].node_a == nodes[i];
        }
        st.f_start = st.fwd[0] ? 0.0 : 1.0;
        st.f_end = st.fwd[9] ? 1.0 : 0.0;
        uint32_t ns = gen_trace(&g, &st, &r, 6.0, TR, ADV_TRACE_MAX_PTS);
        T_CHECK_MSG(ns > 190, "straight %u pts", ns);
        uint32_t gap0 = TR[ns * 3 / 10].t_ms, gap1 = gap0 + 90000, m = 0;
        for (uint32_t i = 0; i < ns; i++)
            if (TR[i].t_ms < gap0 || TR[i].t_ms >= gap1) TR[m++] = TR[i];
        T_CHECK(m + 85 <= ns);
        adv_match(&g, TR, m, ADV_ACT_BIKE, NULL, s, &res);
        T_CHECK_MSG(res.rc == ADV_MATCH_OK && res.n_searches >= 1 && res.n_jumps == 0, "dropout: %s searches %u jumps %u",
                    adv_match_reason(res.rc), res.n_searches, res.n_jumps);
        for (int i = 0; i < 10; i++) {   // the first and last too: the ride starts and ends at a node
            bool whole = false;
            for (uint32_t k = 0; k < res.n_hits; k++) if (res.hits[k].seg == segs[i] && res.hits[k].mask == 0xFFFF) whole = true;
            T_CHECK_MSG(whole, "dropout: block %d not whole", i);
        }
        T_CHECK_MSG(res.matched_m >= 10 * 120 - 30 && res.matched_m <= 10 * 120 + 60, "dropout matched %.0f", res.matched_m);
        printf("dropout: %u pts, %u samples, %u searches, matched %.0f m\n", m, res.n_samples, res.n_searches, res.matched_m);
    }
    // Two rides far apart with a long gap between: no route within the search's reach, so the
    // pair is a jump, and both rides are still credited.
    {
        uint32_t na[5], nb[5];
        static walk wa, wb;
        wa.n = wb.n = 4;
        for (int i = 0; i < 5; i++) { na[i] = node_at(&g, fg_crossing(&o, i, 0)); nb[i] = node_at(&g, fg_crossing(&o, 8 + i, 12)); }
        for (int i = 0; i < 4; i++) {
            wa.seg[i] = seg_between(&g, na[i], na[i + 1]);
            wa.fwd[i] = g.segs[wa.seg[i]].node_a == na[i];
            wb.seg[i] = seg_between(&g, nb[i], nb[i + 1]);
            wb.fwd[i] = g.segs[wb.seg[i]].node_a == nb[i];
        }
        wa.f_start = wa.fwd[0] ? 0.0 : 1.0; wa.f_end = wa.fwd[3] ? 1.0 : 0.0;
        wb.f_start = wb.fwd[0] ? 0.0 : 1.0; wb.f_end = wb.fwd[3] ? 1.0 : 0.0;
        uint32_t n1 = gen_trace(&g, &wa, &r, 6.0, TR, ADV_TRACE_MAX_PTS);
        uint32_t n2 = gen_trace(&g, &wb, &r, 6.0, TR + n1, ADV_TRACE_MAX_PTS - n1);
        for (uint32_t i = 0; i < n2; i++) TR[n1 + i].t_ms += TR[n1 - 1].t_ms + 400000;
        adv_match(&g, TR, n1 + n2, ADV_ACT_BIKE, NULL, s, &res);
        T_CHECK_MSG(res.rc == ADV_MATCH_OK && res.n_jumps >= 1, "jump: %s jumps %u searches %u", adv_match_reason(res.rc), res.n_jumps, res.n_searches);
        int whole = 0;
        uint16_t masks[8] = { 0 };
        for (uint32_t k = 0; k < res.n_hits; k++) {
            for (int i = 0; i < 4; i++) {
                if (res.hits[k].seg == wa.seg[i]) masks[i] = res.hits[k].mask;
                if (res.hits[k].seg == wb.seg[i]) masks[4 + i] = res.hits[k].mask;
            }
        }
        for (int i = 0; i < 8; i++) whole += masks[i] == 0xFFFF;
        // Both rides start and end at a node: every block whole.
        T_CHECK_MSG(whole == 8, "jump: %d blocks whole: %04x %04x %04x %04x | %04x %04x %04x %04x", whole, masks[0], masks[1], masks[2], masks[3],
                    masks[4], masks[5], masks[6], masks[7]);
        T_CHECK_MSG(res.matched_m >= 8 * 120 - 30 && res.matched_m <= 8 * 120 + 60, "jump matched %.0f", res.matched_m);
        printf("jump: %u pts, %u samples, %u searches, %u jumps, matched %.0f m\n", n1 + n2, res.n_samples, res.n_searches, res.n_jumps, res.matched_m);
    }
    T_CHECK(strcmp(adv_match_reason(ADV_MATCH_OK), "ok") == 0 && strcmp(adv_match_reason(ADV_MATCH_RJ_BUDGET), "budget") == 0);
    adv_graph_close(&g);
    unlink(path);
}

// A receiver's first fix is often its last position, kilometres from the ride: the trace must
// start at the second point, not lose the first minutes to the speed bound.
static void test_first_fix(adv_match_scratch *s) {
    fg_opts o = { .cols = 12, .rows = 12, .block_m = 120, .shape_m = 40 };
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_match_f");
    adv_graph g;
    T_CHECK(fg_build(&o, path, &g) == 0);
    static walk w;
    rng r = rng_seed(4242);
    walk_make(&g, &r, ADV_ACC_BIKE, 40, &w);
    uint32_t nt = walk_truth(&w, TRUTH);
    float truth_m = adv_hits_len_m(&g, TRUTH, nt);
    uint32_t n = gen_trace(&g, &w, &r, 6, TR, ADV_TRACE_MAX_PTS);
    adv_frame f = adv_frame_at(TR[0].lat);
    TR[0].lat += (int32_t)(3000 / f.my);   // 3 km north of where the ride starts
    adv_match_result res;
    adv_match(&g, TR, n, ADV_ACT_BIKE, NULL, s, &res);
    T_CHECK_MSG(res.rc == ADV_MATCH_OK, "first fix: %s", adv_match_reason(res.rc));
    T_CHECK_MSG(res.n_spikes <= 2, "first fix: %u spikes (the outlier alone is one)", res.n_spikes);
    float common = adv_hits_common_m(&g, TRUTH, nt, res.hits, res.n_hits);
    double recall = truth_m > 0 ? common / truth_m : 0;
    T_CHECK_MSG(recall >= 0.99, "first fix: recall %.4f (%u samples, %u spikes)", recall, res.n_samples, res.n_spikes);
    adv_graph_close(&g);
    unlink(path);
}

int main(void) {
    adv_match_scratch *s = adv_match_scratch_new();
    T_CHECK(s != NULL);
    if (!s) return 1;
    test_walks(s);
    test_hostile(s);
    test_first_fix(s);
    adv_match_scratch_free(s);
    return t_done("unit_match");
}
