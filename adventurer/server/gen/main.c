// main.c - adv-gen: the fake Strava. Thousands of users (fake.h) upload activities to the server
// at a steady rate from a few threads, a backfill thread floods one user's history at a time, and
// every so often an upload is hostile. One line of counters every 5 s on stderr.
//
//   adv-gen <graph> [--server 127.0.0.1:8711] [--users 3000] [--rate 5] [--threads 4] [--seed 1]
//           [--backfill-users 20] [--backfill-size 400] [--backfill-every 60] [--hostile-every 50]
//           [--duration 0] [--once N]
//
// Every user's state (its visited set, its activity counter) is behind its own mutex: a live
// thread picks a random user with a trylock and moves on when the backfill thread holds it, so
// no thread ever waits on another for longer than one route. --once N uploads N live activities
// and exits non-zero if any failed to reach the server (the smoke test); the backfill thread is
// not started then.
#include "fake.h"
#include "httpc.h"
#include "util/clock.h"
#include "util/err.h"
#include "wire.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_THREADS 64
#define ROSTER_BATCH 4096
#define BUSY_RETRIES 5
#define BUSY_BACKOFF_MS 200

typedef struct {
    const char *graph;
    const char *server;
    uint32_t users, threads, backfill_users, backfill_size, backfill_every, hostile_every, duration, once;
    double rate;
    uint64_t seed;
} cfg_t;

static cfg_t CFG = { .server = "127.0.0.1:8711", .users = 3000, .threads = 4, .backfill_users = 20, .backfill_size = 400,
                     .backfill_every = 60, .hostile_every = 50, .rate = 5, .seed = 1 };

static adv_graph G;
static fake_user *USERS;
static pthread_mutex_t *USER_LOCKS;
static volatile sig_atomic_t STOP;

typedef struct {
    atomic_uint_fast64_t uploads, accepted, rejected, busy, errors, dropped, bytes, backfill_done, hostile, hostile_accepted, wire_bad;
} counters_t;
static counters_t C;
static atomic_uint_fast64_t TICKETS;   // --once: live uploads handed out
static atomic_uint_fast64_t SEQ;       // every live upload's number (the hostile cadence)

// Per-thread scratch, allocated once: the activity (800 KB), the route, the body, a connection.
typedef struct {
    int             index;
    fake_rng        rng;
    fake_route_out *route;
    fake_act       *act;
    uint8_t        *body;
    httpc           conn;
    pthread_t       tid;
} worker_t;

static void on_signal(int sig) {
    (void)sig;
    STOP = 1;
}

// Sleeps ms in short naps so a stop is seen within 50 ms.
static void nap_ms(uint32_t ms) {
    while (ms > 0 && !STOP) {
        uint32_t step = ms < 50 ? ms : 50;
        struct timespec ts = { step / 1000, (long)(step % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        ms -= step;
    }
}

static worker_t *worker_new(int index, uint64_t seed) {
    worker_t *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->index = index;
    fake_rng_seed(&w->rng, seed ^ ((uint64_t)(index + 1) * 0x9E3779B97F4A7C15ull));
    w->route = malloc(sizeof *w->route);
    w->act = malloc(sizeof *w->act);
    w->body = malloc(FAKE_BODY_CAP);
    if (!w->route || !w->act || !w->body || httpc_init(&w->conn, CFG.server) != CB_OK) {
        fprintf(stderr, "adv-gen: cannot set up a worker (memory, or a bad --server '%s')\n", CFG.server);
        exit(2);
    }
    return w;
}

// One upload with the 503 policy: back off and retry a few times, then drop it. True when the
// server took it (202).
static bool upload(worker_t *w, size_t len) {
    uint8_t resp[64];
    for (int tries = 0; tries <= BUSY_RETRIES; tries++) {
        int status = 0;
        size_t rlen = 0;
        atomic_fetch_add(&C.uploads, 1);
        atomic_fetch_add(&C.bytes, len);
        int rc = httpc_post(&w->conn, "/adv/upload", w->body, len, &status, resp, sizeof resp, &rlen);
        if (rc != CB_OK) {
            atomic_fetch_add(&C.errors, 1);
            return false;
        }
        if (status == 202) {
            // The answer is adv_w_accepted; a layout mismatch is counted, not fatal.
            adv_w_accepted acc;
            if (rlen != sizeof acc) atomic_fetch_add(&C.wire_bad, 1);
            else {
                memcpy(&acc, resp, sizeof acc);
                if (acc.hash != adv_wire_hash()) atomic_fetch_add(&C.wire_bad, 1);
            }
            atomic_fetch_add(&C.accepted, 1);
            return true;
        }
        if (status == 503) {
            atomic_fetch_add(&C.busy, 1);
            if (tries < BUSY_RETRIES) nap_ms(BUSY_BACKOFF_MS);
            continue;
        }
        if (status >= 400 && status < 500) atomic_fetch_add(&C.rejected, 1);
        else atomic_fetch_add(&C.errors, 1);
        return false;
    }
    atomic_fetch_add(&C.dropped, 1);
    return false;
}

// A random user whose lock is free, locked; or NULL after a few tries (the backfill holds them).
static fake_user *pick_user(worker_t *w) {
    for (int tries = 0; tries < 8; tries++) {
        uint32_t i = fake_rng_below(&w->rng, CFG.users);
        if (pthread_mutex_trylock(&USER_LOCKS[i]) == 0) return &USERS[i];
    }
    return NULL;
}

static void unlock_user(fake_user *u) { pthread_mutex_unlock(&USER_LOCKS[u->def.id - 1]); }

// Builds one live activity (hostile every hostile_every-th) for a free user and uploads it.
static void live_one(worker_t *w) {
    fake_user *u = pick_user(w);
    if (!u) return;
    uint64_t seq = atomic_fetch_add(&SEQ, 1) + 1;
    bool hostile = CFG.hostile_every > 0 && seq % CFG.hostile_every == 0;
    int64_t start_ms = cb_now_ms() - 2 * 3600 * 1000LL;   // the activity ended a while ago
    bool built;
    if (hostile) built = fake_hostile(&G, 1 + (int)fake_rng_below(&w->rng, FAKE_HOSTILE_N), u, &w->rng, start_ms, w->route, w->act);
    else built = fake_activity(&G, u, &w->rng, start_ms, 0, NULL, w->route, w->act);
    unlock_user(u);
    if (!built) return;
    size_t len = fake_act_write(w->act, w->body, FAKE_BODY_CAP);
    if (!len) return;
    len = fake_act_wire_len(w->act, len);
    if (hostile) atomic_fetch_add(&C.hostile, 1);
    if (upload(w, len) && hostile) atomic_fetch_add(&C.hostile_accepted, 1);
}

static void *live_main(void *arg) {
    worker_t *w = arg;
    // The threads share the rate: each one's interval is threads/rate seconds.
    int64_t interval_us = (int64_t)(1e6 * CFG.threads / (CFG.rate > 0 ? CFG.rate : 1));
    while (!STOP) {
        if (CFG.once && atomic_fetch_add(&TICKETS, 1) >= CFG.once) break;
        int64_t t0 = cb_mono_us();
        live_one(w);
        if (CFG.once) continue;   // the smoke runs flat out
        int64_t left = interval_us - (cb_mono_us() - t0);
        if (left > 0) nap_ms((uint32_t)(left / 1000));
    }
    httpc_close(&w->conn);
    return NULL;
}

// Backfills one user: backfill_size activities, one a day back from now, as fast as the server
// takes them. The user's lock is held per activity, never across the upload.
static void backfill_user(worker_t *w, uint32_t id) {
    fake_user *u = &USERS[id - 1];
    uint32_t done = 0;
    for (uint32_t i = 0; i < CFG.backfill_size && !STOP; i++) {
        int64_t start_ms = cb_now_ms() - (int64_t)(CFG.backfill_size - i) * 86400000LL;
        pthread_mutex_lock(&USER_LOCKS[id - 1]);
        bool built = fake_activity(&G, u, &w->rng, start_ms, ADV_TRF_BACKFILL, NULL, w->route, w->act);
        pthread_mutex_unlock(&USER_LOCKS[id - 1]);
        if (!built) break;
        size_t len = fake_act_write(w->act, w->body, FAKE_BODY_CAP);
        if (!len) break;
        if (upload(w, len)) {
            done++;
            atomic_fetch_add(&C.backfill_done, 1);
        }
    }
    fprintf(stderr, "adv-gen: backfill user %u (%s, %s): %u of %u accepted\n", id, u->def.name, fake_kind_name(u->def.kind), done, CFG.backfill_size);
}

static void *backfill_main(void *arg) {
    worker_t *w = arg;
    uint32_t started = 0;
    // Three users at once at the start, so the demo shows backfill in its first minute.
    for (int k = 0; k < 3 && started < CFG.backfill_users && !STOP; k++, started++) backfill_user(w, 1 + fake_rng_below(&w->rng, CFG.users));
    while (!STOP && started < CFG.backfill_users) {
        nap_ms(CFG.backfill_every * 1000);
        if (STOP) break;
        backfill_user(w, 1 + fake_rng_below(&w->rng, CFG.users));
        started++;
    }
    httpc_close(&w->conn);
    return NULL;
}

// POST /adv/users in batches: adv_w_list then the records. Failures are counted and reported,
// not fatal: against a server that is not up yet the uploads then 404, which the counters show.
static void post_roster(worker_t *w) {
    adv_w_user_def *defs = malloc((size_t)CFG.users * sizeof *defs);
    if (!defs) exit(2);
    fake_users_make(&G, CFG.users, CFG.seed, defs);
    size_t batch_cap = sizeof(adv_w_list) + (size_t)ROSTER_BATCH * sizeof(adv_w_user_def);
    uint8_t *buf = malloc(batch_cap);
    if (!buf) exit(2);
    uint32_t sent = 0, ok = 0;
    for (uint32_t at = 0; at < CFG.users; at += ROSTER_BATCH) {
        uint32_t n = CFG.users - at < ROSTER_BATCH ? CFG.users - at : ROSTER_BATCH;
        adv_w_list head = { adv_wire_hash(), n };
        memcpy(buf, &head, sizeof head);
        memcpy(buf + sizeof head, defs + at, (size_t)n * sizeof *defs);
        size_t len = sizeof head + (size_t)n * sizeof *defs;
        int status = 0;
        size_t rlen = 0;
        uint8_t resp[64];
        int rc = httpc_post(&w->conn, "/adv/users", buf, len, &status, resp, sizeof resp, &rlen);
        atomic_fetch_add(&C.bytes, len);
        sent += n;
        if (rc == CB_OK && status == 204) ok += n;
        else {
            atomic_fetch_add(&C.errors, 1);
            fprintf(stderr, "adv-gen: POST /adv/users (%u users): %s, status %d\n", n, rc == CB_OK ? "answered" : cb_strerror(rc), status);
        }
    }
    fprintf(stderr, "adv-gen: roster: %u of %u users posted to %s\n", ok, sent, CFG.server);
    free(buf);
    free(defs);
}

// One line of counters: the rates are over the window since the previous line.
static void report(uint32_t t_s, double window_s, const counters_t *prev, counters_t *now) {
    if (window_s < 0.001) window_s = 0.001;
#define LD(f) ((unsigned long long)atomic_load(&now->f))
#define RATE(f) ((double)(atomic_load(&now->f) - atomic_load(&prev->f)) / window_s)
    fprintf(stderr, "t=%5us  up/s %5.1f  acc/s %5.1f | accepted %llu  4xx %llu  503 %llu  err %llu  dropped %llu  hostile %llu/%llu  backfill %llu  sent %.1f MB%s\n",
            t_s, RATE(uploads), RATE(accepted), LD(accepted), LD(rejected), LD(busy), LD(errors), LD(dropped), LD(hostile_accepted), LD(hostile),
            LD(backfill_done), (double)LD(bytes) / 1048576.0, LD(wire_bad) ? "  WIRE MISMATCH" : "");
#undef LD
#undef RATE
}

static void snapshot(counters_t *dst, const counters_t *src) {
    atomic_store(&dst->uploads, atomic_load(&src->uploads));
    atomic_store(&dst->accepted, atomic_load(&src->accepted));
}

static bool arg_u32(const char *v, uint32_t *out) {
    char *end;
    errno = 0;
    unsigned long x = strtoul(v, &end, 10);
    if (errno || *end || x > 0xFFFFFFFFul) return false;
    *out = (uint32_t)x;
    return true;
}

static void usage(void) {
    fprintf(stderr, "usage: adv-gen <graph> [--server host:port] [--users N] [--rate per_s] [--threads N] [--seed N]\n"
                    "               [--backfill-users N] [--backfill-size N] [--backfill-every s] [--hostile-every N]\n"
                    "               [--duration s] [--once N]\n");
    exit(2);
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    CFG.graph = argv[1];
    for (int i = 2; i < argc; i += 2) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) usage();
        bool ok = true;
        if (!strcmp(a, "--server")) CFG.server = v;
        else if (!strcmp(a, "--users")) ok = arg_u32(v, &CFG.users) && CFG.users > 0;
        else if (!strcmp(a, "--rate")) { CFG.rate = atof(v); ok = CFG.rate > 0; }
        else if (!strcmp(a, "--threads")) ok = arg_u32(v, &CFG.threads) && CFG.threads > 0 && CFG.threads <= MAX_THREADS;
        else if (!strcmp(a, "--seed")) { uint32_t s; ok = arg_u32(v, &s); CFG.seed = s; }
        else if (!strcmp(a, "--backfill-users")) ok = arg_u32(v, &CFG.backfill_users);
        else if (!strcmp(a, "--backfill-size")) ok = arg_u32(v, &CFG.backfill_size);
        else if (!strcmp(a, "--backfill-every")) ok = arg_u32(v, &CFG.backfill_every) && CFG.backfill_every > 0;
        else if (!strcmp(a, "--hostile-every")) ok = arg_u32(v, &CFG.hostile_every);
        else if (!strcmp(a, "--duration")) ok = arg_u32(v, &CFG.duration);
        else if (!strcmp(a, "--once")) ok = arg_u32(v, &CFG.once);
        else usage();
        if (!ok) usage();
    }
    char err[256] = "";
    int rc = adv_graph_open(CFG.graph, &G, err, sizeof err);
    if (rc != CB_OK) {
        fprintf(stderr, "adv-gen: cannot open graph %s: %s (%s)\n", CFG.graph, cb_strerror(rc), err);
        return 2;
    }
    fprintf(stderr, "adv-gen: graph %s: %u segments, %u nodes, %.0f km; %u users, %.1f uploads/s on %u threads, seed %llu\n",
            CFG.graph, G.h->n_segs, G.h->n_nodes, G.h->total_len_m[ADV_T_ALL] / 1000, CFG.users, CFG.rate, CFG.threads,
            (unsigned long long)CFG.seed);
    USERS = malloc((size_t)CFG.users * sizeof *USERS);
    USER_LOCKS = malloc((size_t)CFG.users * sizeof *USER_LOCKS);
    if (!USERS || !USER_LOCKS) {
        fprintf(stderr, "adv-gen: out of memory for %u users\n", CFG.users);
        return 2;
    }
    for (uint32_t i = 0; i < CFG.users; i++) {
        fake_user_init(&G, &USERS[i], CFG.seed, i + 1);
        pthread_mutex_init(&USER_LOCKS[i], NULL);
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    worker_t *live[MAX_THREADS];
    for (uint32_t t = 0; t < CFG.threads; t++) live[t] = worker_new((int)t, CFG.seed);
    worker_t *bf = worker_new(MAX_THREADS, CFG.seed);
    post_roster(live[0]);
    for (uint32_t t = 0; t < CFG.threads; t++) pthread_create(&live[t]->tid, NULL, live_main, live[t]);
    bool backfill = !CFG.once && CFG.backfill_users > 0 && CFG.backfill_size > 0;
    if (backfill) pthread_create(&bf->tid, NULL, backfill_main, bf);

    counters_t prev;
    memset(&prev, 0, sizeof prev);
    int64_t t_start = cb_mono_us(), next_report = t_start + 5000000, last_report = t_start;
    while (!STOP) {
        if (CFG.once) {
            // Done when every live thread has run out of tickets and finished.
            bool alive = false;
            if (atomic_load(&TICKETS) < CFG.once) alive = true;
            if (!alive) break;
        }
        if (CFG.duration && cb_mono_us() - t_start >= (int64_t)CFG.duration * 1000000) break;
        nap_ms(50);
        if (cb_mono_us() >= next_report) {
            int64_t now = cb_mono_us();
            report((uint32_t)((now - t_start) / 1000000), (double)(now - last_report) / 1e6, &prev, &C);
            snapshot(&prev, &C);
            last_report = now;
            next_report += 5000000;
        }
    }
    STOP = 1;
    for (uint32_t t = 0; t < CFG.threads; t++) pthread_join(live[t]->tid, NULL);
    if (backfill) pthread_join(bf->tid, NULL);
    int64_t t_end = cb_mono_us();
    report((uint32_t)((t_end - t_start) / 1000000), (double)(t_end - last_report) / 1e6, &prev, &C);
    uint64_t errors = atomic_load(&C.errors), dropped = atomic_load(&C.dropped);
    adv_graph_close(&G);
    return CFG.once && (errors || dropped) ? 1 : 0;
}
