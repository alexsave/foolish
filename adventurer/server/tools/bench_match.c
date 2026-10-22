// bench_match.c - the matcher on real roads, in process: the fake world's rides (src/fake.h) on
// the graph, matched and scored, with the CPU per activity and the accuracy against the truth.
//
//   build/adv-bench <graph> [activities (500)] [seed (1)] [users (300)]
//
// What the server pays per upload without the HTTP, the queue and the journal around it; the
// numbers DESIGN.md quotes for the matcher come from here.
#include "coverage.h"
#include "fake.h"
#include "graph.h"
#include "match.h"
#include "util/clock.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: adv-bench <graph> [activities] [seed] [users]\n"); return 2; }
    int n_act = argc > 2 ? atoi(argv[2]) : 500, n_users = argc > 4 ? atoi(argv[4]) : 300;
    uint64_t seed = argc > 3 ? (uint64_t)atoll(argv[3]) : 1;
    if (n_act < 1 || n_act > 100000 || n_users < 1 || n_users > 65535) return 2;
    adv_graph g;
    char err[256];
    if (adv_graph_open(argv[1], &g, err, sizeof err) != CB_OK) { fprintf(stderr, "%s\n", err); return 1; }
    fake_user *users = calloc((size_t)n_users, sizeof *users);
    fake_route_out *route = calloc(1, sizeof *route);
    fake_act *act = calloc(1, sizeof *act);
    uint32_t *cpu = calloc((size_t)n_act, sizeof *cpu);
    adv_hit *truth = calloc(ADV_TRACE_MAX_TRUTH, sizeof *truth);
    adv_match_scratch *s = adv_match_scratch_new();
    if (!users || !route || !act || !cpu || !truth || !s) { fprintf(stderr, "no memory\n"); return 1; }
    fake_world w;
    if (fake_world_init(&w, &g) != CB_OK) { fprintf(stderr, "no memory\n"); return 1; }
    for (int i = 0; i < n_users; i++) fake_user_init(&w, &users[i], seed, (uint32_t)i + 1);
    fake_rng r;
    fake_rng_seed(&r, seed * 7919 + 17);
    double acc_sum = 0, recall_min = 1, prec_sum = 0;
    uint64_t points = 0, samples = 0, fast = 0, searches = 0, jumps = 0, hits = 0, matched_cm = 0;
    int made = 0, rejected = 0;
    // The three worst rides by recall, for a look at what the matcher misses.
    struct { double recall, prec; int i; uint8_t type, kind; uint32_t pts, samples, offroad, spikes, searches, jumps, hits; float truth_m, matched_m; } worst[3];
    for (int k = 0; k < 3; k++) { worst[k].recall = 2; worst[k].i = -1; }
    int64_t wall0 = cb_mono_us();
    for (int i = 0; i < n_act; i++) {
        fake_user *u = &users[fake_rng_below(&r, (uint32_t)n_users)];
        if (!fake_activity(&g, u, &r, 1760000000000LL + (int64_t)i * 1000, 0, NULL, route, act)) continue;
        adv_match_result res;
        adv_match(&g, act->pts, act->n_pts, act->hdr.type, NULL, s, &res);
        cpu[made++] = res.cpu_us;
        points += act->n_pts;
        if (res.rc != ADV_MATCH_OK) { rejected++; continue; }
        samples += res.n_samples;
        fast += res.n_fast;
        searches += res.n_searches;
        jumps += res.n_jumps;
        hits += res.n_hits;
        matched_cm += (uint64_t)(res.matched_m * 100);
        for (uint32_t k = 0; k < act->n_truth; k++) { truth[k].seg = act->truth[k].seg; truth[k].mask = act->truth[k].mask; truth[k].pad = 0; }
        float truth_m = adv_hits_len_m(&g, truth, act->n_truth);
        float common = adv_hits_common_m(&g, truth, act->n_truth, res.hits, res.n_hits);
        double recall = truth_m > 0 ? common / truth_m : 0, prec = res.matched_m > 0 ? common / res.matched_m : 0;
        acc_sum += recall;
        prec_sum += prec;
        if (recall < recall_min) recall_min = recall;
        for (int k = 0; k < 3; k++)
            if (recall < worst[k].recall) {
                for (int m = 2; m > k; m--) worst[m] = worst[m - 1];
                worst[k].recall = recall; worst[k].prec = prec; worst[k].i = i; worst[k].type = act->hdr.type; worst[k].kind = u->def.kind;
                worst[k].pts = act->n_pts; worst[k].samples = res.n_samples; worst[k].offroad = res.n_offroad; worst[k].spikes = res.n_spikes;
                worst[k].searches = res.n_searches; worst[k].jumps = res.n_jumps; worst[k].hits = res.n_hits; worst[k].truth_m = truth_m; worst[k].matched_m = res.matched_m;
                break;
            }
    }
    int64_t wall = cb_mono_us() - wall0;
    qsort(cpu, (size_t)made, sizeof *cpu, cmp_u32);
    uint64_t total = 0;
    for (int i = 0; i < made; i++) total += cpu[i];
    int ok = made - rejected;
    printf("adv-bench: %d activities on %s (%u segments), %d users, seed %llu\n", made, g.h->source, g.h->n_segs, n_users, (unsigned long long)seed);
    printf("  points %.0f per activity, %.0f samples after downsampling; %d rejected\n", (double)points / made, ok ? (double)samples / ok : 0, rejected);
    printf("  cpu per activity: mean %.0f us, p50 %u, p90 %u, p99 %u, max %u (%.1f ns per point)\n", (double)total / made, cpu[made / 2],
           cpu[made * 9 / 10], cpu[made * 99 / 100], cpu[made - 1], (double)total * 1000 / (double)points);
    printf("  accuracy: recall mean %.4f min %.4f, precision mean %.4f; matched %.1f km per activity\n", ok ? acc_sum / ok : 0, recall_min,
           ok ? prec_sum / ok : 0, ok ? (double)matched_cm / 100000 / ok : 0);
    printf("  transitions: %.2f%% arithmetic, %llu searches, %llu jumps; %.0f hits per activity\n",
           fast + searches ? 100.0 * (double)fast / (double)(fast + searches) : 0, (unsigned long long)searches, (unsigned long long)jumps,
           ok ? (double)hits / ok : 0);
    printf("  wall %.2f s for the generator and the matcher together\n", (double)wall / 1e6);
    for (int k = 0; k < 3; k++)
        if (worst[k].i >= 0)
            printf("  worst %d: ride %d type %u kind %u: recall %.3f precision %.3f, %u pts %u samples %u off-road %u spikes, %u searches %u jumps, %u hits, truth %.0f m matched %.0f m\n",
                   k + 1, worst[k].i, worst[k].type, worst[k].kind, worst[k].recall, worst[k].prec, worst[k].pts, worst[k].samples, worst[k].offroad,
                   worst[k].spikes, worst[k].searches, worst[k].jumps, worst[k].hits, worst[k].truth_m, worst[k].matched_m);
    adv_match_scratch_free(s);
    free(users); free(route); free(act); free(cpu); free(truth);
    adv_graph_close(&g);
    return 0;
}
