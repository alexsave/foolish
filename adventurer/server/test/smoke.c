// smoke.c - the whole product end to end against a real `adventurer serve`: the roster posted,
// an upload matched and credited, the feed telling it, the stats and the tiles answering, a
// hostile upload refused, and the state surviving a restart through the journal.
//
//   build/test/smoke build/adventurer build/adv-gen
#include "coverage.h"
#include "graph.h"
#include "lib_client.h"
#include "lib_fakegraph.h"
#include "lib_proc.h"
#include "net/ws.h"
#include "t.h"
#include "trace.h"
#include "wire.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_bin;
static char g_dir[256], g_graph[512], g_db[512];
static fg_opts g_opts = { .cols = 8, .rows = 6, .regions = true };

static int start_server(tp_child *c) {
    char db_env[600], graph_env[600];
    snprintf(db_env, sizeof db_env, "CBASE_DB=%s", g_db);
    snprintf(graph_env, sizeof graph_env, "ADV_GRAPH=%s", g_graph);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_LOG_LEVEL=info", "ADV_WORKERS=1", db_env, graph_env, NULL };
    char *argv[] = { (char *)g_bin, "serve", NULL };
    if (tp_start(c, argv, env) != 0) return -1;
    char line[512];
    if (tp_wait_line(c, "listening on 127.0.0.1:", 20000, line, sizeof line) != 0) return -1;
    const char *p = strstr(line, "listening on 127.0.0.1:");
    return atoi(p + strlen("listening on 127.0.0.1:"));
}

// The trace along row `row` of the grid from crossing i0 to i1, a point every 6 m, and its truth.
static size_t make_trace(int row, int i0, int i1, uint32_t user, uint8_t type, const adv_graph *g, uint8_t *out, size_t cap, float *truth_m) {
    adv_pt a = fg_crossing(&g_opts, i0, row), b = fg_crossing(&g_opts, i1, row);
    adv_frame f = adv_frame_at(a.lat);
    double len = adv_dist_m(&f, a, b);
    uint32_t n = (uint32_t)(len / 6) + 1;
    static adv_trace_pt pts[ADV_TRACE_MAX_PTS];
    for (uint32_t i = 0; i < n; i++) {
        double t = (double)i / (n - 1);
        pts[i].lat = (int32_t)(a.lat + t * ((double)b.lat - a.lat) + ((i % 3) - 1) * 3 / f.my);
        pts[i].lon = (int32_t)(a.lon + t * ((double)b.lon - a.lon));
        pts[i].t_ms = i * 1000;
    }
    // Truth: the row's segments between the two crossings (both end nodes at the row's latitude).
    static adv_hit truth[4096];
    uint32_t nt = 0;
    for (uint32_t s = 0; s < g->h->n_segs && nt < 4096; s++) {
        const adv_seg *sg = &g->segs[s];
        const adv_node *na = &g->nodes[sg->node_a], *nb = &g->nodes[sg->node_b];
        if (abs(na->lat - a.lat) > 3 || abs(nb->lat - a.lat) > 3) continue;
        int32_t lo = a.lon < b.lon ? a.lon : b.lon, hi = a.lon < b.lon ? b.lon : a.lon;
        if (na->lon < lo - 3 || na->lon > hi + 3 || nb->lon < lo - 3 || nb->lon > hi + 3) continue;
        truth[nt].seg = s;
        truth[nt].mask = 0xFFFF;
        truth[nt].pad = 0;
        nt++;
    }
    nt = adv_hits_finish(truth, nt);
    *truth_m = adv_hits_len_m(g, truth, nt);
    adv_trace_hdr h = { .type = type, .flags = 0, .user = user, .n_points = n, .start_ms = 1760000000000LL, .gen_id = 77 };
    return adv_trace_write(out, cap, &h, pts, (const adv_truth *)truth, nt);
}

static int fetch(int port, const char *path, tc_response *r) { return tc_http(port, "GET", path, NULL, NULL, 0, r); }

static void check_user_stats(int port, uint32_t id, uint32_t want_acts, float want_m, adv_w_user_stats *out) {
    char path[64];
    snprintf(path, sizeof path, "/adv/user/%u", id);
    tc_response r;
    T_CHECK(fetch(port, path, &r) == 0 && r.status == 200);
    T_CHECK(r.body.len >= sizeof(adv_w_user_stats));
    adv_w_user_stats s;
    memcpy(&s, r.body.p, sizeof s);
    T_CHECK(s.hash == adv_wire_hash() && s.id == id);
    T_CHECK_MSG(s.n_activities == want_acts, "n_activities %u want %u", s.n_activities, want_acts);
    T_CHECK_MSG(fabs(s.unique_m[ADV_T_ALL] - want_m) < want_m * 0.1 + 1, "unique %f want %f", s.unique_m[ADV_T_ALL], want_m);
    T_CHECK(r.body.len == sizeof s + (size_t)s.n_regions * sizeof(adv_w_region_progress));
    if (out) *out = s;
    tc_response_free(&r);
}

// Reads feed frames until an activity frame for `user` arrives (at most `tries` frames).
static bool feed_activity(int ws, uint32_t user, adv_w_activity *out, int tries) {
    for (int i = 0; i < tries; i++) {
        cb_buf fr;
        cb_buf_init(&fr);
        int op = tc_ws_recv(ws, 15000, &fr);
        if (op < 0) { cb_buf_free(&fr); return false; }
        if (op == CB_WS_OP_BIN && fr.len == sizeof(adv_w_activity) && fr.p[0] == ADV_W_KIND_ACTIVITY) {
            memcpy(out, fr.p, sizeof *out);
            cb_buf_free(&fr);
            if (out->user == user) return true;
            continue;
        }
        cb_buf_free(&fr);
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: smoke <path to adventurer> [adv-gen]\n"); return 2; }
    g_bin = argv[1];
    signal(SIGPIPE, SIG_IGN);
    T_CHECK(tp_tempdir(g_dir, sizeof g_dir) == 0);
    snprintf(g_graph, sizeof g_graph, "%s/smoke.graph", g_dir);
    snprintf(g_db, sizeof g_db, "%s/adv.sqlite", g_dir);
    adv_graph g;
    T_CHECK(fg_build(&g_opts, g_graph, &g) == 0);

    tp_child c;
    int port = start_server(&c);
    T_CHECK_MSG(port > 0, "the server did not start");
    if (port <= 0) return t_done("smoke");
    tc_response r;
    T_CHECK(fetch(port, "/health", &r) == 0 && r.status == 200);
    tc_response_free(&r);
    // Ops before anything: zero users, the graph's numbers.
    T_CHECK(fetch(port, "/adv/ops", &r) == 0 && r.status == 200 && r.body.len == sizeof(adv_w_ops));
    adv_w_ops ops;
    memcpy(&ops, r.body.p, sizeof ops);
    T_CHECK(ops.kind == ADV_W_KIND_OPS && ops.hash == adv_wire_hash() && ops.users == 0 && ops.segs == g.h->n_segs && ops.regions == 2);
    tc_response_free(&r);
    // The roster.
    struct { adv_w_list head; adv_w_user_def u[2]; } roster = {
        { adv_wire_hash(), 2 },
        { { 1, ADV_UK_EXPLORER, 4, 0, 418000000, -714000000, "Mara" }, { 2, ADV_UK_WALKER, 3, 0, 418000000, -714000000, "Jon" } } };
    T_CHECK(tc_http(port, "POST", "/adv/users", "Content-Type: application/octet-stream\r\n", &roster, sizeof roster, &r) == 0 && r.status == 204);
    tc_response_free(&r);
    T_CHECK(fetch(port, "/adv/users", &r) == 0 && r.status == 200 && r.body.len == sizeof(adv_w_list) + 2 * sizeof(adv_w_user));
    adv_w_user u1;
    memcpy(&u1, r.body.p + sizeof(adv_w_list), sizeof u1);
    T_CHECK(u1.id == 1 && u1.kind == ADV_UK_EXPLORER && strcmp(u1.name, "Mara") == 0 && u1.n_activities == 0);
    tc_response_free(&r);
    T_CHECK(fetch(port, "/adv/regions", &r) == 0 && r.status == 200 && r.body.len == sizeof(adv_w_list) + 2 * sizeof(adv_w_region));
    adv_w_region rg0;
    memcpy(&rg0, r.body.p + sizeof(adv_w_list), sizeof rg0);
    T_CHECK(strcmp(rg0.name, "West") == 0 && rg0.len_m[ADV_T_ALL] > 0);
    tc_response_free(&r);
    // The feed, then an upload along row 3 from crossing 0 to 8 (8 segments, 960 m).
    int ws = tc_ws_open(port, "/adv/feed", NULL);
    T_CHECK(ws >= 0);
    static uint8_t body[1 << 20];
    float truth_m;
    size_t n = make_trace(3, 0, 8, 1, ADV_ACT_BIKE, &g, body, sizeof body, &truth_m);
    T_CHECK(n > 0 && fabs(truth_m - 960) < 5);
    T_CHECK(tc_http(port, "POST", "/adv/upload", "Content-Type: application/octet-stream\r\n", body, n, &r) == 0 && r.status == 202);
    T_CHECK(r.body.len == sizeof(adv_w_accepted));
    adv_w_accepted acc;
    memcpy(&acc, r.body.p, sizeof acc);
    T_CHECK(acc.hash == adv_wire_hash() && acc.activity == 1);
    tc_response_free(&r);
    adv_w_activity ev;
    T_CHECK(feed_activity(ws, 1, &ev, 20));
    T_CHECK_MSG(!(ev.flags & ADV_AF_REJECTED), "rejected, reason %u", ev.reason);
    T_CHECK((ev.flags & ADV_AF_TRUTH) && ev.type == ADV_ACT_BIKE && ev.gen_id == 77 && ev.activity == 1);
    T_CHECK_MSG(ev.accuracy >= 0.9f, "accuracy %f", ev.accuracy);
    T_CHECK_MSG(fabs(ev.new_m[ADV_T_ALL] - truth_m) < truth_m * 0.1, "new %f truth %f", ev.new_m[ADV_T_ALL], truth_m);
    T_CHECK(ev.new_m[ADV_T_BIKE] > 0 && ev.new_m[ADV_T_FOOT] == 0);
    T_CHECK_MSG(ev.cpu_us < 100000 && ev.e2e_us < 2000000, "cpu %u e2e %u", ev.cpu_us, ev.e2e_us);
    adv_w_user_stats st;
    check_user_stats(port, 1, 1, truth_m, &st);
    T_CHECK(st.version == 1 && st.n_regions >= 1 && st.n_super_unique_segs == 8);
    // The same ride by user 2 on foot: credited to foot, and the segments stop being unique.
    n = make_trace(3, 0, 8, 2, ADV_ACT_WALK, &g, body, sizeof body, &truth_m);
    T_CHECK(tc_http(port, "POST", "/adv/upload", "Content-Type: application/octet-stream\r\n", body, n, &r) == 0 && r.status == 202);
    tc_response_free(&r);
    T_CHECK(feed_activity(ws, 2, &ev, 20) && !(ev.flags & ADV_AF_REJECTED) && ev.new_m[ADV_T_FOOT] > 0 && ev.new_m[ADV_T_BIKE] == 0);
    check_user_stats(port, 1, 1, truth_m, &st);
    T_CHECK(st.n_super_unique_segs == 0);
    // Leaders of the region the row is in: both users on the combined board.
    T_CHECK(fetch(port, "/adv/leaders/0", &r) == 0 && r.status == 200 && r.body.len == sizeof(adv_w_leaders));
    adv_w_leaders lb;
    memcpy(&lb, r.body.p, sizeof lb);
    T_CHECK(lb.hash == adv_wire_hash() && lb.region == 0);
    tc_response_free(&r);
    T_CHECK(fetch(port, "/adv/leaders/1", &r) == 0 && r.status == 200);
    adv_w_leaders lb1;
    memcpy(&lb1, r.body.p, sizeof lb1);
    tc_response_free(&r);
    // The row crosses both regions: whichever holds the ride's start names user 1 first on the bike board.
    bool on_board = (lb.month[ADV_T_BIKE][0].user == 1) || (lb1.month[ADV_T_BIKE][0].user == 1);
    T_CHECK(on_board);
    // Tiles: the z16 tile of the row's middle, for user 1, with traveled features; a second ask with the ETag is 304.
    adv_pt mid = fg_crossing(&g_opts, 4, 3);
    uint32_t tx, ty;
    adv_tile_of(mid, 16, &tx, &ty);
    char path[128];
    snprintf(path, sizeof path, "/adv/tile/1/16/%u/%u.mvt?t=all", tx, ty);
    T_CHECK(fetch(port, path, &r) == 0 && r.status == 200 && r.body.len > 20);
    char etag[128] = "", ctype[128] = "";
    T_CHECK(tc_header(&r, "ETag", etag, sizeof etag) != NULL && etag[0] == '"');
    T_CHECK(tc_header(&r, "Content-Type", ctype, sizeof ctype) && strcmp(ctype, "application/vnd.mapbox-vector-tile") == 0);
    // The layer names are in the protobuf as plain strings.
    T_CHECK(memmem(r.body.p, r.body.len, "traveled", 8) != NULL && memmem(r.body.p, r.body.len, "untraveled", 10) != NULL);
    tc_response_free(&r);
    char inm[192];
    snprintf(inm, sizeof inm, "If-None-Match: %s\r\n", etag);
    T_CHECK(tc_http(port, "GET", path, inm, NULL, 0, &r) == 0 && r.status == 304);
    tc_response_free(&r);
    snprintf(path, sizeof path, "/adv/base/16/%u/%u.mvt", tx, ty);
    T_CHECK(fetch(port, path, &r) == 0 && r.status == 200 && memmem(r.body.p, r.body.len, "roads", 5) != NULL);
    tc_response_free(&r);
    T_CHECK(fetch(port, "/adv/base/16/1/1.mvt", &r) == 0 && r.status == 200);   // far away: an empty tile, still a tile
    tc_response_free(&r);
    T_CHECK(fetch(port, "/adv/tile/1/16/abc/1.mvt", &r) == 0 && r.status == 404);
    tc_response_free(&r);
    T_CHECK(fetch(port, "/adv/tile/9/16/1/1.mvt", &r) == 0 && r.status == 404);
    tc_response_free(&r);
    // Hostile uploads: garbage is 400, an unknown user 404, a standing-still trace is accepted then rejected by the matcher.
    T_CHECK(tc_http(port, "POST", "/adv/upload", "Content-Type: application/octet-stream\r\n", "nonsense", 8, &r) == 0 && r.status == 400);
    tc_response_free(&r);
    n = make_trace(1, 0, 2, 9, ADV_ACT_BIKE, &g, body, sizeof body, &truth_m);
    T_CHECK(tc_http(port, "POST", "/adv/upload", "Content-Type: application/octet-stream\r\n", body, n, &r) == 0 && r.status == 404);
    tc_response_free(&r);
    {
        static adv_trace_pt still[500];
        for (int i = 0; i < 500; i++) { still[i].lat = mid.lat; still[i].lon = mid.lon; still[i].t_ms = (uint32_t)i * 1000; }
        adv_trace_hdr h = { .type = ADV_ACT_RUN, .user = 2, .n_points = 500, .start_ms = 1760000000000LL, .gen_id = 5 };
        n = adv_trace_write(body, sizeof body, &h, still, NULL, 0);
        T_CHECK(tc_http(port, "POST", "/adv/upload", "Content-Type: application/octet-stream\r\n", body, n, &r) == 0 && r.status == 202);
        tc_response_free(&r);
        T_CHECK(feed_activity(ws, 2, &ev, 20) && (ev.flags & ADV_AF_REJECTED) && ev.reason == 1 && ev.gen_id == 5);
    }
    T_CHECK(fetch(port, "/adv/ops", &r) == 0 && r.status == 200);
    memcpy(&ops, r.body.p, sizeof ops);
    T_CHECK(ops.users == 2 && ops.activities == 2 && ops.rejected == 1 && ops.feed_conns == 1 && ops.act_cpu_p99_us > 0 && ops.tile_p50_us > 0);
    tc_response_free(&r);
    close(ws);
    // Restart: the journal rebuilds both users, the one accepted ride each, and the ownership.
    T_CHECK(tp_stop(&c, SIGTERM, 15000) == 0);
    port = start_server(&c);
    T_CHECK_MSG(port > 0, "the server did not restart");
    if (port > 0) {
        check_user_stats(port, 1, 1, truth_m, &st);
        T_CHECK(st.n_super_unique_segs == 0 && st.version == 1);
        check_user_stats(port, 2, 1, truth_m, &st);
        T_CHECK(fetch(port, "/adv/ops", &r) == 0 && r.status == 200);
        memcpy(&ops, r.body.p, sizeof ops);
        T_CHECK_MSG(ops.recovered == 2 && ops.users == 2 && ops.activities == 2, "recovered %u users %u", ops.recovered, ops.users);
        tc_response_free(&r);
        // A new upload after the restart gets the next id.
        n = make_trace(5, 0, 4, 1, ADV_ACT_BIKE, &g, body, sizeof body, &truth_m);
        T_CHECK(tc_http(port, "POST", "/adv/upload", "Content-Type: application/octet-stream\r\n", body, n, &r) == 0 && r.status == 202);
        memcpy(&acc, r.body.p, sizeof acc);
        T_CHECK_MSG(acc.activity == 4, "activity %u", acc.activity);   // 1, 2 kept; 3 was the rejected one
        tc_response_free(&r);
        T_CHECK(tp_stop(&c, SIGTERM, 15000) == 0);
    }
    adv_graph_close(&g);
    return t_done("smoke");
}
