// trace.h - an ACTIVITY UPLOAD: the one untrusted input of the server (POST /adv/upload, body
// application/octet-stream). The fake generator writes it; a Strava connector would too.
//
//   adv_trace_hdr (32 bytes)
//   n_points x adv_trace_pt (12 bytes each)
//   if ADV_TRF_TRUTH: u32 n_true, then n_true x adv_truth (8 bytes each), sorted by seg, unique:
//      the coverage the generator KNOWS it produced, so the server can score the matcher. A real
//      upload never has one; the flag is the proof-of-concept's own.
//
// Every field is read through adv_trace_parse, which checks every count against the body it was
// given: nothing downstream reads the body again.
#ifndef ADV_TRACE_H
#define ADV_TRACE_H

#include <stddef.h>
#include <stdint.h>

#define ADV_TRACE_MAGIC   0x43525441u   // "ATRC"
#define ADV_TRACE_VERSION 1
#define ADV_TRACE_MAX_PTS 65535         // one upload; a day-long 1 Hz ride is 43200
#define ADV_TRACE_MAX_TRUTH 65535

// Activity types as the upload spells them (ADV_T_* of graph.h is the per-type index).
enum { ADV_ACT_BIKE = 1, ADV_ACT_RUN = 2, ADV_ACT_WALK = 3 };
#define ADV_TRF_BACKFILL 0x1   // history, processed behind live uploads
#define ADV_TRF_TRUTH    0x2   // a truth trailer follows the points

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t  type;       // ADV_ACT_*
    uint8_t  flags;      // ADV_TRF_*
    uint32_t user;       // the user's id (users.h)
    uint32_t n_points;
    int64_t  start_ms;   // the activity's start, epoch ms (t_ms counts from it)
    uint32_t gen_id;     // the generator's own number for the activity (echoed in the feed)
    uint32_t reserved;
} adv_trace_hdr;

typedef struct {
    int32_t  lat, lon;   // geo.h units
    uint32_t t_ms;       // since start_ms, non-decreasing
} adv_trace_pt;

typedef struct {
    uint32_t seg;
    uint16_t mask;
    uint16_t pad;
} adv_truth;

typedef struct {
    adv_trace_hdr       hdr;      // copied out (the body may be unaligned)
    const adv_trace_pt *pts;      // into the body; read with adv_trace_pt_at (unaligned-safe)
    const adv_truth    *truth;    // into the body, or NULL
    uint32_t            n_truth;
} adv_trace;

// Reads the header and bounds every section: CB_OK, or CB_E_INVALID (bad magic or version, a
// type that is none, a count that does not fit the body, a body with bytes left over, a truth
// list out of order).
int adv_trace_parse(const uint8_t *body, size_t len, adv_trace *out);
// Point i, copied out (the body is not aligned).
adv_trace_pt adv_trace_pt_at(const adv_trace *t, uint32_t i);
adv_truth    adv_trace_truth_at(const adv_trace *t, uint32_t i);
// The per-type index (graph.h ADV_T_*) of an upload's type.
static inline int adv_type_index(uint8_t act) { return act == ADV_ACT_BIKE ? 0 : 1; }

// The writer, for the generator and the tests: the body's size for n points and n_true truth
// entries, and the bytes into out (cap >= that size). The size written, or 0 when it does not fit.
size_t adv_trace_size(uint32_t n_points, uint32_t n_truth);
size_t adv_trace_write(uint8_t *out, size_t cap, const adv_trace_hdr *h, const adv_trace_pt *pts,
                       const adv_truth *truth, uint32_t n_truth);

#endif
