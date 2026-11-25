// listbench.c - ns per move of the journal's per-move calls (acquire, commit, release), on the
// tree whose journal.c copy is included (JOURNAL_C). Scratch, never committed.
#include JOURNAL_C   // the journal itself, so the touch loop can reach ov_touch (static)

#include "store/store.h"
#include "awire.h"
#include "card.h"
#include "rng.h"

#include <mach/mach_time.h>
#include <execinfo.h>
#include <signal.h>
static void on_segv(int sig) { void *f[64]; int n = backtrace(f, 64); backtrace_symbols_fd(f, n, 2); _exit(139); }
#include <sys/stat.h>

#define LB_SEED       0x5eedULL   // THE ONE SEED: every draw below comes from it (rng.h)
#define NG            4500        // resident games
#define NA            192         // active games
#define ROUNDS        75          // rounds of the active games between flushes (a game waits ~75 moves)
#define STATE_BYTES   600
#define ROSTER_BYTES  96
#define LATER_IN_8    1           // touches stamped "later" by the other stamp source, out of 8
#define MOVES_PER_MS  64          // the pass clock advances 1 ms every this many clock reads

static uint64_t g_clock_rng;      // the clock's own stream (its draws differ between trees)
static int64_t  g_pass = 1000000000LL;
static long     g_reads;
int64_t lb_clock(void) {
    if (++g_reads % MOVES_PER_MS == 0) g_pass++;
    return g_pass + ((rng_u32(&g_clock_rng) & 7) < LATER_IN_8 ? 1 : 0);
}

static const char UID[] = "aaaaaaaa-0000-4000-8000-000000000001";
static uint8_t g_state[STATE_BYTES], g_roster[ROSTER_BYTES], g_chunk[24];

static int got_version(void *ud, const cb_game_row *row, const struct TableResident *res) {
    *(int64_t *)ud = row->version;
    return CB_OK;
}

// Recovery's replay: never called (the bench starts with no segments).
static int no_apply(void *ud, const cb_game_row *row, const struct cb_jmove *m, cb_game_row *out, uint8_t **log, size_t *log_len) {
    return CB_E_STATE;
}

static double ticks_ns;
static int64_t now_ns(void) { return (int64_t)((double)mach_absolute_time() * ticks_ns); }

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    const int batches = argc > 1 ? atoi(argv[1]) : 12;
    const char *dir = argc > 2 ? argv[2] : "/tmp/listbench";
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    ticks_ns = (double)tb.numer / tb.denom;
    uint64_t rng = rng_seed64(LB_SEED);
    g_clock_rng = rng_seed64(LB_SEED ^ 0xc10c);
    for (size_t i = 0; i < sizeof g_state; i++) g_state[i] = (uint8_t)rng_u32(&rng);
    for (size_t i = 0; i < sizeof g_roster; i++) g_roster[i] = (uint8_t)rng_u32(&rng);
    for (size_t i = 0; i < sizeof g_chunk; i++) g_chunk[i] = (uint8_t)rng_u32(&rng);
    signal(SIGSEGV, on_segv);
    signal(SIGBUS, on_segv);
    fprintf(stderr, "phase start\n");
    mkdir(dir, 0700);
    char path[512];
    snprintf(path, sizeof path, "%s/db.sqlite", dir);
    cb_store *s = NULL;
    if (cb_store_open_sqlite(path, &s) != CB_OK) { fprintf(stderr, "no store\n"); return 1; }
    cb_user_row u;
    memset(&u, 0, sizeof u);
    snprintf(u.id, sizeof u.id, "%s", UID);
    snprintf(u.email, sizeof u.email, "bench@x.io");
    snprintf(u.password_hash, sizeof u.password_hash, "$2b$10$abcdefghijklmnopqrstuuabcdefghijklmnopqrstuvwxyz01234");
    snprintf(u.username, sizeof u.username, "BENCH");
    u.created_at = 1;
    CB_STORE_CALL(s, user_create, &u);
    static char ids[NG][CB_GAME_ID_MAX + 1];
    for (int i = 0; i < NG; i++) {
        snprintf(ids[i], sizeof ids[i], "%06x", 0x100000 + i * 7);
        cb_game_row g;
        memset(&g, 0, sizeof g);
        snprintf(g.id, sizeof g.id, "%s", ids[i]);
        g.status = 1;
        g.state = g_state; g.state_len = sizeof g_state;
        g.roster = g_roster; g.roster_len = sizeof g_roster;
        g.created_at = g.updated_at = g.last_commit_at = 1;
        if (CB_ROWS_CALL(s, game_create, &g, UID) != CB_OK) { fprintf(stderr, "create %d failed\n", i); return 1; }
    }
    fprintf(stderr, "phase created\n");
    cb_journal_opts o;
    memset(&o, 0, sizeof o);
    o.path = path;
    o.game_age_ms = 3600000;
    o.max_games = 8192;
    o.apply = no_apply;
#ifndef LB_B
    o.mono_ms = lb_clock;
    o.no_flush_thread = true;
#endif
    char err[256];
    if (cb_journal_open(s, &o, err, sizeof err) != CB_OK) { fprintf(stderr, "journal: %s\n", err); return 1; }
    fprintf(stderr, "phase opened\n");
    // Every game resident (off the loop: read from the tables).
    for (int i = 0; i < NG; i++) {
        int64_t v;
        if (cb_journal_game_acquire(s, ids[i], got_version, &v, NULL) != CB_OK) { fprintf(stderr, "warm %d\n", i); return 1; }
    }
    fprintf(stderr, "phase warm\n");
    // The active games: NA of them, spread over the ids, each with its version.
    int act[NA];
    int64_t ver[NA];
    for (int k = 0; k < NA; k++) {
        act[k] = (int)(((int64_t)k * NG) / NA + rng_u32(&rng) % (NG / NA));
        ver[k] = 0;
    }
    EvwMove mv;
    memset(&mv, 0, sizeof mv);
    mv.seat = 0;
    AwireAction a;
    memset(&a, 0, sizeof a);
    a.kind = AWIRE_PICKUP;
    mv.wire_len = (uint8_t)awire_encode(&a, mv.wire, (int)sizeof mv.wire);
    int order[NA];
    for (int k = 0; k < NA; k++) order[k] = k;
    const int per = NA * ROUNDS;
    double *ns = calloc((size_t)batches, sizeof *ns);
    long moves = 0;
    for (int b = 0; b < batches; b++) {
        const int64_t t0 = now_ns();
        for (int r = 0; r < ROUNDS; r++) {
            for (int k = NA - 1; k > 0; k--) {   // a shuffled round-robin
                const int x = (int)(rng_u32(&rng) % (uint32_t)(k + 1)), t = order[k];
                order[k] = order[x];
                order[x] = t;
            }
            for (int q = 0; q < NA; q++) {
                const int k = order[q];
                const char *id = ids[act[k]];
                cb_jhold *h = NULL;
                int64_t v = -1;
                if (cb_journal_game_acquire(s, id, got_version, &v, &h) != CB_OK || v != ver[k]) {
                    fprintf(stderr, "acquire %s: v %lld want %lld\n", id, (long long)v, (long long)ver[k]);
                    return 1;
                }
                cb_game_row row;
                memset(&row, 0, sizeof row);
                snprintf(row.id, sizeof row.id, "%s", id);
                row.status = 1;
                row.state = g_state; row.state_len = sizeof g_state;
                row.roster = g_roster; row.roster_len = sizeof g_roster;
                row.version = v + 1;
                row.created_at = 1;
                row.updated_at = row.last_commit_at = 2 + moves;
                cb_game_commit c;
                memset(&c, 0, offsetof(cb_game_commit, humans));
                c.row = &row;
                c.expected_version = v;
                c.moves = &mv;
                c.n_moves = 1;
                c.held = h;
                c.list_add = g_chunk;
                c.list_add_len = sizeof g_chunk;
                const int rc = CB_ROWS_CALL(s, game_commit, &c);
                cb_journal_game_release(s, h);
                if (rc != CB_OK) { fprintf(stderr, "commit %s: %d\n", id, rc); return 1; }
                ver[k] = v + 1;
                moves++;
            }
        }
        const int64_t t1 = now_ns();
        ns[b] = (double)(t1 - t0) / per;
        if (cb_journal_flush(s) != CB_OK) { fprintf(stderr, "flush failed\n"); return 1; }
    }
    cb_journal_stats st;
    cb_journal_stats_get(s, &st);
#ifdef LB_COUNT
    printf("COUNT touches %ld in_place %ld walk_steps %ld wait_begin %ld wait_end %ld moves %ld\n", lb_touch, lb_inplace,
           lb_walk, lb_wbegin, lb_wend, moves);
#endif
    // The touch alone: the same shuffled round-robin over the active games, two touches per move
    // (acquire and apply), stamped by the same clock, in a tight loop (ns per touch).
#if defined(LB_TOUCH)
    {
        cb_journal *j = cb_journal_of(s);
        jgame *gs[NA];
        for (int k = 0; k < NA; k++) gs[k] = *ov_slot(j, ids[act[k]]);
        double tn[16];
        const int reps = 16, tr = 200;
        for (int rep = 0; rep < reps; rep++) {
            const int64_t t0 = now_ns();
            for (int r = 0; r < tr; r++) {
                for (int k = NA - 1; k > 0; k--) {
                    const int x = (int)(rng_u32(&rng) % (uint32_t)(k + 1)), t = order[k];
                    order[k] = order[x];
                    order[x] = t;
                }
                for (int q = 0; q < NA; q++) {
                    jgame *g = gs[order[q]];
                    ov_touch(j, g, lb_clock());
                    ov_touch(j, g, lb_clock());
                }
            }
            tn[rep] = (double)(now_ns() - t0) / (tr * NA * 2);
        }
        qsort(tn, reps, sizeof *tn, cmp_d);
        printf("TOUCH ns_per_touch median %.2f min %.2f max %.2f (incl. the clock's draw)\n", tn[reps / 2], tn[0], tn[reps - 1]);
        // The clock alone, for subtraction.
        const int64_t c0 = now_ns();
        volatile int64_t sink = 0;
        for (long i = 0; i < 1000000; i++) sink += lb_clock();
        printf("CLOCK ns_per_read %.2f\n", (double)(now_ns() - c0) / 1e6);
    }
#endif
    double sorted[4096];
    memcpy(sorted, ns, (size_t)batches * sizeof *ns);
    qsort(sorted, (size_t)batches, sizeof *sorted, cmp_d);
    printf("RESULT %s batches %d moves_per_batch %d median %.1f min %.1f p25 %.1f p75 %.1f max %.1f ns_per_move games %ld lookups %ld\n",
           JOURNAL_NAME, batches, per, sorted[batches / 2], sorted[0], sorted[batches / 4], sorted[(3 * batches) / 4],
           sorted[batches - 1], (long)st.games, (long)st.lookups);
    printf("BATCHES");
    for (int b = 0; b < batches; b++) printf(" %.1f", ns[b]);
    printf("\n");
    cb_journal_close(s);
    cb_store_close(s);
    return 0;
}
