// metrics.h - the server's own numbers: fixed log-scale histograms (quarter-octave buckets of
// microseconds, so a p99 is read in one pass with no sorting and no allocation) and the last ten
// seconds' rates. Every counter is a relaxed atomic: the loop, the matcher workers and the tile
// renders all write, the ops frame reads.
#ifndef ADV_METRICS_H
#define ADV_METRICS_H

#include <stdatomic.h>
#include <stdint.h>

#define ADV_HIST_BUCKETS 128   // 4 per octave: 1 us .. 2^32 us
#define ADV_RATE_SLOTS   10    // one per second

typedef struct {
    atomic_ullong b[ADV_HIST_BUCKETS];
    atomic_ullong n;
} adv_hist;

void     adv_hist_add(adv_hist *h, uint64_t us);
// The value at the given percentile (0..100): the lower bound of the bucket it falls in.
uint32_t adv_hist_pct(const adv_hist *h, double pct);

// A per-second ring: add counts events at `now_ms`; rate answers events/s over the last 10 whole
// seconds (the current, partial second excluded).
typedef struct {
    atomic_ullong count[ADV_RATE_SLOTS];
    atomic_llong  sec[ADV_RATE_SLOTS];
} adv_rate;
void  adv_rate_add(adv_rate *r, int64_t now_ms, uint64_t n);
float adv_rate_per_s(const adv_rate *r, int64_t now_ms);

typedef struct {
    adv_hist act_cpu, tile, e2e;
    adv_rate acts, tiles;
    atomic_ullong tiles_total, tiles_304;
    atomic_uint   feed_conns;
} adv_metrics;

extern adv_metrics adv_m;

// The process's resident set now, in MB (0 where it cannot be read).
uint32_t adv_rss_mb(void);

#endif
