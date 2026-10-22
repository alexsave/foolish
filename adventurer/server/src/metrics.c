// metrics.c - metrics.h.
#include "metrics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

adv_metrics adv_m;

// Bucket of a value: 4 per octave. 0 and 1 go to bucket 0.
static int bucket_of(uint64_t us) {
    if (us < 2) return 0;
    int lg = 63 - __builtin_clzll(us);                     // floor(log2)
    uint64_t frac = (us - (1ull << lg)) * 4 / (1ull << lg); // 0..3 within the octave
    int b = lg * 4 + (int)frac;
    return b >= ADV_HIST_BUCKETS ? ADV_HIST_BUCKETS - 1 : b;
}

static uint32_t bucket_low(int b) {
    int lg = b / 4, frac = b % 4;
    double v = pow(2.0, lg) * (1.0 + frac / 4.0);
    return v > 4294967295.0 ? 4294967295u : (uint32_t)v;
}

void adv_hist_add(adv_hist *h, uint64_t us) {
    atomic_fetch_add_explicit(&h->b[bucket_of(us)], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->n, 1, memory_order_relaxed);
}

uint32_t adv_hist_pct(const adv_hist *h, double pct) {
    uint64_t n = atomic_load_explicit(&h->n, memory_order_relaxed);
    if (!n) return 0;
    uint64_t want = (uint64_t)ceil(n * pct / 100.0);
    if (want < 1) want = 1;
    uint64_t acc = 0;
    for (int b = 0; b < ADV_HIST_BUCKETS; b++) {
        acc += atomic_load_explicit(&h->b[b], memory_order_relaxed);
        if (acc >= want) return bucket_low(b);
    }
    return bucket_low(ADV_HIST_BUCKETS - 1);
}

void adv_rate_add(adv_rate *r, int64_t now_ms, uint64_t n) {
    int64_t s = now_ms / 1000;
    int i = (int)(s % ADV_RATE_SLOTS);
    int64_t have = atomic_load_explicit(&r->sec[i], memory_order_relaxed);
    if (have != s) {
        // A new second in this slot: whoever gets here first resets it (a lost count at the
        // boundary is a display rounding, not a fact anyone acts on).
        if (atomic_compare_exchange_strong(&r->sec[i], &have, s)) atomic_store_explicit(&r->count[i], 0, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&r->count[i], n, memory_order_relaxed);
}

float adv_rate_per_s(const adv_rate *r, int64_t now_ms) {
    int64_t s = now_ms / 1000;
    uint64_t total = 0;
    int secs = 0;
    for (int k = 1; k <= ADV_RATE_SLOTS - 1; k++) {   // the last 9 whole seconds
        int64_t want = s - k;
        int i = (int)(want % ADV_RATE_SLOTS);
        if (atomic_load_explicit(&r->sec[i], memory_order_relaxed) == want) total += atomic_load_explicit(&r->count[i], memory_order_relaxed);
        secs++;
    }
    return secs ? (float)total / secs : 0;
}

uint32_t adv_rss_mb(void) {
#ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return (uint32_t)(info.resident_size >> 20);
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, rss = 0;
    int ok = fscanf(f, "%lu %lu", &size, &rss) == 2;
    fclose(f);
    return ok ? (uint32_t)(rss * 4096 >> 20) : 0;
#endif
}
