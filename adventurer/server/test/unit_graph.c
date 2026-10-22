// unit_graph.c - the graph file: built through the builder, read back, every index consistent;
// the cell index finds what is there; shapes and masks; coverage merges; the trace codec.
#include "coverage.h"
#include "graph.h"
#include "lib_fakegraph.h"
#include "t.h"
#include "trace.h"
#include "util/err.h"

#include <math.h>
#include <unistd.h>

static void test_geo(void) {
    adv_pt a = { 418000000, -714000000 }, b = { 418000000 + 9000, -714000000 };   // 0.0009 deg lat = 100.1 m
    adv_frame f = adv_frame_at(a.lat);
    double d = adv_dist_m(&f, a, b);
    T_CHECK_MSG(fabs(d - 100.1) < 0.5, "d=%f", d);
    double t;
    adv_pt p = { 418004500, -714000000 + 1000 };
    double ds = adv_point_seg_m(&f, p, a, b, &t);
    T_CHECK(fabs(t - 0.5) < 0.01);
    T_CHECK_MSG(fabs(ds - 1000 * f.mx) < 0.1, "ds=%f", ds);
    uint32_t x, y;
    adv_tile_of(a, 16, &x, &y);
    adv_pt mn, mx;
    adv_tile_bounds(16, x, y, &mn, &mx);
    T_CHECK(a.lat >= mn.lat && a.lat <= mx.lat && a.lon >= mn.lon && a.lon <= mx.lon);
    double px, py;
    adv_tile_project(16, x, y, a, &px, &py);
    T_CHECK(px >= 0 && px <= ADV_MVT_EXTENT && py >= 0 && py <= ADV_MVT_EXTENT);
    for (uint32_t d2 = 0; d2 < 64; d2++) {
        uint32_t hx, hy;
        adv_hilbert_d2xy(8, d2, &hx, &hy);
        T_CHECK(adv_hilbert_xy2d(8, hx, hy) == d2);
    }
    // Consecutive Hilbert indices are grid neighbours.
    for (uint32_t d2 = 1; d2 < 64; d2++) {
        uint32_t x0, y0, x1, y1;
        adv_hilbert_d2xy(8, d2 - 1, &x0, &y0);
        adv_hilbert_d2xy(8, d2, &x1, &y1);
        T_CHECK(abs((int)x0 - (int)x1) + abs((int)y0 - (int)y1) == 1);
    }
}

static void test_graph(void) {
    fg_opts o = { .cols = 4, .rows = 3, .regions = true, .foot_only_rows = true };
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_graph");
    adv_graph g;
    T_CHECK(fg_build(&o, path, &g) == 0);
    // 4 blocks x 3 rows: (rows+1)*cols east-west streets + (cols+1)*rows north-south ones.
    uint32_t expect = (uint32_t)((o.rows + 1) * o.cols + (o.cols + 1) * o.rows);
    T_CHECK_MSG(g.h->n_segs == expect, "n_segs=%u expect %u", g.h->n_segs, expect);
    T_CHECK(g.h->n_nodes == (uint32_t)((o.cols + 1) * (o.rows + 1)));
    T_CHECK(g.h->n_regions == 2);
    // Every segment: 120 m, 2 interior shape points (40 m apart), both nodes real, a region.
    uint32_t in_region = 0, foot_only = 0;
    for (uint32_t i = 0; i < g.h->n_segs; i++) {
        const adv_seg *s = &g.segs[i];
        T_CHECK_MSG(fabs(s->len_m - 120) < 0.6, "seg %u len %f", i, s->len_m);
        T_CHECK(s->n_pts == 4);
        T_CHECK(s->node_a != s->node_b);
        if (s->region != ADV_NO_REGION) in_region++;
        if (s->access == ADV_ACC_FOOT) foot_only++;
        adv_pt pts[ADV_SEG_MAX_PTS];
        int n = adv_seg_points(&g, i, pts, ADV_SEG_MAX_PTS);
        T_CHECK(n == 4);
        T_CHECK(pts[0].lat >= s->min_lat && pts[0].lat <= s->max_lat);
        T_CHECK(pts[3].lat >= s->min_lat && pts[3].lat <= s->max_lat && pts[3].lon >= s->min_lon && pts[3].lon <= s->max_lon);
        // The end points are the nodes'.
        T_CHECK(g.nodes[s->node_a].lat == pts[0].lat && g.nodes[s->node_a].lon == pts[0].lon);
        T_CHECK(g.nodes[s->node_b].lat == pts[3].lat && g.nodes[s->node_b].lon == pts[3].lon);
        // Each node lists the segment.
        bool found_a = false, found_b = false;
        for (uint32_t k = 0; k < g.nodes[s->node_a].n_adj; k++) if (g.adj[g.nodes[s->node_a].adj_off + k] == i) found_a = true;
        for (uint32_t k = 0; k < g.nodes[s->node_b].n_adj; k++) if (g.adj[g.nodes[s->node_b].adj_off + k] == i) found_b = true;
        T_CHECK(found_a && found_b);
        // The midpoint is in a cell whose list holds the segment.
        adv_pt mid = adv_seg_at(&g, i, 0.5);
        uint32_t ci, cj;
        T_CHECK(adv_cell_of(&g, mid, &ci, &cj));
        uint32_t n_list;
        const uint32_t *list = adv_cell_list(&g, ci, cj, &n_list);
        bool listed = false;
        for (uint32_t k = 0; k < n_list; k++) if (list[k] == i) listed = true;
        T_CHECK(listed);
    }
    T_CHECK_MSG(in_region == g.h->n_segs, "in_region=%u", in_region);
    T_CHECK_MSG(foot_only == (uint32_t)(2 * o.cols), "foot_only=%u", foot_only);   // rows 1 and 3
    // The corner node has two segments, an inner one four.
    uint32_t deg[8] = { 0 };
    for (uint32_t i = 0; i < g.h->n_nodes; i++) deg[g.nodes[i].n_adj > 7 ? 7 : g.nodes[i].n_adj]++;
    T_CHECK(deg[2] == 4 && deg[4] == (uint32_t)((o.cols - 1) * (o.rows - 1)));
    // Region totals: West + East = everything; foot total > bike total (footpaths).
    double west_all = g.regions[0].len_m[ADV_T_ALL] + g.regions[1].len_m[ADV_T_ALL];
    T_CHECK_MSG(fabs(west_all - g.h->total_len_m[ADV_T_ALL]) < 1, "regions %f total %f", west_all, g.h->total_len_m[ADV_T_ALL]);
    T_CHECK(g.h->total_len_m[ADV_T_FOOT] > g.h->total_len_m[ADV_T_BIKE]);
    T_CHECK(strcmp(g.regions[0].name, "West") == 0 && strcmp(g.regions[1].name, "East") == 0);
    // Hilbert order: the first and last segments are far apart, neighbours close.
    adv_frame f = adv_frame_at(g.h->min_lat);
    double sum_adjacent = 0;
    for (uint32_t i = 1; i < g.h->n_segs; i++) {
        adv_pt a = { g.segs[i - 1].lat, g.segs[i - 1].lon }, b = { g.segs[i].lat, g.segs[i].lon };
        sum_adjacent += adv_dist_m(&f, a, b);
    }
    T_CHECK_MSG(sum_adjacent / (g.h->n_segs - 1) < 250, "mean neighbour distance %f", sum_adjacent / (g.h->n_segs - 1));
    // adv_seg_at walks the length.
    adv_pt q0 = adv_seg_at(&g, 0, 0.0), q1 = adv_seg_at(&g, 0, 1.0), qm = adv_seg_at(&g, 0, 0.25);
    T_CHECK(fabs(adv_dist_m(&f, q0, qm) - 30) < 1.5);
    T_CHECK(fabs(adv_dist_m(&f, q0, q1) - 120) < 1.5);
    // A damaged file refuses: flip a byte in a segment.
    {
        FILE *fp = fopen(path, "rb");
        T_CHECK(fp != NULL);
        uint8_t *buf = malloc(g.map_len);
        T_CHECK(fread(buf, 1, g.map_len, fp) == g.map_len);
        fclose(fp);
        char err[256];
        T_CHECK(adv_graph_check(buf, g.map_len, err, sizeof err) == CB_OK);
        buf[g.h->off_segs + 20] ^= 0x40;
        T_CHECK(adv_graph_check(buf, g.map_len, err, sizeof err) == CB_E_INVALID);
        T_CHECK(adv_graph_check(buf, g.map_len - 1, err, sizeof err) == CB_E_INVALID);
        free(buf);
    }
    // Masks of a run.
    T_CHECK(adv_mask_of_run(0, 1) == 0xFFFF);
    T_CHECK(adv_mask_of_run(0, 0.5) == 0x00FF);
    T_CHECK(adv_mask_of_run(0.5, 0) == 0x00FF);
    T_CHECK(adv_mask_of_run(0.3, 0.3) != 0);   // a point run still marks its sixteenth
    T_CHECK(__builtin_popcount(adv_mask_of_run(0.3, 0.3)) == 1);
    T_CHECK(adv_mask_of_run(0.0, 0.0625 * 0.1) == 0x0001);   // a sliver of the first sixteenth
    T_CHECK(fabs(adv_mask_len_m(0x00FF, 120) - 60) < 0.01);
    adv_graph_close(&g);
    unlink(path);
}

static void test_coverage(void) {
    fg_opts o = { .cols = 3, .rows = 3 };
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_cov");
    adv_graph g;
    T_CHECK(fg_build(&o, path, &g) == 0);
    adv_cov c;
    adv_cov_init(&c);
    adv_hit raw[6] = { { 5, 0x00F0, 0 }, { 2, 0x000F, 0 }, { 5, 0x0F00, 0 }, { 9, 0xFFFF, 0 }, { 2, 0, 0 }, { 7, 0x0001, 0 } };
    uint32_t n = adv_hits_finish(raw, 6);
    T_CHECK(n == 4);
    T_CHECK(raw[0].seg == 2 && raw[0].mask == 0x000F);
    T_CHECK(raw[1].seg == 5 && raw[1].mask == 0x0FF0);
    T_CHECK(raw[2].seg == 7 && raw[3].seg == 9);
    float new_m[3] = { 0, 0, 0 };
    adv_cov_delta d[4];
    T_CHECK(adv_cov_merge(&c, &g, raw, n, ADV_T_BIKE, d, new_m) == CB_OK);
    T_CHECK(c.n == 4);
    float expect = (4 + 8 + 1 + 16) * 120.0f / 16;
    T_CHECK_MSG(fabs(new_m[ADV_T_BIKE] - expect) < 0.1 && fabs(new_m[ADV_T_ALL] - expect) < 0.1 && new_m[ADV_T_FOOT] == 0,
                "new %f %f %f", new_m[0], new_m[1], new_m[2]);
    T_CHECK(d[1].seg == 5 && d[1].new_mask[ADV_T_BIKE] == 0x0FF0 && d[1].new_mask[ADV_T_ALL] == 0x0FF0 && d[1].new_mask[ADV_T_FOOT] == 0);
    T_CHECK(adv_cov_mask(&c, 5, ADV_T_BIKE) == 0x0FF0 && adv_cov_mask(&c, 5, ADV_T_FOOT) == 0 && adv_cov_mask(&c, 5, ADV_T_ALL) == 0x0FF0);
    T_CHECK(adv_cov_mask(&c, 6, ADV_T_ALL) == 0);
    // A foot activity over some of the same segments: new for foot, partly new for all.
    adv_hit h2[3] = { { 1, 0x0003, 0 }, { 5, 0x00FF, 0 }, { 12, 0x8000, 0 } };
    float new2[3] = { 0, 0, 0 };
    adv_cov_delta d2[3];
    T_CHECK(adv_cov_merge(&c, &g, h2, 3, ADV_T_FOOT, d2, new2) == CB_OK);
    T_CHECK(c.n == 6);
    T_CHECK_MSG(fabs(new2[ADV_T_FOOT] - (2 + 8 + 1) * 7.5) < 0.1, "foot %f", new2[ADV_T_FOOT]);
    T_CHECK_MSG(fabs(new2[ADV_T_ALL] - (2 + 4 + 1) * 7.5) < 0.1, "all %f", new2[ADV_T_ALL]);
    T_CHECK(new2[ADV_T_BIKE] == 0);
    T_CHECK(d2[1].new_mask[ADV_T_ALL] == 0x000F && d2[1].new_mask[ADV_T_FOOT] == 0x00FF);
    T_CHECK(adv_cov_mask(&c, 5, ADV_T_ALL) == 0x0FFF);
    // Sorted and unique throughout.
    for (uint32_t i = 1; i < c.n; i++) T_CHECK(c.e[i - 1].seg < c.e[i].seg);
    float tot[3];
    adv_cov_total_m(&c, &g, tot);
    T_CHECK_MSG(fabs(tot[ADV_T_ALL] - (expect + (2 + 4 + 1) * 7.5)) < 0.1, "tot %f", tot[ADV_T_ALL]);
    // Common length between two hit lists.
    adv_hit a[2] = { { 1, 0x00FF, 0 }, { 3, 0xFFFF, 0 } }, b[2] = { { 1, 0x0F0F, 0 }, { 4, 0xFFFF, 0 } };
    T_CHECK(fabs(adv_hits_common_m(&g, a, 2, b, 2) - 4 * 7.5) < 0.01);
    T_CHECK(fabs(adv_hits_len_m(&g, a, 2) - 24 * 7.5) < 0.01);
    // Merging many: growth and order.
    adv_hit many[200];
    for (int i = 0; i < 200; i++) { many[i].seg = (uint32_t)(i % g.h->n_segs); many[i].mask = (uint16_t)(1u << (i % 16)); many[i].pad = 0; }
    uint32_t nm = adv_hits_finish(many, 200);
    float nm3[3] = { 0, 0, 0 };
    T_CHECK(adv_cov_merge(&c, &g, many, nm, ADV_T_BIKE, NULL, nm3) == CB_OK);
    for (uint32_t i = 1; i < c.n; i++) T_CHECK(c.e[i - 1].seg < c.e[i].seg);
    T_CHECK(c.n == g.h->n_segs);
    adv_cov_free(&c);
    adv_graph_close(&g);
    unlink(path);
}

static void test_trace(void) {
    adv_trace_pt pts[3] = { { 418000000, -714000000, 0 }, { 418000100, -714000100, 1000 }, { 418000200, -714000200, 2000 } };
    adv_truth tr[2] = { { 3, 0x00FF, 0 }, { 9, 0x0001, 0 } };
    adv_trace_hdr h = { 0 };
    h.type = ADV_ACT_BIKE;
    h.flags = ADV_TRF_BACKFILL;
    h.user = 7;
    h.n_points = 3;
    h.start_ms = 1700000000000LL;
    h.gen_id = 42;
    uint8_t buf[256];
    size_t n = adv_trace_write(buf, sizeof buf, &h, pts, tr, 2);
    T_CHECK(n == 32 + 36 + 4 + 16);
    T_CHECK(n == adv_trace_size(3, 2));
    adv_trace t;
    T_CHECK(adv_trace_parse(buf, n, &t) == CB_OK);
    T_CHECK(t.hdr.user == 7 && t.hdr.n_points == 3 && t.hdr.type == ADV_ACT_BIKE && t.hdr.gen_id == 42);
    T_CHECK((t.hdr.flags & ADV_TRF_TRUTH) && (t.hdr.flags & ADV_TRF_BACKFILL) && t.hdr.start_ms == 1700000000000LL);
    T_CHECK(t.n_truth == 2 && adv_trace_truth_at(&t, 1).seg == 9);
    adv_trace_pt p1 = adv_trace_pt_at(&t, 1);
    T_CHECK(p1.lat == 418000100 && p1.lon == -714000100 && p1.t_ms == 1000);
    // Unaligned: the same bytes one byte over.
    uint8_t ubuf[257];
    memcpy(ubuf + 1, buf, n);
    T_CHECK(adv_trace_parse(ubuf + 1, n, &t) == CB_OK && adv_trace_pt_at(&t, 2).t_ms == 2000);
    // Refusals: short, a byte over, a bad count, a bad type, truth out of order.
    T_CHECK(adv_trace_parse(buf, n - 1, &t) == CB_E_INVALID);
    T_CHECK(adv_trace_parse(buf, 31, &t) == CB_E_INVALID);
    uint8_t big[300];
    memcpy(big, buf, n);
    T_CHECK(adv_trace_parse(big, n + 1, &t) == CB_E_INVALID);
    memcpy(big, buf, n);
    big[12] = 200;   // n_points
    T_CHECK(adv_trace_parse(big, n, &t) == CB_E_INVALID);
    memcpy(big, buf, n);
    big[6] = 9;
    T_CHECK(adv_trace_parse(big, n, &t) == CB_E_INVALID);
    memcpy(big, buf, n);
    big[32 + 36 + 4] = 9;   // first truth seg 9 == second: not ascending
    T_CHECK(adv_trace_parse(big, n, &t) == CB_E_INVALID);
    // Without a truth list, and no flag: fine.
    size_t n0 = adv_trace_write(buf, sizeof buf, &h, pts, NULL, 0);
    T_CHECK(n0 == 32 + 36 && adv_trace_parse(buf, n0, &t) == CB_OK && t.truth == NULL && !(t.hdr.flags & ADV_TRF_TRUTH));
    T_CHECK(adv_trace_write(buf, 10, &h, pts, NULL, 0) == 0);
}

int main(void) {
    test_geo();
    test_graph();
    test_coverage();
    test_trace();
    return t_done("unit_graph");
}
