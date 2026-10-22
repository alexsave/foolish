// unit_gen.c - the fake world (fake.h) on a synthetic grid: the roster is deterministic and
// well-formed, routes are connected walks that respect access and reach their target, the truth
// matches the route, the sampler stays on the road at one point per second (dropouts and spikes
// where asked), the whole round-trips through the trace codec, and every hostile kind builds and
// parses (or refuses) as its table says.
#include "coverage.h"
#include "fake.h"
#include "lib_fakegraph.h"
#include "t.h"
#include "trace.h"
#include "util/err.h"

#include <math.h>
#include <stdlib.h>
#include <unistd.h>

static const fg_opts GRID = { .cols = 12, .rows = 12, .regions = true, .foot_only_rows = true };
static fake_world W;   // the world of the graph under test (main)

// The node a leg leaves from and arrives at.
static uint32_t leg_from(const adv_graph *g, const fake_leg *l) { return l->reversed ? g->segs[l->seg].node_b : g->segs[l->seg].node_a; }
static uint32_t leg_to(const adv_graph *g, const fake_leg *l) { return l->reversed ? g->segs[l->seg].node_a : g->segs[l->seg].node_b; }

static void test_users(const adv_graph *g) {
    enum { N = 2000 };
    adv_w_user_def *a = calloc(N, sizeof *a), *b = calloc(N, sizeof *b);
    fake_users_make(&W, N, 7, a);
    fake_users_make(&W, N, 7, b);
    T_CHECK(memcmp(a, b, N * sizeof *a) == 0);   // deterministic: a restart posts the same roster
    fake_users_make(&W, N, 8, b);
    T_CHECK(memcmp(a, b, N * sizeof *a) != 0);   // and the seed matters
    uint32_t kinds[8] = { 0 };
    bool bad = false;
    for (uint32_t i = 0; i < N; i++) {
        if (a[i].id != i + 1) bad = true;
        if (a[i].kind < ADV_UK_COMMUTER || a[i].kind > ADV_UK_TOURIST) bad = true;
        else kinds[a[i].kind]++;
        if (a[i].name_len == 0 || a[i].name_len > ADV_W_NAME - 1) bad = true;
        if (a[i].name[a[i].name_len] != 0 || strlen(a[i].name) != a[i].name_len) bad = true;
        if (!strchr(a[i].name, ' ')) bad = true;
        if (a[i].pad != 0) bad = true;
        // Home is a node of the grid.
        bool on_node = false;
        for (uint32_t k = 0; k < g->h->n_nodes && !on_node; k++)
            if (g->nodes[k].lat == a[i].home_lat && g->nodes[k].lon == a[i].home_lon) on_node = true;
        if (!on_node) bad = true;
    }
    T_CHECK(!bad);
    // No population tag: every region is a suburb (30% commuters, 35% explorers, 10% tourists).
    T_CHECK_MSG(kinds[ADV_UK_COMMUTER] > 0.25 * N && kinds[ADV_UK_COMMUTER] < 0.35 * N, "commuters %u", kinds[ADV_UK_COMMUTER]);
    T_CHECK_MSG(kinds[ADV_UK_EXPLORER] > 0.30 * N && kinds[ADV_UK_EXPLORER] < 0.40 * N, "explorers %u", kinds[ADV_UK_EXPLORER]);
    T_CHECK_MSG(kinds[ADV_UK_TOURIST] > 0.06 * N && kinds[ADV_UK_TOURIST] < 0.14 * N, "tourists %u", kinds[ADV_UK_TOURIST]);
    // fake_user_init agrees with the roster and finds the home node.
    fake_user *u = malloc(sizeof *u);
    fake_user_init(&W, u, 7, 17);
    T_CHECK(memcmp(&u->def, &a[16], sizeof u->def) == 0);
    T_CHECK(u->home_node < g->h->n_nodes && g->nodes[u->home_node].lat == u->def.home_lat && g->nodes[u->home_node].lon == u->def.home_lon);
    T_CHECK(u->n_visited == 0 && !fake_user_visited(u, 3));
    free(u);
    free(a);
    free(b);
}

// Fraction of route b's legs whose segment is in route a.
static double reuse(const fake_route_out *a, const fake_route_out *b) {
    if (b->n == 0) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < b->n; i++) {
        bool in = false;
        for (uint32_t j = 0; j < a->n && !in; j++) in = a->leg[j].seg == b->leg[i].seg;
        n += in;
    }
    return (double)n / b->n;
}

static void test_routes(const adv_graph *g) {
    fake_user *u = malloc(sizeof *u);
    fake_route_out *ro = malloc(sizeof *ro), *ro2 = malloc(sizeof *ro2);
    fake_rng r;
    fake_rng_seed(&r, 3);
    uint32_t foot_routes_on_footpath = 0, foot_routes = 0, bike_routes = 0, checked_no_repeat = 0;
    // 150 users: the road pull (a run seeks the footpaths) makes a loop on this small grid repeat
    // a segment more often, and the mask checks below want routes that do not.
    for (uint32_t id = 1; id <= 150; id++) {
        fake_user_init(&W, u, 11, id);
        uint8_t act = fake_kind_act(u->def.kind);
        // Targets on a 1.4 km grid: real ones (20 km) would circle it; 2-6 km shows every rule.
        double target = fake_rng_range(&r, 2000, 6000);
        fake_route(g, u, &r, act, target, ro);
        T_CHECK(ro->n > 0);
        if (ro->n == 0) continue;
        T_CHECK(leg_from(g, &ro->leg[0]) == u->home_node);
        bool connected = true, access_ok = true, on_footpath = false, repeat = false;
        for (uint32_t i = 0; i < ro->n; i++) {
            const adv_seg *s = &g->segs[ro->leg[i].seg];
            if (i > 0 && leg_to(g, &ro->leg[i - 1]) != leg_from(g, &ro->leg[i])) connected = false;
            if (!(s->access & fake_act_access(act))) access_ok = false;
            if (s->access == ADV_ACC_FOOT) on_footpath = true;
            for (uint32_t j = 0; j < i && !repeat; j++) repeat = ro->leg[j].seg == ro->leg[i].seg;
            if (!fake_user_visited(u, ro->leg[i].seg)) access_ok = false;   // marked visited
        }
        T_CHECK_MSG(connected, "user %u: legs do not chain", id);
        T_CHECK_MSG(access_ok, "user %u (%s): access or visited broken", id, fake_kind_name(u->def.kind));
        if (act == ADV_ACT_BIKE) bike_routes++;
        else { foot_routes++; foot_routes_on_footpath += on_footpath; }
        double len = fake_route_len_m(g, ro);
        T_CHECK_MSG(fabs(len - ro->len_m) < 0.01, "len %f vs %f", len, ro->len_m);
        if (!ro->capped && !ro->stuck)
            T_CHECK_MSG(fabs(len - target) < 0.25 * target, "user %u: len %f target %f (n=%u)", id, len, target, ro->n);
        // The truth: sorted unique, non-empty masks, every segment of the route and no other.
        bool sorted = true;
        for (uint32_t i = 0; i < ro->n_truth; i++) {
            if (i > 0 && ro->truth[i - 1].seg >= ro->truth[i].seg) sorted = false;
            if (ro->truth[i].mask == 0 || ro->truth[i].pad != 0) sorted = false;
        }
        T_CHECK(sorted);
        bool covered = true;
        for (uint32_t i = 0; i < ro->n && covered; i++) {
            bool in = false;
            for (uint32_t j = 0; j < ro->n_truth && !in; j++) in = ro->truth[j].seg == ro->leg[i].seg;
            covered = in;
        }
        T_CHECK(covered);
        T_CHECK(ro->n_truth <= ro->n);
        // Whole legs are whole; the ends are the masks of their runs.
        if (ro->n >= 3 && !repeat) {
            for (uint32_t j = 0; j < ro->n_truth; j++) {
                uint32_t seg = ro->truth[j].seg;
                bool is_end = seg == ro->leg[0].seg || seg == ro->leg[ro->n - 1].seg;
                if (!is_end) T_CHECK_MSG(ro->truth[j].mask == 0xFFFF, "inner seg %u mask %04x", seg, ro->truth[j].mask);
            }
            uint16_t m0 = 0, m1 = 0;
            for (uint32_t j = 0; j < ro->n_truth; j++) {
                if (ro->truth[j].seg == ro->leg[0].seg) m0 = ro->truth[j].mask;
                if (ro->truth[j].seg == ro->leg[ro->n - 1].seg) m1 = ro->truth[j].mask;
            }
            uint16_t want0 = ro->leg[0].reversed ? adv_mask_of_run(0, 1 - ro->f0) : adv_mask_of_run(ro->f0, 1);
            uint16_t want1 = ro->leg[ro->n - 1].reversed ? adv_mask_of_run(1 - ro->f1, 1) : adv_mask_of_run(0, ro->f1);
            T_CHECK_MSG(m0 == want0, "first mask %04x want %04x (f0 %f rev %d)", m0, want0, ro->f0, ro->leg[0].reversed);
            T_CHECK_MSG(m1 == want1, "last mask %04x want %04x (f1 %f rev %d)", m1, want1, ro->f1, ro->leg[ro->n - 1].reversed);
            // And the truth's length is the route's within a sixteenth per segment.
            float tl = adv_hits_len_m(g, (const adv_hit *)ro->truth, ro->n_truth);
            T_CHECK_MSG(fabs(tl - len) <= ro->n * 120.0 / 16 + 0.5, "truth %f route %f n %u", tl, len, ro->n);
            checked_no_repeat++;
        } else {
            // With repeats the masks OR: never more than the route, never less than one visit's worth.
            float tl = adv_hits_len_m(g, (const adv_hit *)ro->truth, ro->n_truth);
            T_CHECK(tl <= len + ro->n * 120.0 / 16 + 0.5);
            T_CHECK(tl > 0);
        }
    }
    T_CHECK_MSG(bike_routes > 10 && foot_routes > 10, "bike %u foot %u", bike_routes, foot_routes);
    T_CHECK_MSG(foot_routes_on_footpath > foot_routes / 2, "footpaths used by %u of %u foot routes", foot_routes_on_footpath, foot_routes);
    T_CHECK_MSG(checked_no_repeat >= 5, "routes without repeats: %u", checked_no_repeat);
    // The kinds: an explorer's second route re-uses less of its first than a commuter's.
    double re_explorer = 0, re_commuter = 0;
    for (uint32_t id = 1; id <= 20; id++) {
        for (int kind = 0; kind < 2; kind++) {
            fake_user_init(&W, u, 5, id);
            u->def.kind = kind ? ADV_UK_COMMUTER : ADV_UK_EXPLORER;
            fake_route(g, u, &r, ADV_ACT_BIKE, 4000, ro);
            fake_route(g, u, &r, ADV_ACT_BIKE, 4000, ro2);
            double re = reuse(ro, ro2);
            if (kind) re_commuter += re / 20;
            else re_explorer += re / 20;
        }
    }
    T_CHECK_MSG(re_commuter > re_explorer + 0.2, "reuse commuter %f explorer %f", re_commuter, re_explorer);
    // A target longer than the grid can give in 2048 legs is capped, never a hang.
    fake_user_init(&W, u, 5, 1);
    u->def.kind = ADV_UK_TOURIST;
    fake_route(g, u, &r, ADV_ACT_BIKE, 1e9, ro);
    T_CHECK(ro->capped && ro->n == FAKE_ROUTE_MAX_SEGS);
    T_CHECK(fake_route_len_m(g, ro) > 0);
    free(u);
    free(ro);
    free(ro2);
}

// Distance from p to the route's polyline (every leg's shape), in f.
static double dist_to_route(const adv_graph *g, const adv_frame *f, const fake_route_out *ro, adv_pt p) {
    double best = 1e18;
    adv_pt pts[ADV_SEG_MAX_PTS];
    for (uint32_t i = 0; i < ro->n; i++) {
        int n = adv_seg_points(g, ro->leg[i].seg, pts, ADV_SEG_MAX_PTS);
        for (int k = 0; k + 1 < n; k++) {
            double t, d = adv_point_seg_m(f, p, pts[k], pts[k + 1], &t);
            if (d < best) best = d;
        }
    }
    return best;
}

static void test_sampler(const adv_graph *g) {
    fake_user *u = malloc(sizeof *u);
    fake_route_out *ro = malloc(sizeof *ro);
    adv_trace_pt *pts = malloc(sizeof *pts * ADV_TRACE_MAX_PTS);
    fake_rng r;
    fake_rng_seed(&r, 9);
    fake_user_init(&W, u, 5, 2);
    u->def.kind = ADV_UK_TOURIST;
    adv_frame f = adv_frame_at(u->def.home_lat);
    fake_noise quiet;
    fake_noise_default(&quiet);
    quiet.sigma_m = 1.0;
    quiet.dropout_p = 0;
    quiet.spike_p = 0;
    for (int rep = 0; rep < 5; rep++) {
        fake_route(g, u, &r, ADV_ACT_BIKE, 5000, ro);
        uint32_t n = fake_sample(g, ro, ADV_ACT_BIKE, &r, &quiet, pts, ADV_TRACE_MAX_PTS);
        T_CHECK_MSG(n > 500 && n < 1400, "n=%u for %f m", n, ro->len_m);   // 5 km at 4-8 m/s
        double poly = 0, worst = 0;
        bool spacing = true, mono = true;
        for (uint32_t i = 0; i < n; i++) {
            adv_pt p = { pts[i].lat, pts[i].lon };
            if (i > 0) {
                adv_pt q = { pts[i - 1].lat, pts[i - 1].lon };
                poly += adv_dist_m(&f, q, p);
                int64_t dt = (int64_t)pts[i].t_ms - pts[i - 1].t_ms;
                if (dt < 0) mono = false;
                if (dt < 600 || dt > 1400) spacing = false;
            }
            double d = dist_to_route(g, &f, ro, p);
            if (d > worst) worst = d;
        }
        T_CHECK(mono);
        T_CHECK_MSG(spacing, "points not 1 s apart");
        T_CHECK_MSG(fabs(poly - ro->len_m) < 0.15 * ro->len_m, "polyline %f route %f", poly, ro->len_m);
        T_CHECK_MSG(worst < 10, "worst off-road %f m at sigma 1", worst);
        // The samples start where the route starts (f0 into the first leg) and end at f1.
        const fake_leg *l0 = &ro->leg[0], *l1 = &ro->leg[ro->n - 1];
        adv_pt start = adv_seg_at(g, l0->seg, l0->reversed ? 1 - ro->f0 : ro->f0);
        adv_pt end = adv_seg_at(g, l1->seg, l1->reversed ? 1 - ro->f1 : ro->f1);
        adv_pt p0 = { pts[0].lat, pts[0].lon }, pn = { pts[n - 1].lat, pts[n - 1].lon };
        T_CHECK_MSG(adv_dist_m(&f, p0, start) < 12, "first point %f m from the route's start", adv_dist_m(&f, p0, start));
        T_CHECK_MSG(adv_dist_m(&f, pn, end) < 12 + 10, "last point %f m from the route's end", adv_dist_m(&f, pn, end));
        T_CHECK(pts[n - 1].t_ms >= (n - 1) * 1000 - 200 * (n - 1) / 1000 - 400);   // one second per point, about
    }
    // Default noise without spikes or dropouts: within 60 m of the road, every point.
    fake_noise nz;
    fake_noise_default(&nz);
    nz.dropout_p = 0;
    nz.spike_p = 0;
    fake_route(g, u, &r, ADV_ACT_BIKE, 5000, ro);
    uint32_t n = fake_sample(g, ro, ADV_ACT_BIKE, &r, &nz, pts, ADV_TRACE_MAX_PTS);
    uint32_t far = 0;
    double rms = 0;
    for (uint32_t i = 0; i < n; i++) {
        adv_pt p = { pts[i].lat, pts[i].lon };
        double d = dist_to_route(g, &f, ro, p);
        if (d > 60) far++;
        rms += d * d / n;
    }
    T_CHECK_MSG(far == 0, "%u points over 60 m with sigma 4", far);
    rms = sqrt(rms);
    T_CHECK_MSG(rms > 1.5 && rms < 8, "rms off-road %f at sigma 4", rms);   // noise is there, and about 4 m
    // Spikes: some points far off; dropout: one gap over 10 s.
    nz.spike_p = 0.05;
    nz.dropout_p = 1.0;
    fake_route(g, u, &r, ADV_ACT_BIKE, 5000, ro);
    n = fake_sample(g, ro, ADV_ACT_BIKE, &r, &nz, pts, ADV_TRACE_MAX_PTS);
    far = 0;
    uint32_t gaps = 0;
    for (uint32_t i = 0; i < n; i++) {
        adv_pt p = { pts[i].lat, pts[i].lon };
        if (dist_to_route(g, &f, ro, p) > 40) far++;
        if (i > 0 && pts[i].t_ms - pts[i - 1].t_ms > 10000) gaps++;
    }
    T_CHECK_MSG(far > n / 100 && far < n / 10, "spikes %u of %u", far, n);
    T_CHECK_MSG(gaps == 1, "gaps %u", gaps);
    // The cap cuts the activity.
    T_CHECK(fake_sample(g, ro, ADV_ACT_BIKE, &r, &nz, pts, 100) == 100);
    T_CHECK(fake_sample(g, ro, ADV_ACT_BIKE, &r, &nz, pts, 0) == 0);
    // Walking and running speeds.
    fake_route(g, u, &r, ADV_ACT_WALK, 3000, ro);
    n = fake_sample(g, ro, ADV_ACT_WALK, &r, &quiet, pts, ADV_TRACE_MAX_PTS);
    T_CHECK_MSG(n > 3000 / 1.7 && n < 3000 / 1.0 * 1.3, "walk n=%u", n);
    // The whole activity round-trips through the codec with its truth.
    fake_act *a = malloc(sizeof *a);
    uint8_t *body = malloc(FAKE_BODY_CAP);
    fake_user_init(&W, u, 5, 3);
    T_CHECK(fake_activity(g, u, &r, 1700000000000LL, ADV_TRF_BACKFILL, NULL, ro, a));
    T_CHECK(a->hdr.user == 3 && a->hdr.gen_id == 1 && a->hdr.n_points == a->n_pts && a->n_truth == ro->n_truth && a->n_truth > 0);
    T_CHECK(a->hdr.type == fake_kind_act(u->def.kind));
    size_t len = fake_act_write(a, body, FAKE_BODY_CAP);
    T_CHECK(len == adv_trace_size(a->n_pts, a->n_truth) && len > 0);
    T_CHECK(fake_act_wire_len(a, len) == len);
    adv_trace t;
    T_CHECK(adv_trace_parse(body, len, &t) == CB_OK);
    T_CHECK(t.hdr.user == 3 && t.hdr.n_points == a->n_pts && t.n_truth == a->n_truth && (t.hdr.flags & ADV_TRF_TRUTH) && (t.hdr.flags & ADV_TRF_BACKFILL));
    T_CHECK(t.hdr.start_ms == 1700000000000LL && t.hdr.gen_id == 1);
    bool same = true;
    for (uint32_t i = 0; i < t.hdr.n_points && same; i++) {
        adv_trace_pt p = adv_trace_pt_at(&t, i);
        same = p.lat == a->pts[i].lat && p.lon == a->pts[i].lon && p.t_ms == a->pts[i].t_ms;
    }
    for (uint32_t i = 0; i < t.n_truth && same; i++) {
        adv_truth tr = adv_trace_truth_at(&t, i);
        same = tr.seg == a->truth[i].seg && tr.mask == a->truth[i].mask;
    }
    T_CHECK(same);
    T_CHECK(fake_activity(g, u, &r, 1700000000000LL, 0, NULL, ro, a) && a->hdr.gen_id == 2 && u->n_activities == 2);
    free(a);
    free(body);
    free(u);
    free(ro);
    free(pts);
}

static void test_hostile(const adv_graph *g) {
    fake_user *u = malloc(sizeof *u);
    fake_route_out *ro = malloc(sizeof *ro);
    fake_act *a = malloc(sizeof *a);
    uint8_t *body = malloc(FAKE_BODY_CAP);
    fake_rng r;
    fake_rng_seed(&r, 21);
    fake_user_init(&W, u, 5, 4);
    for (int kind = 1; kind <= FAKE_HOSTILE_N; kind++) {
        T_CHECK_MSG(fake_hostile(g, kind, u, &r, 1700000000000LL, ro, a), "kind %d builds", kind);
        T_CHECK(a->hostile == kind && a->truncate == FAKE_HOSTILE[kind].truncate && FAKE_HOSTILE[kind].name[0]);
        size_t len = fake_act_write(a, body, FAKE_BODY_CAP);
        T_CHECK_MSG(len > 0, "kind %d writes", kind);
        adv_trace t;
        int rc = adv_trace_parse(body, len, &t);
        T_CHECK_MSG((rc == CB_OK) == FAKE_HOSTILE[kind].parses, "kind %d (%s): parse %d", kind, FAKE_HOSTILE[kind].name, rc);
        if (a->truncate) {
            size_t wl = fake_act_wire_len(a, len);
            T_CHECK(wl == len - FAKE_TRUNCATE_BYTES && adv_trace_parse(body, wl, &t) == CB_E_INVALID);
        }
        uint32_t n = a->n_pts;
        adv_frame f = adv_frame_at(u->def.home_lat);
        switch (kind) {
        case 1: T_CHECK(n == 0); break;
        case 2: T_CHECK(n == 1); break;
        case 3: T_CHECK(n == 2000 && a->pts[1999].lat == a->pts[0].lat && a->pts[1999].t_ms == 1999000); break;
        case 4: T_CHECK(n == ADV_TRACE_MAX_PTS && a->pts[n - 1].t_ms == 0 && a->pts[n - 1].lon == a->pts[0].lon); break;
        case 5: {
            T_CHECK(n == ADV_TRACE_MAX_PTS);
            adv_pt p0 = { a->pts[0].lat, a->pts[0].lon }, p1 = { a->pts[1].lat, a->pts[1].lon };
            T_CHECK(adv_pt_valid(p0) && adv_pt_valid(p1) && (abs(p0.lat - p1.lat) > ADV_DEG || abs(p0.lon - p1.lon) > ADV_DEG));
            break;
        }
        case 6: {
            bool back = false, fwd = true;
            for (uint32_t i = 1; i < n; i++) {
                if (a->pts[i].t_ms < a->pts[i - 1].t_ms) back = true;
                if (i < n / 2 && a->pts[i].t_ms < a->pts[i - 1].t_ms) fwd = false;
            }
            T_CHECK(back && fwd && n > 100);
            break;
        }
        case 7: {
            uint32_t invalid = 0;
            for (uint32_t i = 0; i < n; i++) { adv_pt p = { a->pts[i].lat, a->pts[i].lon }; invalid += !adv_pt_valid(p); }
            T_CHECK(invalid >= n / 10 && invalid <= n / 10 + 1);
            break;
        }
        case 8: {
            double jump = 0;
            for (uint32_t i = 1; i < n; i++) {
                adv_pt p = { a->pts[i].lat, a->pts[i].lon }, q = { a->pts[i - 1].lat, a->pts[i - 1].lon };
                double d = adv_dist_m(&f, p, q);
                if (d > jump) jump = d;
            }
            T_CHECK_MSG(jump > 19000 && jump < 21000, "teleport %f", jump);
            break;
        }
        case 9: {
            bool outside = true;
            for (uint32_t i = 0; i < n; i++)
                if (a->pts[i].lat <= g->h->max_lat) outside = false;
            T_CHECK(outside && n > 100 && a->n_truth > 0);
            break;
        }
        case 10: T_CHECK(n > 100 && a->n_truth > 0); break;
        case 11: T_CHECK(a->n_truth >= 2 && a->truth[0].seg >= a->truth[1].seg); break;
        case 12: {
            bool sparse = n > 50;
            for (uint32_t i = 1; i < n; i++) {
                uint32_t dt = a->pts[i].t_ms - a->pts[i - 1].t_ms;
                if (dt < 9000) sparse = false;
            }
            T_CHECK_MSG(sparse, "sparse n=%u", n);
            break;
        }
        default: break;
        }
    }
    T_CHECK(!fake_hostile(g, 0, u, &r, 0, ro, a) && !fake_hostile(g, FAKE_HOSTILE_N + 1, u, &r, 0, ro, a));
    // The writer refuses more than the caps.
    adv_trace_hdr h = { 0 };
    h.type = ADV_ACT_BIKE;
    h.user = 1;
    h.n_points = ADV_TRACE_MAX_PTS + 1;
    T_CHECK(adv_trace_write(body, FAKE_BODY_CAP, &h, a->pts, NULL, 0) == 0);
    h.n_points = 1;
    T_CHECK(adv_trace_write(body, FAKE_BODY_CAP, &h, a->pts, a->truth, ADV_TRACE_MAX_TRUTH + 1) == 0);
    T_CHECK(adv_trace_write(body, FAKE_BODY_CAP, &h, a->pts, a->truth, 1) == adv_trace_size(1, 1));
    T_CHECK(adv_trace_size(ADV_TRACE_MAX_PTS, ADV_TRACE_MAX_TRUTH) == FAKE_BODY_CAP);
    free(u);
    free(ro);
    free(a);
    free(body);
}

// THE WORLD: homes follow population. On the grid's two near-equal halves (about 10 km of
// streets lived on each), West tagged with 90,000 people and East with 3,000: a region's share of
// homes is its share of the people, 90/93; West's density puts it in the city (urbanity 1, more
// runners and walkers), East's in the suburbs (0). Untagged, the halves split
// evenly. And an activity starts at an attractor (a footpath of the grid) as often as fake.c
// says for a city runner.
static void test_world(void) {
    static const fg_opts POP = { .cols = 12, .rows = 12, .regions = true, .foot_only_rows = true, .pop_west = 90000, .pop_east = 3000 };
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_gen_pop");
    adv_graph g;
    T_CHECK(fg_build(&POP, path, &g) == 0);
    fake_world w;
    T_CHECK((fake_world_init(&w, &g)) == (CB_OK));
    T_CHECK((w.n_tagged) == (2));
    int west = g.regions[0].name[0] == 'W' ? 0 : 1, east = 1 - west;
    T_CHECK_MSG(w.region_density[west] > FAKE_DENSITY_CITY && w.region_density[east] < FAKE_DENSITY_SUBURB, "densities %f %f",
                w.region_density[west], w.region_density[east]);
    T_CHECK(w.region_urban[west] == 1.0f && w.region_urban[east] == 0.0f);
    enum { N = 4000 };
    uint32_t in_west = 0, foot_west = 0, foot_east = 0, n_east = 0;
    for (uint32_t id = 1; id <= N; id++) {
        adv_w_user_def d;
        uint32_t seg;
        float urban;
        fake_user_def(&w, 3, id, &d, &seg, &urban);
        bool is_west = g.segs[seg].region == west;
        in_west += is_west;
        n_east += !is_west;
        T_CHECK(urban == (is_west ? 1.0f : 0.0f));
        bool foot = d.kind == ADV_UK_RUNNER || d.kind == ADV_UK_WALKER;
        if (is_west) foot_west += foot;
        else foot_east += foot;
    }
    T_CHECK_MSG(fabs(in_west / (double)N - 90.0 / 93.0) < 0.015, "%u of %u homes in West", in_west, N);
    T_CHECK_MSG(foot_west > 0.50 * in_west && foot_west < 0.60 * in_west, "city runners and walkers %u of %u", foot_west, in_west);
    T_CHECK_MSG(foot_east < 0.35 * n_east, "suburb runners and walkers %u of %u", foot_east, n_east);
    // Starts: a city runner's activities start on a footpath about 0.7 of the time.
    fake_user *u = malloc(sizeof *u);
    fake_route_out *ro = malloc(sizeof *ro);
    fake_act *a = malloc(sizeof *a);
    uint32_t id = 1, tried = 0;
    for (; id < 400; id++) {
        fake_user_init(&w, u, 3, id);
        if (u->urban == 1.0f && u->def.kind == ADV_UK_RUNNER) break;
    }
    T_CHECK(id < 400);
    fake_rng r;
    fake_rng_seed(&r, 99);
    uint32_t away = 0;
    for (int i = 0; i < 400; i++) {
        if (!fake_activity(&g, u, &r, 1700000000000LL, 0, NULL, ro, a)) continue;
        tried++;
        uint32_t from = ro->leg[0].reversed ? g.segs[ro->leg[0].seg].node_b : g.segs[ro->leg[0].seg].node_a;
        away += from != u->home_node;
    }
    T_CHECK_MSG(tried > 300 && away > 0.55 * tried && away < 0.85 * tried, "%u of %u started away from home", away, tried);
    free(u);
    free(ro);
    free(a);
    fake_world_free(&w);
    adv_graph_close(&g);
    unlink(path);
    // Nested: "Whole" (level 7, 100,000 people) holds West (level 8, 90,000); Whole's own streets
    // are the east half, its own people 10,000, so West has 90 of every 100 homes, not 90 of 190.
    static const fg_opts NEST = { .cols = 12, .rows = 12, .regions = true, .foot_only_rows = true, .pop_west = 90000, .pop_east = 100000,
                                  .nested = true };
    fg_tmp_path(path, sizeof path, "unit_gen_nest");
    T_CHECK(fg_build(&NEST, path, &g) == 0);
    T_CHECK(fake_world_init(&w, &g) == CB_OK);
    west = g.regions[0].name[0] == 'W' ? 0 : 1;
    in_west = 0;
    for (uint32_t i = 1; i <= N; i++) {
        adv_w_user_def d;
        uint32_t seg;
        fake_user_def(&w, 3, i, &d, &seg, NULL);
        in_west += g.segs[seg].region == west;
    }
    T_CHECK_MSG(fabs(in_west / (double)N - 0.90) < 0.015, "nested: %u of %u homes in West", in_west, N);
    fake_world_free(&w);
    adv_graph_close(&g);
    unlink(path);
    // Untagged: homes follow street length, half and half.
    fg_tmp_path(path, sizeof path, "unit_gen_flat");
    T_CHECK(fg_build(&GRID, path, &g) == 0);
    T_CHECK((fake_world_init(&w, &g)) == (CB_OK));
    T_CHECK((w.n_tagged) == (0));
    in_west = 0;
    for (uint32_t i = 1; i <= N; i++) {
        adv_w_user_def d;
        uint32_t seg;
        fake_user_def(&w, 3, i, &d, &seg, NULL);
        in_west += g.segs[seg].region == west;
    }
    T_CHECK_MSG(in_west > 0.40 * N && in_west < 0.60 * N, "untagged: %u of %u homes in West", in_west, N);
    fake_world_free(&w);
    adv_graph_close(&g);
    unlink(path);
}

int main(void) {
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_gen");
    adv_graph g;
    T_CHECK(fg_build(&GRID, path, &g) == 0);
    T_CHECK((fake_world_init(&W, &g)) == (CB_OK));
    test_world();
    test_users(&g);
    test_routes(&g);
    test_sampler(&g);
    test_hostile(&g);
    fake_world_free(&W);
    adv_graph_close(&g);
    unlink(path);
    return t_done("unit_gen");
}
