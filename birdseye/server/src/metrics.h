// metrics.h - the sim's own numbers: fixed log-scale histograms (four buckets per octave of
// microseconds, so a percentile is read in one pass with no sorting and no allocation) and
// per-second rates. Every counter is a relaxed atomic: the player threads write, the ops frame
// reads once a second.
//
// TEMPORARY, TO BE DELETED: the histogram, the rate ring, the RSS and CPU readers and the ops
// snapshot are being lifted into c_base as a declared table of named metrics (branch
// cbase-app-lifts). Once that lands, birdseye keeps only its list of metric names and the calls
// that bump them, and everything in this file and metrics.c that is not a name goes.
#ifndef BE_METRICS_H
#define BE_METRICS_H

#include <stdatomic.h>
#include <stdint.h>

#define BE_HIST_BUCKETS 128   // 4 per octave: 1 us .. 2^32 us
#define BE_RATE_SLOTS   10    // one per second

typedef struct {
    atomic_ullong b[BE_HIST_BUCKETS];
    atomic_ullong n;
    atomic_ullong max;
} be_hist;

void     be_hist_add(be_hist *h, uint64_t us);
// The value at `pct` (0..100): the lower bound of the bucket it falls in; 0 for an empty one.
uint32_t be_hist_pct(const be_hist *h, double pct);

// A per-second ring on the wall clock: add counts `n` events now; per_s answers events/s over the
// last whole seconds (the current, partial second excluded; BE_RATE_SLOTS - 1 of them).
typedef struct {
    atomic_ullong count[BE_RATE_SLOTS];
    atomic_llong  sec[BE_RATE_SLOTS];
} be_rate;
void  be_rate_add(be_rate *r, uint64_t n);
float be_rate_per_s(const be_rate *r);

typedef struct {
    be_hist act, push, cast, req, game_s;
    be_rate games, moves, pushes, push_bytes, reqs;
    atomic_ullong n_moves, n_refused, n_hostile, n_hostile_accepted, n_resyncs, n_stalls, n_games;
} be_metrics;

extern be_metrics be_m;

// A process's resident set now, in MB (0 where it cannot be read): `pid` 0 is this process.
uint32_t be_rss_mb(int pid);
// A process's CPU time so far (user + system), in microseconds; 0 when unknown.
uint64_t be_cpu_us(int pid);

#endif
