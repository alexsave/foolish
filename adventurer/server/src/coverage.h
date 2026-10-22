// coverage.h - WHAT A USER HAS TRAVELLED: sorted (segment, mask per type) entries, and the merge
// of one activity's hits into it, which is where new length is credited.
//
// An activity's result is its HITS: sorted unique (segment, mask) pairs, one mask (the type the
// activity was). The merge ORs each hit into the user's entry for that type and reports, per
// hit, the bits that were new for bike, for foot and for either (ADV_T_ALL), so the caller
// credits new length to the user, the region and the leaderboards without a second pass, and
// knows which segments to re-check for super-uniqueness.
#ifndef ADV_COVERAGE_H
#define ADV_COVERAGE_H

#include "graph.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t seg;
    uint16_t mask[2];   // ADV_T_BIKE, ADV_T_FOOT
} adv_cov_entry;

typedef struct {
    adv_cov_entry *e;
    uint32_t       n, cap;
} adv_cov;

typedef struct {
    uint32_t seg;
    uint16_t mask;
    uint16_t pad;
} adv_hit;

// One hit's outcome: the bits newly set in each type's view.
typedef struct {
    uint32_t seg;
    uint16_t new_mask[ADV_T_N];
    uint16_t pad;
} adv_cov_delta;

void adv_cov_init(adv_cov *c);
void adv_cov_free(adv_cov *c);
// The user's mask for a segment, by type (ADV_T_ALL: the OR); 0 when the segment is not covered.
uint16_t adv_cov_mask(const adv_cov *c, uint32_t seg, int type);
// Merges hits (sorted by seg, unique) under `type` (ADV_T_BIKE or ADV_T_FOOT), writing out[i]
// for hit i (out may be NULL) and adding the new length per type to new_m[3] (not reset: the
// caller zeroes it). CB_OK or CB_E_NOMEM (nothing merged then).
int adv_cov_merge(adv_cov *c, const adv_graph *g, const adv_hit *h, uint32_t n, int type,
                  adv_cov_delta *out, float new_m[ADV_T_N]);
// Total covered length per type, from scratch (a check; the running totals are the caller's).
void adv_cov_total_m(const adv_cov *c, const adv_graph *g, float out[ADV_T_N]);

// HITS from a matcher's or a generator's raw (seg, mask) marks, in any order with repeats:
// sort, OR the repeats, drop empty masks. The count left in h[0..n).
uint32_t adv_hits_finish(adv_hit *h, uint32_t n);
// Length a set of hits covers.
float adv_hits_len_m(const adv_graph *g, const adv_hit *h, uint32_t n);
// Hits in both lists, as length: what the generator's truth and the matcher's hits share.
float adv_hits_common_m(const adv_graph *g, const adv_hit *a, uint32_t na, const adv_hit *b, uint32_t nb);

#endif
