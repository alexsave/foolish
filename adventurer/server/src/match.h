// match.h - THE MAP MATCHER: a GPS trace in, the segments and sixteenths it travelled out.
//
// 1. Downsample the 1 Hz points to about opts.sample_m apart (a day-long ride still fits the
//    sample cap: the spacing widens when it would not), dropping repeats and marking gaps; a
//    sample whose implied speed is beyond the type's ceiling is a spike and is dropped.
// 2. For each sample, the candidate segments within opts.cand_radius_m (the cell and its eight
//    neighbours, graph.h), at most ADV_MATCH_K of them, each with its projection and distance.
//    A sample with none is off-road (a state of its own, so a trail through a park neither
//    breaks the chain nor snaps to the nearest street).
// 3. Viterbi over the candidates: emission is the projection distance; transition is how far the
//    route distance between two consecutive projections strays from the straight-line distance
//    between the samples. On the same segment or two that share a node the route distance is
//    arithmetic (the fast path, the great majority of transitions); otherwise a bounded local
//    search over the nodes (at most ADV_MATCH_SEARCH expansions) and, past that, a jump penalty.
// 4. The best path becomes hits: the sixteenths between consecutive projections, every segment
//    a local route passed through whole.
//
// BOUNDS. Every loop here is bounded by a constant or by the sample cap, and the whole run by
// opts.cpu_budget_us of thread CPU, checked every ADV_MATCH_BUDGET_EVERY samples: a trace that
// cannot be matched in the budget is refused, never served slowly. The scratch is allocated
// once per thread and sized for the caps, so a match allocates nothing.
#ifndef ADV_MATCH_H
#define ADV_MATCH_H

#include "coverage.h"
#include "graph.h"
#include "trace.h"

#include <stdint.h>

#define ADV_MATCH_MAX_SAMPLES 8192   // after downsampling
#define ADV_MATCH_K           8      // candidates per sample
#define ADV_MATCH_SEARCH      96     // node expansions one local search may make
#define ADV_MATCH_BUDGET_EVERY 64
#define ADV_MATCH_MAX_HITS    (ADV_MATCH_MAX_SAMPLES * 4)

typedef struct {
    int    cpu_budget_us;    // 0: 200000
    double cand_radius_m;    // 0: 35
    double sample_m;         // 0: 15
    double max_speed_mps;    // 0: by type (bike 30, foot 12)
} adv_match_opts;

// Why a trace was refused (adv_match_result.rc > 0; 0 is matched).
enum {
    ADV_MATCH_OK = 0,
    ADV_MATCH_RJ_SHORT,       // under 2 usable points, or under 100 m end to end
    ADV_MATCH_RJ_COORDS,      // a point outside the planet, or every point outside the graph
    ADV_MATCH_RJ_TIME,        // time runs backwards, or a point more than a day after the start
    ADV_MATCH_RJ_TELEPORT,    // most of the samples are spikes: the trace is noise
    ADV_MATCH_RJ_BUDGET,      // the CPU budget ran out
    ADV_MATCH_RJ_N
};
const char *adv_match_reason(int rc);

typedef struct {
    int       rc;           // ADV_MATCH_*
    adv_hit  *hits;         // into the scratch: valid until the next match on it
    uint32_t  n_hits;
    float     matched_m;    // length the hits cover
    float     trace_m;      // the samples' straight-line length
    uint32_t  n_points;     // as given
    uint32_t  n_samples;    // kept after downsampling
    uint32_t  n_offroad;    // samples with no candidate
    uint32_t  n_spikes;     // points dropped as spikes
    uint32_t  n_searches;   // local searches run (the slow path)
    uint32_t  cpu_us;       // thread CPU the match took
} adv_match_result;

typedef struct adv_match_scratch adv_match_scratch;
adv_match_scratch *adv_match_scratch_new(void);   // NULL: no memory
void adv_match_scratch_free(adv_match_scratch *s);

// Matches pts[0..n) (type ADV_ACT_*) on g into out. Always returns: out->rc says whether the
// hits mean anything. opts NULL: the defaults.
void adv_match(const adv_graph *g, const adv_trace_pt *pts, uint32_t n, uint8_t type, const adv_match_opts *opts,
               adv_match_scratch *s, adv_match_result *out);

#endif
