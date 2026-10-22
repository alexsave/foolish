// import_main.c - adv-import: an OSM PBF extract in, the road graph out, the numbers on stderr.
//
//   adv-import <in.osm.pbf> <out.graph> [--admin-levels 7,8] [--within 6:NAME,NAME...] [--region-name NAME]
//
// --admin-levels: the OSM admin_levels whose boundaries become regions (default 8); a road
// belongs to the innermost. --within LEVEL:NAMES: keep only the regions, and the roads, inside the
// boundaries at LEVEL named (comma-separated), so a bbox extract cut with a margin around nine
// counties imports exactly those counties. --region-name: the one region made when no boundary
// closes (default: the file's basename).
#include "graph.h"
#include "import.h"
#include "util/err.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static const char *hc_names[ADV_HC_N] = {
    "motorway", "trunk", "primary", "secondary", "tertiary", "residential", "unclassified", "service",
    "living_street", "track", "cycleway", "footway", "path", "pedestrian", "steps", "other",
};

static int usage(void) {
    fprintf(stderr, "usage: adv-import <in.osm.pbf> <out.graph> [--admin-levels 7,8] [--within LEVEL:NAME,NAME...] [--region-name NAME]\n");
    return 1;
}

// "7,8" -> bits 7 and 8; false for anything else.
static bool parse_levels(const char *s, uint32_t *out) {
    uint32_t mask = 0;
    while (*s) {
        char *end;
        long l = strtol(s, &end, 10);
        if (end == s || l < 1 || l > ADV_REGION_MAX_LEVEL) return false;
        mask |= 1u << l;
        s = end;
        if (*s == ',') s++;
        else if (*s) return false;
    }
    *out = mask;
    return mask != 0;
}

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL;
    adv_import_opts o = { 0 };
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--admin-levels") == 0 || strcmp(argv[i], "--admin-level") == 0) && i + 1 < argc) {
            if (!parse_levels(argv[++i], &o.admin_levels)) return usage();
        } else if (strcmp(argv[i], "--within") == 0 && i + 1 < argc) {
            char *end;
            long l = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != ':' || l < 1 || l > ADV_REGION_MAX_LEVEL || !end[1]) return usage();
            o.within_level = (int)l;
            o.within_names = end + 1;
        } else if (strcmp(argv[i], "--region-name") == 0 && i + 1 < argc) o.fallback_region = argv[++i];
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
    fprintf(stderr, "  build  %.2fs: %llu roads dropped (a node missing), rings %llu closed %llu dropped%s", st.s_build,
            (unsigned long long)st.ways_dropped, (unsigned long long)st.rings_closed, (unsigned long long)st.rings_dropped,
            st.fallback_used ? ", fallback region" : "");
    if (o.within_level)
        fprintf(stderr, ", fence of %llu boundaries: %llu regions outside it, %u segments outside every region", (unsigned long long)st.fences,
                (unsigned long long)st.regions_outside, st.build.segs_unregioned);
    if (st.build.regions_empty) fprintf(stderr, ", %u regions with no road left out", st.build.regions_empty);
    fprintf(stderr, "\n  graph: %u segments, %u nodes, %u deltas, %u cells (%u entries), %u regions\n", st.build.segs, st.build.nodes,
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
        fprintf(stderr, "%s %s (%u) %.1f", i ? "," : "", g.regions[i].name, g.regions[i].level, g.regions[i].len_m[ADV_T_ALL] / 1000);
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
