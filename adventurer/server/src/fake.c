// fake.c - fake.h: the users, the walker, the sampler, the hostile makers.
#include "fake.h"

#include "coverage.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// ---- the PRNG -------------------------------------------------------------------------------
// splitmix64 spreads a small seed (1, 2, 3...) over the whole word, so neighbouring seeds do not
// give correlated xorshift streams; xorshift64* then needs a non-zero state, which it guarantees.
static uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

void fake_rng_seed(fake_rng *r, uint64_t seed) {
    r->s = splitmix64(seed);
    if (!r->s) r->s = 0x9E3779B97F4A7C15ull;
}

uint64_t fake_rng_u64(fake_rng *r) {
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

uint32_t fake_rng_below(fake_rng *r, uint32_t n) {
    if (n == 0) return 0;
    return (uint32_t)(((uint64_t)(uint32_t)(fake_rng_u64(r) >> 32) * n) >> 32);
}

double fake_rng_unit(fake_rng *r) { return (double)(fake_rng_u64(r) >> 11) * (1.0 / 9007199254740992.0); }

double fake_rng_range(fake_rng *r, double lo, double hi) { return lo + (hi - lo) * fake_rng_unit(r); }

double fake_rng_normal(fake_rng *r) {
    // Box-Muller; u1 is kept away from 0 so the log is finite.
    double u1 = 1.0 - fake_rng_unit(r), u2 = fake_rng_unit(r);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

// ---- users ----------------------------------------------------------------------------------
static const char *const FIRST_A[16] = { "Ma", "Le", "To", "Ani", "Ka", "Jo", "El", "Ro", "Sa", "Mi", "Da", "Vi", "Ha", "Ne", "Ol", "Ta" };
static const char *const FIRST_B[16] = { "ra", "na", "mas", "ka", "ri", "sh", "sa", "ren", "ya", "lo", "vid", "ta", "ne", "la", "ver", "mi" };
static const char *const LAST_A[16] = { "Oak", "Ash", "Stone", "River", "Hill", "Wood", "Fair", "Black", "Green", "Marsh", "Thorn", "Wester", "Fox", "Birch", "Cold", "Sun" };
static const char *const LAST_B[16] = { "hurst", "worth", "field", "brook", "man", "ley", "wood", "burn", "ford", "croft", "dale", "ton", "well", "moor", "gate", "wick" };

uint8_t fake_kind_act(uint8_t kind) {
    switch (kind) {
    case ADV_UK_RUNNER: return ADV_ACT_RUN;
    case ADV_UK_WALKER: return ADV_ACT_WALK;
    default: return ADV_ACT_BIKE;
    }
}

uint8_t fake_act_access(uint8_t act) { return act == ADV_ACT_BIKE ? ADV_ACC_BIKE : ADV_ACC_FOOT; }

const char *fake_kind_name(uint8_t kind) {
    switch (kind) {
    case ADV_UK_COMMUTER: return "commuter";
    case ADV_UK_EXPLORER: return "explorer";
    case ADV_UK_RUNNER: return "runner";
    case ADV_UK_WALKER: return "walker";
    case ADV_UK_TOURIST: return "tourist";
    default: return "?";
    }
}

// Everything about a user comes from this stream, so the roster is a function of the seed.
static void user_rng(fake_rng *r, uint64_t seed, uint32_t id) {
    fake_rng_seed(r, splitmix64(seed) ^ ((uint64_t)id * 0xD6E8FEB86659FD93ull));
}

void fake_user_def(const adv_graph *g, uint64_t seed, uint32_t id, adv_w_user_def *def, uint32_t *home_seg) {
    fake_rng r;
    user_rng(&r, seed, id);
    memset(def, 0, sizeof *def);
    def->id = id;
    double k = fake_rng_unit(&r);
    def->kind = k < 0.35 ? ADV_UK_COMMUTER : k < 0.60 ? ADV_UK_EXPLORER : k < 0.75 ? ADV_UK_RUNNER : k < 0.90 ? ADV_UK_WALKER : ADV_UK_TOURIST;
    // Home: the first point of a segment the kind can travel, so the first route has a way out.
    // A few random draws almost always find one; the scan is for a graph that is nearly all of
    // one access, and is bounded by the segment count.
    uint8_t access = fake_act_access(fake_kind_act(def->kind));
    uint32_t seg = 0;
    if (g->h->n_segs > 0) {
        seg = fake_rng_below(&r, g->h->n_segs);
        bool ok = (g->segs[seg].access & access) != 0;
        for (int tries = 0; !ok && tries < 64; tries++) {
            seg = fake_rng_below(&r, g->h->n_segs);
            ok = (g->segs[seg].access & access) != 0;
        }
        for (uint32_t i = 0; !ok && i < g->h->n_segs; i++) {
            uint32_t s = (seg + i) % g->h->n_segs;
            if (g->segs[s].access & access) { seg = s; ok = true; }
        }
        def->home_lat = g->segs[seg].lat;
        def->home_lon = g->segs[seg].lon;
    }
    if (home_seg) *home_seg = seg;
    int n = snprintf(def->name, sizeof def->name, "%s%s %s%s", FIRST_A[fake_rng_below(&r, 16)], FIRST_B[fake_rng_below(&r, 16)],
                     LAST_A[fake_rng_below(&r, 16)], LAST_B[fake_rng_below(&r, 16)]);
    if (n < 0) n = 0;
    if (n > ADV_W_NAME - 1) n = ADV_W_NAME - 1;
    def->name_len = (uint8_t)n;
}

void fake_users_make(const adv_graph *g, uint32_t n, uint64_t seed, adv_w_user_def *out) {
    for (uint32_t i = 0; i < n; i++) fake_user_def(g, seed, i + 1, &out[i], NULL);
}

void fake_user_init(const adv_graph *g, fake_user *u, uint64_t seed, uint32_t id) {
    memset(u, 0, sizeof *u);
    fake_user_def(g, seed, id, &u->def, &u->home_seg);
    u->home_node = g->h->n_segs > 0 ? g->segs[u->home_seg].node_a : 0;
    for (uint32_t i = 0; i < FAKE_VISITED_CAP; i++) u->visited[i] = FAKE_NO_SEG;
}

static uint32_t visited_slot(uint32_t seg) { return (seg * 0x9E3779B1u) >> (32 - 12); }
_Static_assert(FAKE_VISITED_CAP == 1u << 12, "visited_slot's shift is the table's size");

bool fake_user_visited(const fake_user *u, uint32_t seg) {
    uint32_t i = visited_slot(seg);
    for (uint32_t probe = 0; probe < FAKE_VISITED_CAP; probe++, i = (i + 1) & (FAKE_VISITED_CAP - 1)) {
        if (u->visited[i] == seg) return true;
        if (u->visited[i] == FAKE_NO_SEG) return false;
    }
    return false;
}

static void visited_add(fake_user *u, uint32_t seg) {
    if (u->n_visited >= FAKE_VISITED_FILL) return;   // full: the set stops learning, and every probe still ends
    uint32_t i = visited_slot(seg);
    for (uint32_t probe = 0; probe < FAKE_VISITED_CAP; probe++, i = (i + 1) & (FAKE_VISITED_CAP - 1)) {
        if (u->visited[i] == seg) return;
        if (u->visited[i] == FAKE_NO_SEG) {
            u->visited[i] = seg;
            u->n_visited++;
            return;
        }
    }
}

// ---- routes ---------------------------------------------------------------------------------
double fake_target_m(fake_rng *r, uint8_t act) {
    switch (act) {
    case ADV_ACT_RUN: return fake_rng_range(r, 6000, 12000);
    case ADV_ACT_WALK: return fake_rng_range(r, 3000, 6000);
    default: return fake_rng_range(r, 20000, 40000);
    }
}

// Whether `seg` may be travelled by `act` leaving `from`; the direction into *reversed. A bike
// does not ride a oneway backwards (a dead-end turn-around is the one exception, made by the
// caller).
static bool leg_usable(const adv_graph *g, uint32_t seg, uint8_t act, uint32_t from, bool *reversed) {
    const adv_seg *s = &g->segs[seg];
    if (!(s->access & fake_act_access(act))) return false;
    if (s->node_a == from) *reversed = false;
    else if (s->node_b == from) *reversed = true;
    else return false;   // a node listing a segment that does not end there: a damaged file
    if (act == ADV_ACT_BIKE && (s->flags & ADV_SEGF_ONEWAY) && *reversed) return false;
    return true;
}

static double node_dist_m(const adv_graph *g, const adv_frame *f, uint32_t a, uint32_t b) {
    adv_pt pa = { g->nodes[a].lat, g->nodes[a].lon }, pb = { g->nodes[b].lat, g->nodes[b].lon };
    return adv_dist_m(f, pa, pb);
}

// The next leg out of `node`: among the node's segments the type may use, not the one just
// travelled unless nothing else is, weighted by the kind and (homeward) by whether the far end
// is nearer home. False at a dead end with no way back.
static bool choose_leg(const adv_graph *g, const fake_user *u, fake_rng *r, uint8_t act, uint32_t node, uint32_t prev_seg,
                       bool homeward, const adv_frame *f, fake_leg *out) {
    const adv_node *nd = &g->nodes[node];
    uint32_t cand[ADV_NODE_MAX_ADJ];
    bool rev[ADV_NODE_MAX_ADJ];
    double w[ADV_NODE_MAX_ADJ];
    int n = 0;
    double sum = 0, d_cur = homeward ? node_dist_m(g, f, node, u->home_node) : 0;
    uint32_t n_adj = nd->n_adj < ADV_NODE_MAX_ADJ ? nd->n_adj : ADV_NODE_MAX_ADJ;
    for (uint32_t k = 0; k < n_adj; k++) {
        uint32_t seg = g->adj[nd->adj_off + k];
        if (seg == prev_seg || seg >= g->h->n_segs) continue;
        bool rv;
        if (!leg_usable(g, seg, act, node, &rv)) continue;
        double wt = 1.0;
        bool seen = fake_user_visited(u, seg);
        switch (u->def.kind) {
        case ADV_UK_COMMUTER: wt = seen ? 4.0 : 1.0; break;      // the same roads, again
        case ADV_UK_TOURIST: wt = 1.0; break;                     // anywhere
        default: wt = seen ? 0.15 : 1.0; break;                   // explorer, runner, walker: new roads
        }
        if (homeward) {
            uint32_t far = rv ? g->segs[seg].node_a : g->segs[seg].node_b;
            wt *= node_dist_m(g, f, far, u->home_node) < d_cur ? 4.0 : 0.3;
            if (far == u->home_node) wt *= 4.0;
        }
        cand[n] = seg;
        rev[n] = rv;
        w[n] = wt;
        sum += wt;
        n++;
    }
    if (n == 0) {
        // A dead end: turn around on the segment that brought us here, if any still allows it.
        bool rv;
        if (prev_seg == FAKE_NO_SEG) return false;
        if (!leg_usable(g, prev_seg, act, node, &rv)) {
            // A bike at the end of a oneway: the only way back is against it.
            const adv_seg *s = &g->segs[prev_seg];
            if (!(s->access & fake_act_access(act))) return false;
            rv = s->node_b == node;
        }
        out->seg = prev_seg;
        out->reversed = rv;
        return true;
    }
    double pick = fake_rng_unit(r) * sum;
    int i = 0;
    for (; i < n - 1; i++) {
        pick -= w[i];
        if (pick < 0) break;
    }
    out->seg = cand[i];
    out->reversed = rev[i];
    return true;
}

static void route_truth(const adv_graph *g, fake_route_out *o) {
    for (uint32_t i = 0; i < o->n; i++) {
        double start = i == 0 ? o->f0 : 0.0, end = i == o->n - 1 ? o->f1 : 1.0;
        uint16_t mask;
        if (start <= 0.0 && end >= 1.0) mask = 0xFFFF;
        else {
            // Fractions are in the travelled direction; the mask's are node_a to node_b.
            double a = o->leg[i].reversed ? 1.0 - end : start, b = o->leg[i].reversed ? 1.0 - start : end;
            mask = adv_mask_of_run(a, b);
        }
        o->truth[i].seg = o->leg[i].seg;
        o->truth[i].mask = mask;
        o->truth[i].pad = 0;
    }
    // adv_truth and adv_hit are the same bytes (trace.h, coverage.h), so the hits' finisher sorts,
    // merges the repeats (a loop that crosses itself) and drops nothing that matters.
    o->n_truth = adv_hits_finish((adv_hit *)o->truth, o->n);
    (void)g;
}
_Static_assert(sizeof(adv_truth) == sizeof(adv_hit), "truth is a hit");
_Static_assert(offsetof(adv_truth, seg) == offsetof(adv_hit, seg) && offsetof(adv_truth, mask) == offsetof(adv_hit, mask), "truth is a hit");

void fake_route(const adv_graph *g, fake_user *u, fake_rng *r, uint8_t act, double target_m, fake_route_out *out) {
    memset(out, 0, offsetof(fake_route_out, truth));
    out->stuck = out->capped = false;
    out->f0 = fake_rng_unit(r) * 0.5;
    out->f1 = 0.5 + fake_rng_unit(r) * 0.5;
    if (g->h->n_nodes == 0 || u->home_node >= g->h->n_nodes) return;
    adv_frame f = adv_frame_at(g->nodes[u->home_node].lat);
    uint32_t node = u->home_node, prev = FAKE_NO_SEG;
    double acc = 0;
    while (out->n < FAKE_ROUTE_MAX_SEGS) {
        // Head home once home is about as far as the budget left (a loop that returns, not one
        // that races back from half way); a loop that closes past 80% of the target is done.
        double remaining = target_m - acc;
        bool homeward = acc >= target_m * 0.5 && node_dist_m(g, &f, node, u->home_node) >= 0.7 * remaining;
        if (acc >= target_m * 0.8 && node == u->home_node && out->n > 0) break;
        fake_leg leg;
        if (!choose_leg(g, u, r, act, node, prev, homeward, &f, &leg)) {
            out->stuck = true;
            break;
        }
        const adv_seg *s = &g->segs[leg.seg];
        acc += out->n == 0 ? (1.0 - out->f0) * s->len_m : s->len_m;
        out->leg[out->n++] = leg;
        node = leg.reversed ? s->node_a : s->node_b;
        prev = leg.seg;
        if (acc >= target_m) break;
    }
    if (out->n == FAKE_ROUTE_MAX_SEGS) out->capped = true;
    if (out->n == 0) return;
    const adv_seg *last = &g->segs[out->leg[out->n - 1].seg];
    if (out->n == 1) acc = (out->f1 - out->f0) * last->len_m;
    else acc -= (1.0 - out->f1) * last->len_m;
    out->len_m = acc;
    route_truth(g, out);
    for (uint32_t i = 0; i < out->n; i++) visited_add(u, out->leg[i].seg);
}

double fake_route_len_m(const adv_graph *g, const fake_route_out *route) {
    double t = 0;
    for (uint32_t i = 0; i < route->n; i++) {
        double start = i == 0 ? route->f0 : 0.0, end = i == route->n - 1 ? route->f1 : 1.0;
        t += (end - start) * g->segs[route->leg[i].seg].len_m;
    }
    return t;
}

// ---- the sampler ----------------------------------------------------------------------------
void fake_noise_default(fake_noise *n) {
    n->sigma_m = 4.0;
    n->rho = 0.92;
    n->dropout_p = 0.3;
    n->spike_p = 1.0 / 400;
}

// A cursor along the route's polyline, one leg's shape in memory at a time (a route is up to
// 2048 legs of 256 points; the whole polyline would be megabytes of scratch for nothing).
typedef struct {
    const adv_graph *g;
    const fake_route_out *r;
    adv_frame f;
    uint32_t leg;
    adv_pt pts[ADV_SEG_MAX_PTS];
    int n;              // points of this leg, in the travelled order
    int k;              // the sub-segment pts[k]..pts[k+1] we are on
    double off;         // metres into it
    double sub_len;     // its length
    double along;       // metres into the leg
    double leg_len;     // the leg's polyline length in this frame
    double end_along;   // where travel stops on the last leg
    bool done;
} cursor;

static double cur_sub_len(const cursor *c, int k) { return adv_dist_m(&c->f, c->pts[k], c->pts[k + 1]); }

static void cur_load(cursor *c) {
    c->n = adv_seg_points(c->g, c->r->leg[c->leg].seg, c->pts, ADV_SEG_MAX_PTS);
    if (c->n < 2) { c->done = true; return; }   // a damaged file; the opener refuses one, so this is belt and braces
    if (c->r->leg[c->leg].reversed)
        for (int i = 0, j = c->n - 1; i < j; i++, j--) {
            adv_pt t = c->pts[i];
            c->pts[i] = c->pts[j];
            c->pts[j] = t;
        }
    c->leg_len = 0;
    for (int i = 0; i + 1 < c->n; i++) c->leg_len += cur_sub_len(c, i);
    c->k = 0;
    c->off = 0;
    c->along = 0;
    c->sub_len = cur_sub_len(c, 0);
    c->end_along = c->leg == c->r->n - 1 ? c->r->f1 * c->leg_len : c->leg_len;
}

static void cur_init(cursor *c, const adv_graph *g, const fake_route_out *r) {
    memset(c, 0, sizeof *c);
    c->g = g;
    c->r = r;
    c->done = r->n == 0;
    if (c->done) return;
    // One frame for the whole activity, at its first segment: a 40 km ride spans a third of a
    // degree, where cos(lat) moves by half a percent, which is under the GPS noise.
    c->f = adv_frame_at(g->segs[r->leg[0].seg].lat);
    cur_load(c);
}

// Forward d metres; the loop is bounded by the sub-segments left (the file's caps), never by d.
static void cur_advance(cursor *c, double d) {
    uint32_t guard = (uint32_t)FAKE_ROUTE_MAX_SEGS * ADV_SEG_MAX_PTS;
    while (!c->done && d > 0 && guard-- > 0) {
        double rem = c->sub_len - c->off;
        if (c->leg == c->r->n - 1 && c->along + d >= c->end_along) { c->done = true; return; }
        if (d < rem) {
            c->off += d;
            c->along += d;
            return;
        }
        d -= rem;
        c->along += rem;
        c->k++;
        if (c->k + 1 >= c->n) {
            c->leg++;
            if (c->leg >= c->r->n) { c->done = true; return; }
            cur_load(c);
        } else {
            c->off = 0;
            c->sub_len = cur_sub_len(c, c->k);
        }
    }
}

static adv_pt cur_pos(const cursor *c) {
    adv_pt a = c->pts[c->k], b = c->pts[c->k + 1];
    double t = c->sub_len > 0 ? c->off / c->sub_len : 0;
    adv_pt p;
    p.lat = (int32_t)(a.lat + t * ((double)b.lat - a.lat));
    p.lon = (int32_t)(a.lon + t * ((double)b.lon - a.lon));
    return p;
}

static double speed_mean(fake_rng *r, uint8_t act) {
    switch (act) {
    case ADV_ACT_RUN: return fake_rng_range(r, 2.4, 3.6);
    case ADV_ACT_WALK: return fake_rng_range(r, 1.1, 1.6);
    default: return fake_rng_range(r, 4.0, 8.0);
    }
}

static int32_t clamp_i32(double v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : (int32_t)v; }

uint32_t fake_sample(const adv_graph *g, const fake_route_out *route, uint8_t act, fake_rng *r,
                     const fake_noise *noise, adv_trace_pt *out, uint32_t cap) {
    cursor c;
    cur_init(&c, g, route);
    if (c.done || cap == 0) return 0;
    cur_advance(&c, route->f0 * c.leg_len);
    double v_mean = speed_mean(r, act);
    double rho = noise->rho, innov = noise->sigma_m * sqrt(1.0 - rho * rho);
    double ex = fake_rng_normal(r) * noise->sigma_m, ey = fake_rng_normal(r) * noise->sigma_m;   // stationary start
    // One dropout at most, placed by the expected duration; it may fall past the end (then none).
    bool dropout = fake_rng_unit(r) < noise->dropout_p;
    double expect_s = route->len_m / v_mean;
    uint32_t drop_at = (uint32_t)(fake_rng_unit(r) * expect_s), drop_len = 10 + fake_rng_below(r, 111);
    uint32_t n = 0, t_prev = 0;
    uint64_t sec = 0;
    // A second per point and at most cap points, plus one dropout: the loop ends by count.
    for (uint32_t i = 0; i < cap && !c.done; i++) {
        adv_pt pos = cur_pos(&c);
        ex = rho * ex + innov * fake_rng_normal(r);
        ey = rho * ey + innov * fake_rng_normal(r);
        double dlat = ey / c.f.my, dlon = ex / c.f.mx;
        if (fake_rng_unit(r) < noise->spike_p) {
            double ang = fake_rng_unit(r) * 2 * M_PI, dist = fake_rng_range(r, 50, 500);
            dlat += dist * sin(ang) / c.f.my;
            dlon += dist * cos(ang) / c.f.mx;
        }
        adv_trace_pt p;
        p.lat = clamp_i32(pos.lat + dlat, -ADV_LAT_MAX, ADV_LAT_MAX);
        p.lon = clamp_i32(pos.lon + dlon, -ADV_LON_MAX, ADV_LON_MAX);
        int64_t t = (int64_t)sec * 1000 + (int64_t)fake_rng_below(r, 401) - 200;
        if (t < t_prev) t = t_prev;
        if (t > UINT32_MAX) t = UINT32_MAX;
        p.t_ms = (uint32_t)t;
        t_prev = p.t_ms;
        out[n++] = p;
        cur_advance(&c, v_mean * (1.0 + 0.15 * (2.0 * fake_rng_unit(r) - 1.0)));
        sec++;
        if (dropout && sec >= drop_at) {
            // The phone kept moving while it had no fix: time jumps, the error drifts on.
            dropout = false;
            for (uint32_t s = 0; s < drop_len && !c.done; s++) {
                cur_advance(&c, v_mean * (1.0 + 0.15 * (2.0 * fake_rng_unit(r) - 1.0)));
                ex = rho * ex + innov * fake_rng_normal(r);
                ey = rho * ey + innov * fake_rng_normal(r);
                sec++;
            }
        }
    }
    return n;
}

// ---- an activity ----------------------------------------------------------------------------
static bool good_activity(const adv_graph *g, fake_user *u, fake_rng *r, int64_t start_ms, uint8_t flags,
                          const fake_noise *noise, fake_route_out *route, fake_act *a) {
    uint8_t act = fake_kind_act(u->def.kind);
    fake_route(g, u, r, act, fake_target_m(r, act), route);
    memset(&a->hdr, 0, sizeof a->hdr);
    a->n_pts = 0;
    a->n_truth = 0;
    a->route_m = route->len_m;
    a->truncate = false;
    a->hostile = 0;
    if (route->n == 0) return false;
    fake_noise nz;
    if (noise) nz = *noise;
    else {
        fake_noise_default(&nz);
        nz.sigma_m = fake_rng_range(r, 2.0, 8.0);   // a phone's day: some clear, some under trees
    }
    a->n_pts = fake_sample(g, route, act, r, &nz, a->pts, ADV_TRACE_MAX_PTS);
    a->n_truth = route->n_truth;
    memcpy(a->truth, route->truth, (size_t)route->n_truth * sizeof a->truth[0]);
    a->hdr.magic = ADV_TRACE_MAGIC;
    a->hdr.version = ADV_TRACE_VERSION;
    a->hdr.type = act;
    a->hdr.flags = (uint8_t)(flags & ADV_TRF_BACKFILL);
    a->hdr.user = u->def.id;
    a->hdr.n_points = a->n_pts;
    a->hdr.start_ms = start_ms;
    a->hdr.gen_id = ++u->n_activities;
    return true;
}

bool fake_activity(const adv_graph *g, fake_user *u, fake_rng *r, int64_t start_ms, uint8_t flags,
                   const fake_noise *noise, fake_route_out *route, fake_act *a) {
    return good_activity(g, u, r, start_ms, flags, noise, route, a);
}

size_t fake_act_write(const fake_act *a, uint8_t *out, size_t cap) {
    return adv_trace_write(out, cap, &a->hdr, a->pts, a->n_truth ? a->truth : NULL, a->n_truth);
}

// ---- the hostile makers --------------------------------------------------------------------
const fake_hostile_kind FAKE_HOSTILE[FAKE_HOSTILE_N + 1] = {
    { "-", false, false },
    { "zero points", true, false },
    { "one point", true, false },
    { "2000 points at one coordinate", true, false },
    { "65535 points at t=0 at one coordinate", true, false },
    { "65535 points random across the planet", true, false },
    { "time runs backwards after the middle", true, false },
    { "points with lat 2e9", true, false },
    { "a 20 km teleport in the middle", true, false },
    { "entirely outside the graph (5 degrees north)", true, false },
    { "body truncated by 7 bytes", true, true },
    { "truth trailer out of order", false, false },
    { "very sparse, one point per 10 s", true, false },
};

// A header for a hostile that is not built on a route.
static void bare_hdr(fake_user *u, int64_t start_ms, uint32_t n_pts, fake_act *a) {
    memset(&a->hdr, 0, sizeof a->hdr);
    a->hdr.magic = ADV_TRACE_MAGIC;
    a->hdr.version = ADV_TRACE_VERSION;
    a->hdr.type = fake_kind_act(u->def.kind);
    a->hdr.user = u->def.id;
    a->hdr.n_points = n_pts;
    a->hdr.start_ms = start_ms;
    a->hdr.gen_id = ++u->n_activities;
    a->n_pts = n_pts;
    a->n_truth = 0;
    a->route_m = 0;
    a->truncate = false;
}

bool fake_hostile(const adv_graph *g, int kind, fake_user *u, fake_rng *r, int64_t start_ms,
                  fake_route_out *route, fake_act *a) {
    if (kind < 1 || kind > FAKE_HOSTILE_N) return false;
    adv_pt home = { u->def.home_lat, u->def.home_lon };
    switch (kind) {
    case 1: bare_hdr(u, start_ms, 0, a); break;
    case 2:
        bare_hdr(u, start_ms, 1, a);
        a->pts[0].lat = home.lat;
        a->pts[0].lon = home.lon;
        a->pts[0].t_ms = 0;
        break;
    case 3:
        bare_hdr(u, start_ms, 2000, a);
        for (uint32_t i = 0; i < 2000; i++) { a->pts[i].lat = home.lat; a->pts[i].lon = home.lon; a->pts[i].t_ms = i * 1000; }
        break;
    case 4:
        bare_hdr(u, start_ms, ADV_TRACE_MAX_PTS, a);
        for (uint32_t i = 0; i < ADV_TRACE_MAX_PTS; i++) { a->pts[i].lat = home.lat; a->pts[i].lon = home.lon; a->pts[i].t_ms = 0; }
        break;
    case 5:
        bare_hdr(u, start_ms, ADV_TRACE_MAX_PTS, a);
        for (uint32_t i = 0; i < ADV_TRACE_MAX_PTS; i++) {
            a->pts[i].lat = (int32_t)fake_rng_range(r, -ADV_LAT_MAX, ADV_LAT_MAX);
            a->pts[i].lon = (int32_t)fake_rng_range(r, -ADV_LON_MAX, ADV_LON_MAX);
            a->pts[i].t_ms = i * 1000;
        }
        break;
    default: {
        if (!good_activity(g, u, r, start_ms, 0, NULL, route, a)) return false;
        uint32_t n = a->n_pts, mid = n / 2;
        adv_frame f = adv_frame_at(home.lat);
        switch (kind) {
        case 6:   // the clock runs backwards from the middle on (saturating at 0)
            for (uint32_t i = mid; i < n; i++) {
                int64_t t = (int64_t)a->pts[mid > 0 ? mid - 1 : 0].t_ms - (int64_t)(i - mid + 1) * 1000;
                a->pts[i].t_ms = t < 0 ? 0 : (uint32_t)t;
            }
            break;
        case 7:
            for (uint32_t i = 0; i < n; i += 10) a->pts[i].lat = 2000000000;
            break;
        case 8: {
            int32_t dlat = (int32_t)(20000.0 / f.my);
            for (uint32_t i = mid; i < n; i++) a->pts[i].lat = clamp_i32((double)a->pts[i].lat + dlat, -ADV_LAT_MAX, ADV_LAT_MAX);
            break;
        }
        case 9:
            for (uint32_t i = 0; i < n; i++) a->pts[i].lat = clamp_i32((double)a->pts[i].lat + 5.0 * ADV_DEG, -ADV_LAT_MAX, ADV_LAT_MAX);
            break;
        case 10: a->truncate = true; break;
        case 11:
            if (a->n_truth >= 2) {
                adv_truth t = a->truth[0];
                a->truth[0] = a->truth[1];
                a->truth[1] = t;
            } else {
                // Too short a route for a swap: a repeat is out of order too (seg <= prev).
                a->truth[1] = a->truth[0];
                a->n_truth = 2;
            }
            break;
        case 12: {
            uint32_t w = 0;
            for (uint32_t i = 0; i < n; i += 10) a->pts[w++] = a->pts[i];
            a->n_pts = w;
            a->hdr.n_points = w;
            break;
        }
        default: break;
        }
        break;
    }
    }
    a->hostile = kind;
    return true;
}
