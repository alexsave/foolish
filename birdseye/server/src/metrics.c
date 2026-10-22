// metrics.c - metrics.h.
#include "metrics.h"

#include "util/clock.h"

#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach.h>
#include <sys/resource.h>
#else
#include <stdio.h>
#endif

be_metrics be_m;

// Bucket of a microsecond value: 4 per octave, so bucket = 4*log2(us) rounded down over the
// quarter-octave grid; 0 and 1 us land in bucket 0.
static int bucket_of(uint64_t us) {
    if (us < 2) return 0;
    int lg = 63 - __builtin_clzll(us);                 // floor(log2)
    uint64_t frac = (us << 2) >> lg;                   // 4..7: the quarter within the octave
    int b = lg * 4 + (int)(frac & 3);
    return b >= BE_HIST_BUCKETS ? BE_HIST_BUCKETS - 1 : b;
}

static uint32_t bucket_floor(int b) {
    int lg = b / 4, q = b % 4;
    uint64_t v = ((uint64_t)(4 + q) << lg) >> 2;
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

void be_hist_add(be_hist *h, uint64_t us) {
    atomic_fetch_add_explicit(&h->b[bucket_of(us)], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->n, 1, memory_order_relaxed);
    uint64_t m = atomic_load_explicit(&h->max, memory_order_relaxed);
    while (us > m && !atomic_compare_exchange_weak_explicit(&h->max, &m, us, memory_order_relaxed, memory_order_relaxed)) {}
}

uint32_t be_hist_pct(const be_hist *h, double pct) {
    uint64_t n = atomic_load_explicit(&h->n, memory_order_relaxed);
    if (!n) return 0;
    uint64_t want = (uint64_t)((double)n * pct / 100.0 + 0.5);
    if (want < 1) want = 1;
    if (want > n) want = n;
    uint64_t seen = 0;
    for (int b = 0; b < BE_HIST_BUCKETS; b++) {
        seen += atomic_load_explicit(&h->b[b], memory_order_relaxed);
        if (seen >= want) return bucket_floor(b);
    }
    return bucket_floor(BE_HIST_BUCKETS - 1);
}

void be_rate_add(be_rate *r, uint64_t n) {
    int64_t sec = cb_now_ms() / 1000;
    int i = (int)(sec % BE_RATE_SLOTS);
    if (atomic_load_explicit(&r->sec[i], memory_order_relaxed) != sec) {
        // A new second in this slot: the previous occupant was BE_RATE_SLOTS seconds ago. A race
        // between two writers at the second's edge loses at most a few counts of one second.
        atomic_store_explicit(&r->count[i], 0, memory_order_relaxed);
        atomic_store_explicit(&r->sec[i], sec, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&r->count[i], n, memory_order_relaxed);
}

float be_rate_per_s(const be_rate *r) {
    int64_t sec = cb_now_ms() / 1000;
    uint64_t total = 0;
    int whole = 0;
    for (int k = 1; k < BE_RATE_SLOTS; k++) {
        int64_t s = sec - k;
        int i = (int)(s % BE_RATE_SLOTS);
        if (atomic_load_explicit(&r->sec[i], memory_order_relaxed) != s) continue;
        total += atomic_load_explicit(&r->count[i], memory_order_relaxed);
        whole++;
    }
    if (whole == 0) return 0;
    return (float)total / (float)(BE_RATE_SLOTS - 1);
}

uint32_t be_rss_mb(int pid) {
#ifdef __APPLE__
    if (pid == 0) {
        struct mach_task_basic_info info;
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
        return (uint32_t)(info.resident_size >> 20);
    }
    struct proc_taskinfo ti;
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti) != (int)sizeof ti) return 0;
    return (uint32_t)(ti.pti_resident_size >> 20);
#else
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/statm", pid == 0 ? (int)getpid() : pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    int ok = fscanf(f, "%lu %lu", &size, &resident) == 2;
    fclose(f);
    if (!ok) return 0;
    return (uint32_t)((resident * (unsigned long)sysconf(_SC_PAGESIZE)) >> 20);
#endif
}

uint64_t be_cpu_us(int pid) {
#ifdef __APPLE__
    if (pid == 0) {
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
        return (uint64_t)ru.ru_utime.tv_sec * 1000000 + (uint64_t)ru.ru_utime.tv_usec + (uint64_t)ru.ru_stime.tv_sec * 1000000 +
               (uint64_t)ru.ru_stime.tv_usec;
    }
    struct proc_taskinfo ti;
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti) != (int)sizeof ti) return 0;
    // Mach absolute time units on Apple silicon are nanoseconds (timebase 1/1).
    return (ti.pti_total_user + ti.pti_total_system) / 1000;
#else
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid == 0 ? (int)getpid() : pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    unsigned long ut = 0, st = 0;
    // After the comm: state, ppid, pgrp, session, tty, tpgid, flags, minflt, cminflt, majflt, cmajflt, utime, stime
    if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) return 0;
    long hz = sysconf(_SC_CLK_TCK);
    return (uint64_t)(ut + st) * 1000000 / (uint64_t)(hz > 0 ? hz : 100);
#endif
}
