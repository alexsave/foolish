// unit_metrics.c - the histogram's buckets and percentiles, the rate ring, the process readers.
// TEMPORARY with metrics.h: goes when the lift into c_base lands.
#include "metrics.h"
#include "t.h"
#include "util/clock.h"

#include <unistd.h>

static void hist_percentiles(void) {
    be_hist h;
    memset(&h, 0, sizeof h);
    T_EQ_INT(be_hist_pct(&h, 50), 0);
    // 100 samples of 100 us and one of 10 ms: the median is in 100's bucket, the max is exact.
    for (int i = 0; i < 100; i++) be_hist_add(&h, 100);
    be_hist_add(&h, 10000);
    uint32_t p50 = be_hist_pct(&h, 50), p99 = be_hist_pct(&h, 99), p100 = be_hist_pct(&h, 100);
    T_CHECK(p50 >= 64 && p50 <= 100);        // the quarter-octave bucket 100 falls in starts at 96
    T_CHECK(p99 >= 64 && p99 <= 100);        // 99% of 101 samples is the 100th: still a 100
    T_CHECK(p100 >= 8192 && p100 <= 10000);  // the last sample's bucket starts at 8192
    T_EQ_INT((long long)h.max, 10000);
    T_EQ_INT((long long)h.n, 101);
    // Bucket floors never exceed the value they hold: a value lands at or above its bucket's floor.
    for (uint64_t v = 1; v < 1u << 20; v = v * 3 + 1) {
        be_hist h2;
        memset(&h2, 0, sizeof h2);
        be_hist_add(&h2, v);
        uint32_t f = be_hist_pct(&h2, 50);
        T_CHECK(f <= v && (uint64_t)f * 2 > v);   // within one octave below
    }
}

static void rate_ring(void) {
    be_rate r;
    memset(&r, 0, sizeof r);
    T_CHECK(be_rate_per_s(&r) == 0);
    // Events counted in the current second are excluded (partial); the previous whole seconds count.
    be_rate_add(&r, 5);
    T_CHECK(be_rate_per_s(&r) == 0);
    // Five events a whole second ago, nine seconds ago, and a stale slot from long ago: the window
    // is the last BE_RATE_SLOTS - 1 whole seconds, so 10 events over 9 seconds, the stale one unseen.
    memset(&r, 0, sizeof r);
    int64_t sec = cb_now_ms() / 1000;
    int64_t a = sec - 1, b = sec - (BE_RATE_SLOTS - 1), old = sec - 50;
    r.sec[a % BE_RATE_SLOTS] = a;
    r.count[a % BE_RATE_SLOTS] = 5;
    r.sec[b % BE_RATE_SLOTS] = b;
    r.count[b % BE_RATE_SLOTS] = 5;
    r.sec[old % BE_RATE_SLOTS] = old;
    r.count[old % BE_RATE_SLOTS] = 100;
    float per_s = be_rate_per_s(&r);
    T_CHECK(per_s > 10.0f / (BE_RATE_SLOTS - 1) - 0.01f && per_s < 10.0f / (BE_RATE_SLOTS - 1) + 0.01f);
}

static void process_readers(void) {
    T_CHECK(be_rss_mb(0) > 0);
    T_CHECK(be_rss_mb((int)getpid()) > 0);
    // Burn a little CPU so the counter is not zero.
    volatile double x = 0;
    for (int i = 0; i < 2000000; i++) x += 1.0 / (i + 1);
    T_CHECK(be_cpu_us(0) > 0);
    T_CHECK(be_cpu_us((int)getpid()) > 0);
    T_CHECK(be_rss_mb(999999999) == 0);
}

int main(void) {
    T_RUN(hist_percentiles);
    T_RUN(rate_ring);
    T_RUN(process_readers);
    return T_DONE();
}
