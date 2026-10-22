// import_main.c - adv-import: an OSM PBF extract in, the road graph out, the numbers on stderr.
//
//   adv-import <in.osm.pbf> <out.graph> [--admin-level N] [--region-name NAME]
#include "graph.h"
#include "import.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static const char *hc_names[ADV_HC_N] = {
    "motorway", "trunk", "primary", "secondary", "tertiary", "residential", "unclassified", "service",
    "living_street", "track", "cycleway", "footway", "path", "pedestrian", "steps", "other",
};

static int usage(void) {
    fprintf(stderr, "usage: adv-import <in.osm.pbf> <out.graph> [--admin-level N] [--region-name NAME]\n");
    return 1;
}

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL;
    adv_import_opts o = { 0 };
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--admin-level") == 0 && i + 1 < argc) o.admin_level = atoi(argv[++i]);
        else if (strcmp(argv[i], "--region-name") == 0 && i + 1 < argc) o.fallback_region = argv[++i];
        else if (argv[i][0] == '-') return usage();
        else if (!in) in = argv[i];
        else if (!out) out = argv[i];
        else return usage();
    }
    if (!in || !out) return usage();
    adv_import_stats st;
    char err[512];
    int rc = adv_import(in, out, &o, &st, err, sizeof err);
    if (rc != CB_OK) {
        fprintf(stderr, "adv-import: %s (%s)\n", err, cb_strerror(rc));
        return 1;
    }
    fprintf(stderr, "adv-import: %s -> %s\n", in, out);
    fprintf(stderr, "  pass 1 %.2fs: %llu ways read, %llu roads, %llu boundary-only ways, %llu relations read, %llu wanted\n",
            st.s_pass1, (unsigned long long)st.ways_read, (unsigned long long)st.roads_kept, (unsigned long long)st.member_ways,
            (unsigned long long)st.relations_read, (unsigned long long)st.relations_wanted);
    fprintf(stderr, "  pass 2 %.2fs: %llu nodes read, %llu needed, %llu kept\n", st.s_pass2, (unsigned long long)st.nodes_read,
            (unsigned long long)st.nodes_needed, (unsigned long long)st.nodes_kept);
    fprintf(stderr, "  build  %.2fs: %llu roads dropped (a node missing), rings %llu closed %llu dropped%s\n", st.s_build,
            (unsigned long long)st.ways_dropped, (unsigned long long)st.rings_closed, (unsigned long long)st.rings_dropped,
            st.fallback_used ? ", fallback region" : "");
    fprintf(stderr, "  graph: %u segments, %u nodes, %u deltas, %u cells (%u entries), %u regions\n", st.build.segs, st.build.nodes,
            st.build.deltas, st.build.cells, st.build.cell_segs, st.build.regions);
    fprintf(stderr, "  km: bike %.1f, foot %.1f, all %.1f\n", st.build.total_len_m[ADV_T_BIKE] / 1000, st.build.total_len_m[ADV_T_FOOT] / 1000,
            st.build.total_len_m[ADV_T_ALL] / 1000);
    // Open what was written (the check a server will run) and sum it by class and region.
    adv_graph g;
    if ((rc = adv_graph_open(out, &g, err, sizeof err)) != CB_OK) {
        fprintf(stderr, "adv-import: the graph does not open: %s (%s)\n", err, cb_strerror(rc));
        return 1;
    }
    double by_class[ADV_HC_N] = { 0 };
    for (uint32_t i = 0; i < g.h->n_segs; i++) by_class[g.segs[i].hclass] += g.segs[i].len_m;
    fprintf(stderr, "  by class:");
    for (int c = 0; c < ADV_HC_N; c++)
        if (by_class[c] > 0) fprintf(stderr, " %s %.1f", hc_names[c], by_class[c] / 1000);
    fprintf(stderr, " (km)\n");
    fprintf(stderr, "  regions (%u):", g.h->n_regions);
    for (uint32_t i = 0; i < g.h->n_regions; i++)
        fprintf(stderr, "%s %s %.1f", i ? "," : "", g.regions[i].name, g.regions[i].len_m[ADV_T_ALL] / 1000);
    fprintf(stderr, " (km)\n");
    fprintf(stderr, "  file: %llu bytes\n", (unsigned long long)g.h->file_size);
    adv_graph_close(&g);
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
#ifdef __APPLE__
        fprintf(stderr, "  peak rss: %.1f MB\n", ru.ru_maxrss / 1048576.0);
#else
        fprintf(stderr, "  peak rss: %.1f MB\n", ru.ru_maxrss / 1024.0);
#endif
    }
    return 0;
}
