// match.c - match.h: validate, downsample, candidates, Viterbi, hits.
//
// THE SCRATCH is one allocation carved into fixed arrays sized by match.h's caps (about 3.4 MB,
// the sum is at scratch_new): a match never allocates, so a worker thread's memory is a fact of
// the build and not of the upload. Two hash tables with generation stamps replace what would be
// per-graph arrays (a stamp over n_segs, a distance over n_nodes), so the scratch is sized by the
// caps alone and knows nothing of the graph it will be used on: the dedupe table holds the
// segments nine cells may list (9 * ADV_SEG_MAX_PER_CELL), the node table what one local search
// may touch (ADV_MATCH_SEARCH expansions of ADV_NODE_MAX_ADJ each).
//
// THE COSTS are additive, lower better. Emission (d / 12 m)^2: 12 m is the GPS error at which a
// candidate costs 1; off-road costs a hair more than the farthest candidate allowed, so it wins
// only when nothing is near. Transition |route - straight| / 15 m: a route that detours 15 m
// more than the samples moved costs 1; a jump (no route found in the budget) costs its 400 m of
// pretend detour plus 2. Into or out of off-road costs 3 plus one per 100 m of the step, and
// staying off-road costs the per-100 m part alone, so a long way off the roads is cheap to stay
// in, a one-sample excursion onto a near street (and back) costs 6, more than any plausible
// noise, and a kilometre's gap is not free just because both its ends are off-road (two on-road
// fixes a kilometre apart are then a jump, which credits them, rather than nothing).
#include "match.h"

#include "util/clock.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_S     ADV_MATCH_MAX_SAMPLES
#define K         ADV_MATCH_K
#define NSTATE    (ADV_MATCH_K + 1)   // the candidates, then the off-road state at index K
#define OFF       ADV_MATCH_K
#define PATH_MAX  32                  // segments one searched transition may pass whole
#define ARENA_N   (ADV_MATCH_MAX_SAMPLES * 8)   // path segments stored for one match, then jumps
#define DD_SLOTS  65536               // > 9 * ADV_SEG_MAX_PER_CELL, so it never fills
#define NS_SLOTS  8192                // > ADV_MATCH_SEARCH * ADV_NODE_MAX_ADJ + 2 seeds
#define HEAP_CAP  (ADV_MATCH_SEARCH * ADV_NODE_MAX_ADJ + 2)
#define NONE      0xFFFFFFFFu
#define INF_COST  3.0e38f

#define EMIT_M    12.0    // metres of projection distance that cost 1
#define TRANS_M   15.0    // metres of route detour that cost 1
#define JUMP_M    400.0   // the detour a failed search is charged
#define JUMP_PEN  2.0f
#define OFF_PEN   3.0
#define OFF_PER_M 0.01
#define SPIKE_SLACK_M  50.0   // what a point may move beyond max_speed * dt before it is a spike
#define GAP_KEEP_MS    60000  // a point this long after the last kept is kept however near
#define BACKSTEP_MS    2000   // a clock may jitter back this much; more is time running backwards
#define BBOX_GROW_M    2000.0
#define MIN_LEN_M      100.0
#define DAY_MS         86400000u

// How a state was reached from its predecessor: what backtrack emits for the pair.
enum { TR_NONE = 0, TR_SAME, TR_ADJ, TR_PATH, TR_JUMP, TR_OFF };
#define KIND(k)      ((k) & 7)
#define END_A(k)     (((k) >> 3) & 1)
#define END_B(k)     (((k) >> 4) & 1)
#define MK(kind, ea, eb) ((uint8_t)((kind) | ((ea) << 3) | ((eb) << 4)))

typedef struct {
    double   x, y;    // frame metres from the first point
    adv_pt   p;
    uint32_t t_ms;
} sample;

typedef struct {
    uint32_t seg;
    float    frac;    // along the segment, by length
    float    dist;    // projection distance, metres
} cand;

typedef struct {
    float    cost;
    uint8_t  pred;    // state index at the previous sample
    uint8_t  kind;    // MK()
    uint8_t  path_n;
    uint8_t  pad;
    uint32_t path_off;   // into the arena, TR_PATH only
} state;

typedef struct {
    uint32_t node, gen;
    float    dist;
    uint32_t pred;      // slot, or NONE for a seed
    uint32_t via;       // the segment walked into this node
    uint8_t  settled, seed_end, pad[2];
} nslot;

typedef struct {
    float    dist;
    uint32_t slot;
} hent;

struct adv_match_scratch {
    sample   *smp;        // MAX_S
    cand     *cands;      // MAX_S * K
    uint8_t  *n_cand;     // MAX_S
    state    *st;         // MAX_S * NSTATE
    uint8_t  *chosen;     // MAX_S, the backtracked state per sample
    uint32_t *arena;      // ARENA_N
    uint32_t  arena_n;
    uint32_t  best_path[PATH_MAX], cur_path[PATH_MAX];
    uint32_t *dd_key, *dd_gen;   // DD_SLOTS each
    uint32_t  dd_cur;
    nslot    *ns;         // NS_SLOTS
    uint32_t  ns_cur;
    hent     *heap;       // HEAP_CAP
    uint32_t  heap_n;
    adv_hit  *hits;       // ADV_MATCH_MAX_HITS
    adv_pt    segpts[ADV_SEG_MAX_PTS];
};

const char *adv_match_reason(int rc) {
    switch (rc) {
    case ADV_MATCH_OK: return "ok";
    case ADV_MATCH_RJ_SHORT: return "short";
    case ADV_MATCH_RJ_COORDS: return "coords";
    case ADV_MATCH_RJ_TIME: return "time";
    case ADV_MATCH_RJ_TELEPORT: return "teleport";
    case ADV_MATCH_RJ_BUDGET: return "budget";
    default: return "unknown";
    }
}

// ---- the scratch ----------------------------------------------------------------------------

#define CARVE(ptr, type, count)                        \
    do {                                               \
        ptr = (type *)(base + at);                     \
        at += ((size_t)(count) * sizeof(type) + 15) & ~(size_t)15; \
    } while (0)

static size_t scratch_bytes(void) {
    // Mirrors the carving below: smp 262 KB, cands 786 KB, st 885 KB, arena 262 KB, dedupe
    // 524 KB, nodes 197 KB, heap 25 KB, hits 262 KB, small arrays: about 3.2 MB in all.
    size_t at = (sizeof(struct adv_match_scratch) + 15) & ~(size_t)15;
    at += ((size_t)MAX_S * sizeof(sample) + 15) & ~(size_t)15;
    at += ((size_t)MAX_S * K * sizeof(cand) + 15) & ~(size_t)15;
    at += ((size_t)MAX_S + 15) & ~(size_t)15;
    at += ((size_t)MAX_S * NSTATE * sizeof(state) + 15) & ~(size_t)15;
    at += ((size_t)MAX_S + 15) & ~(size_t)15;
    at += ((size_t)ARENA_N * sizeof(uint32_t) + 15) & ~(size_t)15;
    at += 2 * (((size_t)DD_SLOTS * sizeof(uint32_t) + 15) & ~(size_t)15);
    at += ((size_t)NS_SLOTS * sizeof(nslot) + 15) & ~(size_t)15;
    at += ((size_t)HEAP_CAP * sizeof(hent) + 15) & ~(size_t)15;
    at += ((size_t)ADV_MATCH_MAX_HITS * sizeof(adv_hit) + 15) & ~(size_t)15;
    return at;
}

adv_match_scratch *adv_match_scratch_new(void) {
    size_t total = scratch_bytes();
    uint8_t *base = calloc(1, total);
    if (!base) return NULL;
    adv_match_scratch *s = (adv_match_scratch *)base;
    size_t at = (sizeof *s + 15) & ~(size_t)15;
    CARVE(s->smp, sample, MAX_S);
    CARVE(s->cands, cand, (size_t)MAX_S * K);
    CARVE(s->n_cand, uint8_t, MAX_S);
    CARVE(s->st, state, (size_t)MAX_S * NSTATE);
    CARVE(s->chosen, uint8_t, MAX_S);
    CARVE(s->arena, uint32_t, ARENA_N);
    CARVE(s->dd_key, uint32_t, DD_SLOTS);
    CARVE(s->dd_gen, uint32_t, DD_SLOTS);
    CARVE(s->ns, nslot, NS_SLOTS);
    CARVE(s->heap, hent, HEAP_CAP);
    CARVE(s->hits, adv_hit, ADV_MATCH_MAX_HITS);
    if (at > total) {   // the mirror above drifted from the carving: refuse rather than overrun
        free(base);
        return NULL;
    }
    // Generation 0 is "never used" in both tables; the counters start at 1.
    s->dd_cur = 1;
    s->ns_cur = 1;
    return s;
}

void adv_match_scratch_free(adv_match_scratch *s) { free(s); }

// ---- the dedupe table (segments already listed for this sample) -------------------------------

static inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static void dd_next(adv_match_scratch *s) {
    if (++s->dd_cur == 0) {   // wrapped: every stale stamp could collide with 0, so clear them
        memset(s->dd_gen, 0, (size_t)DD_SLOTS * sizeof(uint32_t));
        s->dd_cur = 1;
    }
}

// True when seg was already seen this generation; marks it seen otherwise.
static bool dd_seen(adv_match_scratch *s, uint32_t seg) {
    uint32_t i = hash32(seg) & (DD_SLOTS - 1);
    for (uint32_t probe = 0; probe < DD_SLOTS; probe++, i = (i + 1) & (DD_SLOTS - 1)) {
        if (s->dd_gen[i] != s->dd_cur) {
            s->dd_gen[i] = s->dd_cur;
            s->dd_key[i] = seg;
            return false;
        }
        if (s->dd_key[i] == seg) return true;
    }
    return false;   // full (cannot be: the table is bigger than nine cells' lists)
}

// ---- the node table and heap of one local search -------------------------------------------

static void ns_next(adv_match_scratch *s) {
    if (++s->ns_cur == 0) {
        memset(s->ns, 0, (size_t)NS_SLOTS * sizeof(nslot));
        s->ns_cur = 1;
    }
    s->heap_n = 0;
}

// The slot of node, created (dist INF, unsettled) when new; NONE when the table is full.
static uint32_t ns_slot(adv_match_scratch *s, uint32_t node) {
    uint32_t i = hash32(node) & (NS_SLOTS - 1);
    for (uint32_t probe = 0; probe < NS_SLOTS; probe++, i = (i + 1) & (NS_SLOTS - 1)) {
        nslot *n = &s->ns[i];
        if (n->gen != s->ns_cur) {
            n->gen = s->ns_cur;
            n->node = node;
            n->dist = INF_COST;
            n->pred = NONE;
            n->via = NONE;
            n->settled = 0;
            n->seed_end = 0;
            return i;
        }
        if (n->node == node) return i;
    }
    return NONE;
}

static void heap_push(adv_match_scratch *s, float dist, uint32_t slot) {
    if (s->heap_n >= HEAP_CAP) return;   // bounded: a dropped push is a route not found, never a fault
    uint32_t i = s->heap_n++;
    while (i > 0) {
        uint32_t p = (i - 1) / 2;
        if (s->heap[p].dist <= dist) break;
        s->heap[i] = s->heap[p];
        i = p;
    }
    s->heap[i].dist = dist;
    s->heap[i].slot = slot;
}

static hent heap_pop(adv_match_scratch *s) {
    hent top = s->heap[0];
    hent last = s->heap[--s->heap_n];
    uint32_t i = 0;
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        float md = last.dist;
        if (l < s->heap_n && s->heap[l].dist < md) { m = l; md = s->heap[l].dist; }
        if (r < s->heap_n && s->heap[r].dist < md) { m = r; }
        if (m == i) break;
        s->heap[i] = s->heap[m];
        i = m;
    }
    if (s->heap_n > 0) s->heap[i] = last;
    return top;
}

// Metres along seg from frac to its end (0: node_a, 1: node_b).
static inline double along_to_end(const adv_graph *g, uint32_t seg, double frac, int end) {
    double len = g->segs[seg].len_m;
    return end ? (1.0 - frac) * len : frac * len;
}

// The route from frac fa on a to frac fb on b, through the nodes, by Dijkstra from a's two ends
// (seeded with the distance from the projection to each), stopping once no unsettled node is
// nearer than the best route found (so a node of b settled first does not hide a shorter route
// via b's other node). The route's segments, b's end entered and a's end left are returned for
// the hits. False: no route within the cutoff, the expansion cap or PATH_MAX segments.
static bool local_search(const adv_graph *g, adv_match_scratch *s, uint32_t a, double fa, uint32_t b, double fb,
                         double cutoff, double *route_m, uint32_t *path, uint8_t *path_n, int *end_a, int *end_b) {
    ns_next(s);
    const adv_seg *sa = &g->segs[a], *sb = &g->segs[b];
    for (int e = 0; e < 2; e++) {
        uint32_t node = e ? sa->node_b : sa->node_a;
        uint32_t slot = ns_slot(s, node);
        if (slot == NONE) return false;
        float d = (float)along_to_end(g, a, fa, e);
        if (d < s->ns[slot].dist) {   // a loop segment seeds the same node twice: keep the nearer
            s->ns[slot].dist = d;
            s->ns[slot].seed_end = (uint8_t)e;
            heap_push(s, d, slot);
        }
    }
    float best = INF_COST;
    uint32_t best_slot = NONE;
    int best_end_b = 0;
    uint32_t expansions = 0;
    while (s->heap_n > 0 && expansions < ADV_MATCH_SEARCH) {
        hent h = heap_pop(s);
        nslot *n = &s->ns[h.slot];
        if (n->settled || h.dist > n->dist) continue;   // a stale copy
        if (h.dist >= best || h.dist > cutoff) break;
        n->settled = 1;
        expansions++;
        uint32_t node = n->node;
        if (node == sb->node_a) {
            float tot = h.dist + (float)along_to_end(g, b, fb, 0);
            if (tot < best) { best = tot; best_slot = h.slot; best_end_b = 0; }
        }
        if (node == sb->node_b) {
            float tot = h.dist + (float)along_to_end(g, b, fb, 1);
            if (tot < best) { best = tot; best_slot = h.slot; best_end_b = 1; }
        }
        const adv_node *nd = &g->nodes[node];
        for (uint32_t k = 0; k < nd->n_adj; k++) {
            uint32_t e = g->adj[nd->adj_off + k];
            // Along a lands on a's other seed; along b lands on b's other node, which the direct
            // projection distance already prices. Neither can shorten a route.
            if (e == a || e == b) continue;
            const adv_seg *se = &g->segs[e];
            uint32_t other = se->node_a == node ? se->node_b : se->node_a;
            float ndist = h.dist + se->len_m;
            if (ndist > cutoff) continue;
            uint32_t os = ns_slot(s, other);
            if (os == NONE) continue;
            nslot *on = &s->ns[os];
            if (on->settled || ndist >= on->dist) continue;
            on->dist = ndist;
            on->pred = h.slot;
            on->via = e;
            heap_push(s, ndist, os);
        }
    }
    if (best_slot == NONE) return false;
    // Walk the predecessors back to the seed: the segments passed whole, and which end of a.
    uint8_t n = 0;
    uint32_t slot = best_slot;
    for (uint32_t steps = 0; steps <= NS_SLOTS; steps++) {
        const nslot *ns = &s->ns[slot];
        if (ns->pred == NONE) {
            *end_a = ns->seed_end;
            *end_b = best_end_b;
            *route_m = best;
            *path_n = n;
            return true;
        }
        if (n >= PATH_MAX) return false;
        path[n++] = ns->via;
        slot = ns->pred;
    }
    return false;   // a cycle in the predecessors: impossible, but bounded
}

// ---- candidates ------------------------------------------------------------------------------

// The projection of p onto seg: the distance, and in *frac where along the segment by length.
static double project(const adv_graph *g, adv_match_scratch *s, const adv_frame *f, adv_pt p, uint32_t seg, double *frac) {
    int n = adv_seg_points(g, seg, s->segpts, ADV_SEG_MAX_PTS);
    if (n < 2) {
        *frac = 0;
        return adv_dist_m(f, p, s->segpts[0]);
    }
    double best = DBL_MAX, best_at = 0, acc = 0, total = 0;
    for (int i = 1; i < n; i++) {
        double t, elen = adv_dist_m(f, s->segpts[i - 1], s->segpts[i]);
        double d = adv_point_seg_m(f, p, s->segpts[i - 1], s->segpts[i], &t);
        if (d < best) {
            best = d;
            best_at = acc + t * elen;
        }
        acc += elen;
    }
    total = acc;
    double fr = total > 0 ? best_at / total : 0;
    if (fr < 0) fr = 0;
    if (fr > 1) fr = 1;
    *frac = fr;
    return best;
}

// The candidates of sample i: every segment listed by its cell and the eight around it, within
// radius, with the type's access bit, the nearest ADV_MATCH_K kept. Returns the count.
static int gather(const adv_graph *g, adv_match_scratch *s, const adv_frame *f, adv_pt p, uint8_t access,
                  double radius, int32_t rlat, int32_t rlon, cand *out) {
    // The cell, clamped onto the grid when the point is within one cell of it: a road along the
    // edge of the extract is still a road, and a fix a few metres outside the file's bounding box
    // still belongs to it (the segment bounding boxes below reject what is really far).
    uint32_t tx, ty, ci, cj;
    adv_tile_of(p, ADV_CELL_ZOOM, &tx, &ty);
    const adv_graph_hdr *h = g->h;
    if (tx + 1 < h->cell_x0 || ty + 1 < h->cell_y0 || tx > h->cell_x0 + h->cells_w || ty > h->cell_y0 + h->cells_h) return 0;
    ci = tx < h->cell_x0 ? 0 : tx - h->cell_x0 >= h->cells_w ? h->cells_w - 1 : tx - h->cell_x0;
    cj = ty < h->cell_y0 ? 0 : ty - h->cell_y0 >= h->cells_h ? h->cells_h - 1 : ty - h->cell_y0;
    dd_next(s);
    int nc = 0;
    for (int dj = -1; dj <= 1; dj++) {
        for (int di = -1; di <= 1; di++) {
            int64_t ii = (int64_t)ci + di, jj = (int64_t)cj + dj;
            if (ii < 0 || jj < 0) continue;
            uint32_t n;
            const uint32_t *list = adv_cell_list(g, (uint32_t)ii, (uint32_t)jj, &n);
            for (uint32_t k = 0; k < n; k++) {
                uint32_t seg = list[k];
                const adv_seg *sg = &g->segs[seg];
                if (sg->max_lat < p.lat - rlat || sg->min_lat > p.lat + rlat || sg->max_lon < p.lon - rlon || sg->min_lon > p.lon + rlon) continue;
                if (!(sg->access & access)) continue;
                if (dd_seen(s, seg)) continue;
                double fr, d = project(g, s, f, p, seg, &fr);
                if (d > radius) continue;
                if (nc == K && d >= out[K - 1].dist) continue;
                int at = nc < K ? nc : K - 1;
                while (at > 0 && out[at - 1].dist > d) {
                    out[at] = out[at - 1];
                    at--;
                }
                out[at].seg = seg;
                out[at].frac = (float)fr;
                out[at].dist = (float)d;
                if (nc < K) nc++;
            }
        }
    }
    return nc;
}

// ---- downsample -----------------------------------------------------------------------------

// Walks the points once with the spacing sample_m: kept samples into s->smp (at most MAX_S, the
// rest counted in *overflow), spikes counted, the kept path's length in *len_m. The count kept.
static uint32_t downsample(adv_match_scratch *s, const adv_trace_pt *pts, uint32_t n, const adv_frame *f,
                           double sample_m, double max_speed, uint32_t *spikes, uint32_t *overflow, double *len_m) {
    adv_pt o = { pts[0].lat, pts[0].lon };
    uint32_t kept = 0;
    *spikes = 0;
    *overflow = 0;
    *len_m = 0;
    double lx = 0, ly = 0;
    uint32_t lt = pts[0].t_ms;
    for (uint32_t i = 0; i < n; i++) {
        double x = ((double)pts[i].lon - o.lon) * f->mx, y = ((double)pts[i].lat - o.lat) * f->my;
        if (kept == 0) {
            s->smp[0].x = x;
            s->smp[0].y = y;
            s->smp[0].p = o;
            s->smp[0].t_ms = pts[i].t_ms;
            kept = 1;
            lx = x;
            ly = y;
            lt = pts[i].t_ms;
            continue;
        }
        double dt = pts[i].t_ms >= lt ? (pts[i].t_ms - lt) / 1000.0 : 0;
        double d = hypot(x - lx, y - ly);
        if (d > max_speed * dt + SPIKE_SLACK_M) {   // farther than anyone moves: a spike, not a move
            (*spikes)++;
            continue;
        }
        uint32_t dt_ms = pts[i].t_ms >= lt ? pts[i].t_ms - lt : 0;
        // The last point, and the last before a gap, are kept however near: where the rider was
        // last seen is where the ride's (or the stretch's) last metres are credited.
        bool last = i + 1 >= n || pts[i + 1].t_ms > pts[i].t_ms + GAP_KEEP_MS;
        if (d < sample_m && dt_ms <= GAP_KEEP_MS && !last) continue;
        *len_m += d;
        if (kept < MAX_S) {
            s->smp[kept].x = x;
            s->smp[kept].y = y;
            s->smp[kept].p.lat = pts[i].lat;
            s->smp[kept].p.lon = pts[i].lon;
            s->smp[kept].t_ms = pts[i].t_ms;
        } else {
            (*overflow)++;
        }
        kept++;
        lx = x;
        ly = y;
        lt = pts[i].t_ms;
    }
    return kept;
}

// ---- hits -----------------------------------------------------------------------------------

static void emit(adv_match_scratch *s, uint32_t *n_hits, uint32_t *dropped, uint32_t seg, uint16_t mask) {
    if (*n_hits >= ADV_MATCH_MAX_HITS) {
        (*dropped)++;
        return;
    }
    s->hits[*n_hits].seg = seg;
    s->hits[*n_hits].mask = mask;
    s->hits[*n_hits].pad = 0;
    (*n_hits)++;
}

static inline bool is_run(int kind) { return kind == TR_SAME || kind == TR_ADJ || kind == TR_PATH; }

// ---- the match ------------------------------------------------------------------------------

void adv_match(const adv_graph *g, const adv_trace_pt *pts, uint32_t n, uint8_t type, const adv_match_opts *opts_in,
               adv_match_scratch *s, adv_match_result *out) {
    int64_t cpu0 = cb_thread_cpu_us();
    memset(out, 0, sizeof *out);
    out->n_points = n;
    out->hits = s->hits;
    adv_match_opts o = opts_in ? *opts_in : (adv_match_opts){ 0 };
    if (o.cpu_budget_us <= 0) o.cpu_budget_us = 200000;
    if (!(o.cand_radius_m > 0)) o.cand_radius_m = 35;
    if (!(o.sample_m > 0)) o.sample_m = 15;
    if (!(o.max_speed_mps > 0)) o.max_speed_mps = type == ADV_ACT_BIKE ? 30 : 12;
    uint8_t access = type == ADV_ACT_BIKE ? ADV_ACC_BIKE : ADV_ACC_FOOT;
    int rc = ADV_MATCH_OK;

    // Validation: every point on the planet and in order, at least one near the graph.
    adv_frame gf = adv_frame_at((int32_t)(((int64_t)g->h->min_lat + g->h->max_lat) / 2));
    int32_t glat = (int32_t)(BBOX_GROW_M / gf.my), glon = (int32_t)(BBOX_GROW_M / (gf.mx > 1e-12 ? gf.mx : 1e-12));
    if (glon < 0 || glon > ADV_LON_MAX) glon = ADV_LON_MAX;
    bool any_inside = false;
    for (uint32_t i = 0; i < n && rc == ADV_MATCH_OK; i++) {
        adv_pt p = { pts[i].lat, pts[i].lon };
        if (!adv_pt_valid(p)) rc = ADV_MATCH_RJ_COORDS;
        else if (pts[i].t_ms > 2 * DAY_MS) rc = ADV_MATCH_RJ_TIME;
        else if (i > 0 && pts[i].t_ms + BACKSTEP_MS < pts[i - 1].t_ms) rc = ADV_MATCH_RJ_TIME;
        else if (p.lat >= g->h->min_lat - glat && p.lat <= g->h->max_lat + glat &&
                 p.lon >= g->h->min_lon - glon && p.lon <= g->h->max_lon + glon)
            any_inside = true;
    }
    if (rc == ADV_MATCH_OK && n < 2) rc = ADV_MATCH_RJ_SHORT;
    if (rc == ADV_MATCH_OK && !any_inside) rc = ADV_MATCH_RJ_COORDS;

    // Downsample, once more with a wider spacing when the cap would overflow.
    adv_frame f = adv_frame_at(rc == ADV_MATCH_OK ? pts[0].lat : 0);
    uint32_t ns = 0, spikes = 0, overflow = 0;
    double len_m = 0, sample_m = o.sample_m;
    if (rc == ADV_MATCH_OK) {
        ns = downsample(s, pts, n, &f, sample_m, o.max_speed_mps, &spikes, &overflow, &len_m);
        if (ns > MAX_S) {
            double wider = len_m / MAX_S * 1.1;
            if (wider > sample_m) sample_m = wider;
            ns = downsample(s, pts, n, &f, sample_m, o.max_speed_mps, &spikes, &overflow, &len_m);
            if (ns > MAX_S) ns = MAX_S;   // still over: the tail is cut, the budget holds
        }
        out->n_spikes = spikes;
        if (spikes > 20 && spikes * 2 > n) rc = ADV_MATCH_RJ_TELEPORT;
        else if (ns < 2 || len_m < MIN_LEN_M) rc = ADV_MATCH_RJ_SHORT;
    }
    out->n_samples = ns;
    out->trace_m = (float)len_m;
    if (rc != ADV_MATCH_OK) {
        out->rc = rc;
        out->cpu_us = (uint32_t)(cb_thread_cpu_us() - cpu0);
        return;
    }

    // Candidates and Viterbi, sample by sample, the budget checked every ADV_MATCH_BUDGET_EVERY.
    int32_t rlat = (int32_t)(o.cand_radius_m / f.my) + 1, rlon = (int32_t)(o.cand_radius_m / (f.mx > 1e-12 ? f.mx : 1e-12)) + 1;
    if (rlon < 0) rlon = ADV_LON_MAX;
    const float off_emit = (float)((o.cand_radius_m / EMIT_M) * (o.cand_radius_m / EMIT_M) + 1.0);
    s->arena_n = 0;
    uint32_t n_searches = 0, n_jumps = 0, n_fast = 0, n_offroad = 0;
    for (uint32_t i = 0; i < ns; i++) {
        if (i % ADV_MATCH_BUDGET_EVERY == 0 && cb_thread_cpu_us() - cpu0 > o.cpu_budget_us) {
            rc = ADV_MATCH_RJ_BUDGET;
            break;
        }
        const sample *sp = &s->smp[i];
        adv_pt p = sp->p;
        cand *cc = &s->cands[(size_t)i * K];
        int nc = gather(g, s, &f, p, access, o.cand_radius_m, rlat, rlon, cc);
        s->n_cand[i] = (uint8_t)nc;
        if (nc == 0) n_offroad++;
        state *st = &s->st[(size_t)i * NSTATE];
        for (int j = 0; j < NSTATE; j++) {
            st[j].cost = INF_COST;
            st[j].pred = OFF;
            st[j].kind = TR_NONE;
            st[j].path_n = 0;
            st[j].path_off = 0;
        }
        if (i == 0) {
            for (int j = 0; j < nc; j++) st[j].cost = (float)((cc[j].dist / EMIT_M) * (cc[j].dist / EMIT_M));
            st[OFF].cost = off_emit;
            continue;
        }
        const sample *pp = &s->smp[i - 1];
        const state *ps = &s->st[(size_t)(i - 1) * NSTATE];
        const cand *pc = &s->cands[(size_t)(i - 1) * K];
        int pnc = s->n_cand[i - 1];
        double straight = hypot(sp->x - pp->x, sp->y - pp->y);
        float off_step = (float)(OFF_PEN + straight * OFF_PER_M);
        for (int j = 0; j < nc; j++) {
            uint32_t b = cc[j].seg;
            double fb = cc[j].frac;
            const adv_seg *sb = &g->segs[b];
            float best = INF_COST;
            uint8_t best_pred = OFF, best_kind = TR_NONE, best_n = 0;
            // From off-road: the entry penalty.
            if (ps[OFF].cost < INF_COST) {
                best = ps[OFF].cost + off_step;
                best_pred = OFF;
                best_kind = TR_OFF;
            }
            // The fast path first: same segment, or two that share a node.
            uint8_t fast[K];
            for (int k = 0; k < pnc; k++) {
                fast[k] = 0;
                if (ps[k].cost >= INF_COST) { fast[k] = 1; continue; }
                uint32_t a = pc[k].seg;
                double fa = pc[k].frac, route;
                uint8_t kind;
                if (a == b) {
                    route = fabs(fa - fb) * sb->len_m;
                    kind = TR_SAME;
                } else {
                    const adv_seg *sa = &g->segs[a];
                    double r = DBL_MAX;
                    int ea = 0, eb = 0;
                    for (int x = 0; x < 2; x++) {
                        uint32_t na = x ? sa->node_b : sa->node_a;
                        for (int y = 0; y < 2; y++) {
                            uint32_t nb = y ? sb->node_b : sb->node_a;
                            if (na != nb) continue;
                            double rr = along_to_end(g, a, fa, x) + along_to_end(g, b, fb, y);
                            if (rr < r) { r = rr; ea = x; eb = y; }
                        }
                    }
                    if (r == DBL_MAX) continue;   // not adjacent: the slow path below
                    route = r;
                    kind = MK(TR_ADJ, ea, eb);
                }
                fast[k] = 1;
                n_fast++;
                float c = ps[k].cost + (float)(fabs(route - straight) / TRANS_M);
                if (c < best) { best = c; best_pred = (uint8_t)k; best_kind = kind; best_n = 0; }
            }
            // The slow path: a bounded search, skipped when the predecessor is already dearer
            // than the best found (a transition never costs less than nothing).
            for (int k = 0; k < pnc; k++) {
                if (fast[k] || ps[k].cost >= best) continue;
                uint32_t a = pc[k].seg;
                double fa = pc[k].frac, route;
                uint8_t pn = 0;
                int ea = 0, eb = 0;
                n_searches++;
                float c;
                uint8_t kind;
                if (local_search(g, s, a, fa, b, fb, 3 * straight + 200, &route, s->cur_path, &pn, &ea, &eb)) {
                    c = ps[k].cost + (float)(fabs(route - straight) / TRANS_M);
                    kind = MK(TR_PATH, ea, eb);
                } else {
                    n_jumps++;
                    route = straight + JUMP_M;
                    c = ps[k].cost + (float)(fabs(route - straight) / TRANS_M) + JUMP_PEN;
                    kind = TR_JUMP;
                    pn = 0;
                }
                if (c < best) {
                    best = c;
                    best_pred = (uint8_t)k;
                    best_kind = kind;
                    best_n = pn;
                    if (pn) memcpy(s->best_path, s->cur_path, (size_t)pn * sizeof(uint32_t));
                }
            }
            st[j].cost = best + (float)((cc[j].dist / EMIT_M) * (cc[j].dist / EMIT_M));
            st[j].pred = best_pred;
            st[j].kind = best_kind;
            if (KIND(best_kind) == TR_PATH && best_n > 0) {
                if (s->arena_n + best_n <= ARENA_N) {
                    st[j].path_off = s->arena_n;
                    st[j].path_n = best_n;
                    memcpy(s->arena + s->arena_n, s->best_path, (size_t)best_n * sizeof(uint32_t));
                    s->arena_n += best_n;
                } else {
                    st[j].kind = TR_JUMP;   // the arena is full: the chain stands, the whole segments are not credited
                }
            }
        }
        // The off-road state: stay for the distance alone, or leave a candidate for the exit penalty.
        {
            float best = ps[OFF].cost < INF_COST ? ps[OFF].cost + (float)(straight * OFF_PER_M) : INF_COST;
            uint8_t best_pred = OFF, best_kind = TR_NONE;
            for (int k = 0; k < pnc; k++) {
                if (ps[k].cost >= INF_COST) continue;
                float c = ps[k].cost + off_step;
                if (c < best) { best = c; best_pred = (uint8_t)k; best_kind = TR_OFF; }
            }
            st[OFF].cost = best + off_emit;
            st[OFF].pred = best_pred;
            st[OFF].kind = best_kind;
        }
    }
    out->n_offroad = n_offroad;
    out->n_searches = n_searches;
    out->n_jumps = n_jumps;
    out->n_fast = n_fast;
    if (rc != ADV_MATCH_OK) {
        out->rc = rc;
        out->cpu_us = (uint32_t)(cb_thread_cpu_us() - cpu0);
        return;
    }

    // Backtrack the cheapest final state.
    {
        const state *last = &s->st[(size_t)(ns - 1) * NSTATE];
        int bj = OFF;
        for (int j = 0; j < s->n_cand[ns - 1]; j++)
            if (last[j].cost < last[bj].cost) bj = j;
        for (uint32_t i = ns; i-- > 0;) {
            s->chosen[i] = (uint8_t)bj;
            bj = s->st[(size_t)i * NSTATE + bj].pred;
        }
    }
    // Hits: the run between consecutive on-road projections, whole segments a route passed, and
    // a lone sample's or a jump's single sixteenth.
    uint32_t n_hits = 0, dropped = 0;
    for (uint32_t i = 0; i < ns; i++) {
        int j = s->chosen[i];
        if (j == OFF) continue;
        const cand *c = &s->cands[(size_t)i * K + j];
        const state *st = &s->st[(size_t)i * NSTATE + j];
        int in_kind = KIND(st->kind);
        int out_kind = TR_NONE;
        if (i + 1 < ns && s->chosen[i + 1] != OFF) out_kind = KIND(s->st[(size_t)(i + 1) * NSTATE + s->chosen[i + 1]].kind);
        if ((!is_run(in_kind) && !is_run(out_kind)) || in_kind == TR_JUMP || out_kind == TR_JUMP)
            emit(s, &n_hits, &dropped, c->seg, adv_mask_of_run(c->frac, c->frac));
        if (!is_run(in_kind)) continue;
        const cand *pc = &s->cands[(size_t)(i - 1) * K + st->pred];
        if (in_kind == TR_SAME) {
            emit(s, &n_hits, &dropped, c->seg, adv_mask_of_run(pc->frac, c->frac));
            continue;
        }
        emit(s, &n_hits, &dropped, pc->seg, adv_mask_of_run(pc->frac, END_A(st->kind) ? 1.0 : 0.0));
        emit(s, &n_hits, &dropped, c->seg, adv_mask_of_run(END_B(st->kind) ? 1.0 : 0.0, c->frac));
        if (in_kind == TR_PATH) {
            for (uint32_t k = 0; k < st->path_n; k++) {
                uint32_t seg = s->arena[st->path_off + k];
                if (seg == pc->seg || seg == c->seg) continue;
                emit(s, &n_hits, &dropped, seg, 0xFFFF);
            }
        }
    }
    out->n_hits = adv_hits_finish(s->hits, n_hits);
    out->matched_m = adv_hits_len_m(g, s->hits, out->n_hits);
    out->rc = ADV_MATCH_OK;
    out->cpu_us = (uint32_t)(cb_thread_cpu_us() - cpu0);
}
