// bots.c - the bot loop, rebuilt (bots.h): a lease, a drive, a scan.
//
// The port of bot_actions.ts (lockedBotLoop, runCycle) and
// functions/bot-heartbeat/index.ts. What moved: the CPU soft budget was the
// edge runtime's ~2 s per request; a Lambda has none, so the budget here is
// the invocation's deadline (ctx->deadline_ms) minus a margin, predicted the
// same way (the next cycle costs max(average * 1.5, the slowest so far)), plus
// the same 120 s wall ceiling. Continuation was the heartbeat's alone; here a
// drive that stops on its budget with work left kicks a fresh one itself.
#include "bots.h"

#include "app_internal.h"
#include "store/rows_foolish.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "after.h"
#include "jobs.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOT_PREDICT_FACTOR      1.5
#define BOT_DEADLINE_MARGIN_MS  5000   // Lambda: stop this long before the invocation's deadline
#define BOT_PREFS_CAP           4096

void (*cb_bots_cycle_observer)(const char *game_id, int attempt, int logs_len, int prefs_len);

typedef struct {
    double  compute_ms;
    int     cycles;
    int64_t max_ms;
} cpu_acct;

// What one cycle came to.
enum { CYCLE_DONE = 0, CYCLE_AGAIN = 1, CYCLE_ERROR = -1 };

// One cycle, committed (runCycle). CYCLE_AGAIN with *delay_ms set to the
// kernel's wait (0 after a committed cycle), CYCLE_DONE when nothing is left to
// drive (no bot can act, the game ended, or the game is gone), CYCLE_ERROR.
// *idle_version is the version of a row the scan holds although the kernel
// says no bot can act on it (store.h bots_idle), else -1.
static int run_cycle_locked(cb_ctx *ctx, cb_section *s, const char *game_id, int cycle, cpu_acct *cpu, int64_t *delay_ms,
                            int64_t *idle_version) {
    // The moves an attempt that lost the CAS already chose, offered back: the
    // kernel replays one only while the reloaded board still makes it legal.
    uint8_t prefs[BOT_PREFS_CAP];
    int prefs_len = 0;
    int32_t ratings[CB_MAX_SEATS];
    bool have_ratings = false;
    *delay_ms = 0;
    *idle_version = -1;

    for (int attempt = 1; attempt <= CB_TABLE_ATTEMPTS; attempt++) {
        // Always fresh, and always with the log: its length seeds every bot
        // decision (bot_drive.h), whatever the seated brains read.
        cb_game_row row;
        bool cached = false;
        int rc = cb_row_load(ctx, game_id, false, &row, &cached);
        if (rc == CB_E_NOT_FOUND) return CYCLE_DONE;
        if (rc != CB_OK) return CYCLE_ERROR;

        // ---- the kernel section ----
        int kr = cb_section_load(s, &row);
        if (kr < 0) {
            CB_ERROR("bots", "game %s: load refused (%d)", game_id, kr);
            cb_game_row_free(&row);
            return CYCLE_ERROR;
        }
        // The kernel's own answer, the one the row's needs_bots was written
        // from (table_bots_due): a row that says otherwise leaves the scan.
        if (!table_bots_due(&s->t)) {
            if (row.needs_bots) *idle_version = row.version;
            cb_game_row_free(&row);
            return CYCLE_DONE;
        }
        int64_t wait = cb_bots_wait_ms(ctx, &s->t, row.created_at, cb_app_now());
        if (wait > 0) { cb_game_row_free(&row); *delay_ms = wait; return CYCLE_AGAIN; }
        // The stored session log, whole (the belief brains read it): the LOG# items since the deal.
        if (CB_ROWS_CALL(ctx->store, game_logs, game_id, row.log_from, &row.logs, &row.logs_len) != CB_OK) {
            CB_ERROR("bots", "game %s: session log unreadable", game_id);
            cb_game_row_free(&row);
            return CYCLE_ERROR;
        }
        const int log_len = (int)row.logs_len;
        kr = table_set_session_log(&s->t, row.logs, log_len);
        if (kr < 0) {
            CB_ERROR("bots", "game %s: session log refused (%d)", game_id, kr);
            cb_game_row_free(&row);
            return CYCLE_ERROR;
        }
        if (cb_bots_cycle_observer) cb_bots_cycle_observer(game_id, attempt, log_len, prefs_len);
        BotDriveOut drv;
        const int64_t t0 = cb_mono_ms();
        kr = table_bot_drive(&s->t, prefs, prefs_len, 0, &drv);
        const int64_t drive_ms = cb_mono_ms() - t0;
        if (kr < 0) {
            CB_ERROR("bots", "game %s: bot drive refused (%d)", game_id, kr);
            cb_game_row_free(&row);
            return CYCLE_ERROR;
        }
        cpu->compute_ms += (double)drive_ms;
        cpu->cycles += 1;
        if (drive_ms > cpu->max_ms) cpu->max_ms = drive_ms;
        CB_INFO("bots", "game %s cycle %d: drove %d action(s) in %lldms (stop %d)", game_id, cycle, drv.n,
                (long long)drive_ms, drv.stop);
        if (drv.n == 0) {
            // table_bots_due said a bot could act and the drive found none:
            // the kernel disagrees with itself. Out of the scan all the same,
            // or the heartbeat would ask it the same question every minute.
            CB_WARN("bots", "game %s: a bot was due at version %lld and none acted", game_id, (long long)row.version);
            if (row.needs_bots) *idle_version = row.version;
            cb_game_row_free(&row);
            return CYCLE_DONE;
        }

        int pl = table_drive_prefs(&s->t, &drv, prefs, sizeof prefs);
        prefs_len = pl > 0 ? pl : 0;
        cb_commit_out co;
        rc = cb_section_commit(ctx, s, &row, NULL, NULL, ratings, &have_ratings, &co);
        cb_game_row_free(&row);
        if (rc != CB_OK) return CYCLE_ERROR;
        if (co.lost) continue;   // somebody else committed: reload and drive again
        return co.ended ? CYCLE_DONE : CYCLE_AGAIN;
    }
    CB_ERROR("bots", "could not commit game %s after %d attempts: write contention", game_id, CB_TABLE_ATTEMPTS);
    return CYCLE_ERROR;
}

// A cycle holds the game's lock from the row read to the commit's pushes (table_ops.c ONE LOCK),
// the drive's compute included: a human's move at the same table waits for it, as it would wait
// for the CAS it would otherwise lose.
static int run_cycle(cb_ctx *ctx, cb_section *s, const char *game_id, int cycle, cpu_acct *cpu, int64_t *delay_ms,
                     int64_t *idle_version) {
    pthread_mutex_t *order = cb_table_order_lock(game_id);
    int rc = run_cycle_locked(ctx, s, game_id, cycle, cpu, delay_ms, idle_version);
    pthread_mutex_unlock(order);
    return rc;
}

// ---- the speedup (config.h CBASE_BOT_SPEEDUP) ----------------------------------------

static int64_t speedup(const cb_ctx *ctx) {
    return ctx && ctx->cfg && ctx->cfg->bot_speedup > 1 ? ctx->cfg->bot_speedup : 1;
}

int64_t cb_bots_ms(const cb_ctx *ctx, int64_t production_ms) {
    return cb_config_scaled_ms(ctx ? ctx->cfg : NULL, production_ms);
}

int64_t cb_bots_board_now(const cb_ctx *ctx, int64_t created_at, int64_t now_ms) {
    const int64_t k = speedup(ctx);
    if (k == 1 || created_at <= 0 || now_ms <= created_at) return now_ms;
    return created_at + (now_ms - created_at) * k;
}

int64_t cb_bots_wait_ms(const cb_ctx *ctx, const Table *t, int64_t created_at, int64_t now_ms) {
    const int64_t k = speedup(ctx);
    const int64_t wait = table_bot_wait_ms(t, cb_bots_board_now(ctx, created_at, now_ms));
    return wait > 0 ? (wait + k - 1) / k : 0;
}

// ---- the lease: in memory under serve, the store's item elsewhere ---------------------------
//
// Under serve there is one process, so "a drive holds the game" is a set of game ids behind a
// mutex: no row is written to take, renew or release it, and kick_wanted reads it without a
// store round trip. An entry older than twice the drive's wall ceiling is a drive that hung (it
// cannot be released by its owner) and is taken over. The store's lease (another provider has no
// shared memory) stays for every other provider; serve is told apart by job_start_after.
typedef struct mem_lease {
    struct mem_lease *next;
    int64_t           since;
    char              id[CB_GAME_ID_MAX + 1];
} mem_lease;
static pthread_mutex_t g_lease_mu = PTHREAD_MUTEX_INITIALIZER;
static mem_lease *g_leases;

static bool in_memory(const cb_ctx *ctx) { return ctx->job_start_after != NULL; }

static bool mem_lease_live(const mem_lease *l, const cb_ctx *ctx, int64_t now) {
    return now - l->since <= 2 * cb_bots_ms(ctx, CB_BOT_WALL_MS);
}

static bool mem_lease_acquire(const cb_ctx *ctx, const char *id) {
    const int64_t now = cb_mono_ms();
    bool ok = true;
    pthread_mutex_lock(&g_lease_mu);
    for (mem_lease *l = g_leases; l; l = l->next)
        if (strcmp(l->id, id) == 0) {
            if (mem_lease_live(l, ctx, now)) ok = false;
            else l->since = now;
            pthread_mutex_unlock(&g_lease_mu);
            return ok;
        }
    mem_lease *l = calloc(1, sizeof *l);
    if (l) {
        snprintf(l->id, sizeof l->id, "%s", id);
        l->since = now;
        l->next = g_leases;
        g_leases = l;
    }
    pthread_mutex_unlock(&g_lease_mu);
    return l != NULL;
}

static void mem_lease_release(const char *id) {
    pthread_mutex_lock(&g_lease_mu);
    for (mem_lease **pp = &g_leases; *pp; pp = &(*pp)->next)
        if (strcmp((*pp)->id, id) == 0) {
            mem_lease *dead = *pp;
            *pp = dead->next;
            free(dead);
            break;
        }
    pthread_mutex_unlock(&g_lease_mu);
}

static bool mem_lease_held(const cb_ctx *ctx, const char *id) {
    const int64_t now = cb_mono_ms();
    bool held = false;
    pthread_mutex_lock(&g_lease_mu);
    for (mem_lease *l = g_leases; l; l = l->next)
        if (strcmp(l->id, id) == 0) held = mem_lease_live(l, ctx, now);
    pthread_mutex_unlock(&g_lease_mu);
    return held;
}

// ms left before this drive must stop, or INT64_MAX under serve (no deadline).
static int64_t budget_left(const cb_ctx *ctx) {
    if (ctx->deadline_ms <= 0) return INT64_MAX;
    return ctx->deadline_ms - BOT_DEADLINE_MARGIN_MS - cb_app_now();
}

// One leased round of a drive: acquire, cycle until done or out of budget,
// release. *settled is true when it let go because no bot could act (or the
// game ended) with the lease still its own: the case a kick skipped while the
// lease was held relies on (kick_wanted), so the caller looks once more.
static int drive_round(cb_ctx *ctx, const char *game_id, bool *settled) {
    *settled = false;
    cb_uuid lease;
    int rc = CB_OK;
    if (in_memory(ctx)) { if (!mem_lease_acquire(ctx, game_id)) rc = CB_E_CONFLICT; }
    else rc = CB_ROWS_CALL(ctx->store, lease_acquire, game_id, cb_app_now(), cb_bots_ms(ctx, CB_BOT_LEASE_MS), lease);
    if (rc == CB_E_CONFLICT) {
        CB_INFO("bots", "game %s: bot lease held by another drive, skipping", game_id);
        return CB_OK;
    }
    if (rc != CB_OK) {
        if (rc != CB_E_NOT_FOUND) CB_ERROR("bots", "game %s: lease not acquired (%s)", game_id, cb_strerror(rc));
        return rc;
    }
    cb_section *s = cb_section_new();
    if (!s) {
        if (in_memory(ctx)) mem_lease_release(game_id);
        else CB_ROWS_CALL(ctx->store, lease_release, game_id, lease);
        return CB_E_NOMEM;
    }
    cpu_acct cpu = { 0, 0, 0 };
    const int64_t started = cb_app_now();
    bool rekick = false;
    int64_t wait_ms = 0;   // > 0: the drive lets go and the provider re-runs it after this long
    int64_t idle_version = -1;
    rc = CB_OK;
    for (int cycle = 0;; cycle++) {
        if (cycle > 0) {
            if (cb_app_now() - started > cb_bots_ms(ctx, CB_BOT_WALL_MS)) { rekick = true; break; }
            if (cpu.cycles > 0) {
                double avg = cpu.compute_ms / cpu.cycles;
                double predicted = avg * BOT_PREDICT_FACTOR;
                if (predicted < (double)cpu.max_ms) predicted = (double)cpu.max_ms;
                if (predicted > (double)budget_left(ctx)) { rekick = true; break; }
            }
            int r = in_memory(ctx) ? CB_OK
                                   : CB_ROWS_CALL(ctx->store, lease_renew, game_id, lease, cb_app_now(), cb_bots_ms(ctx, CB_BOT_LEASE_MS));
            // Another drive took the lease over; a store failure keeps going (renewBotLease).
            if (r == CB_E_CONFLICT) break;
            if (r != CB_OK) CB_ERROR("bots", "game %s: lease renewal failed (%s), driving on", game_id, cb_strerror(r));
        }
        int64_t delay = 0;
        int c = run_cycle(ctx, s, game_id, cycle, &cpu, &delay, &idle_version);
        if (c == CYCLE_ERROR) { rc = CB_E_DB; break; }
        if (c == CYCLE_DONE) { *settled = true; break; }
        if (delay > 0) {
            // A wait that would outlive the budget is the next drive's to sit out.
            if (delay > budget_left(ctx)) { rekick = true; break; }
            // Where the provider can start a job later without holding a thread
            // (serve: a loop timer), the wait is its to keep: sleeping here would
            // stall every other game's bots behind this one's pause. A one-shot platform has
            // no such start and sits the wait out, one invocation per drive.
            if (ctx->job_start_after) { wait_ms = delay; break; }
            cb_app_sleep(delay);
        }
    }
    if (idle_version >= 0) {
        // Before the release: the delete takes the lease with the marker.
        int r = CB_ROWS_CALL(ctx->store, bots_idle, game_id, idle_version);
        if (r == CB_OK)
            CB_INFO("bots", "game %s: no bot can act at version %lld, out of the scan", game_id, (long long)idle_version);
        else if (r != CB_E_CONFLICT)
            CB_ERROR("bots", "game %s: could not take it out of the scan (%s)", game_id, cb_strerror(r));
    }
    if (in_memory(ctx)) mem_lease_release(game_id);
    else CB_ROWS_CALL(ctx->store, lease_release, game_id, lease);
    cb_section_free(s);
    if (wait_ms > 0) {
        int r = cb_job_start_after(ctx, CB_BOTS_JOB, game_id, wait_ms);
        // A lost timer is the heartbeat scan's to catch, as a lost kick is.
        if (r != CB_OK) CB_WARN("bots", "wait %s: not scheduled (%s)", game_id, cb_strerror(r));
    }
    if (rekick && ctx->job_start) {
        CB_INFO("bots", "game %s: budget spent with bot work left, kicking a fresh drive", game_id);
        int r = cb_job_start(ctx, CB_BOTS_JOB, game_id);
        if (r != CB_OK) CB_WARN("bots", "kick %s: not started (%s)", game_id, cb_strerror(r));
    }
    return rc;
}

// A bot can act on the stored row now (a fresh read).
static bool due_now(cb_ctx *ctx, const char *game_id) {
    cb_game_row row;
    bool cached = false;
    if (cb_row_load(ctx, game_id, false, &row, &cached) != CB_OK) return false;
    cb_section *s = cb_section_new();
    bool due = s && cb_section_load(s, &row) >= 0 && table_bots_due(&s->t);
    cb_section_free(s);
    cb_game_row_free(&row);
    return due;
}

#define BOT_DRIVE_ROUNDS 8

int cb_bots_drive(cb_ctx *ctx, const char *game_id) {
    if (!ctx->store) return CB_E_STATE;
    int rc = CB_OK;
    for (int round = 0; round < BOT_DRIVE_ROUNDS; round++) {
        bool settled = false;
        rc = drive_round(ctx, game_id, &settled);
        // A commit that landed while this drive held the lease kicked no drive
        // (kick_wanted): its kick's lease check came after its commit, and this
        // read comes after the release, so one of the two saw the other.
        if (rc != CB_OK || !settled || !due_now(ctx, game_id)) break;
        CB_INFO("bots", "game %s: a move landed while the lease was held, driving again", game_id);
    }
    return rc;
}

// ---- the heartbeat's scan -----------------------------------------------------------

typedef struct {
    int  n;
    char ids[CB_BOT_SCAN_MAX][CB_GAME_ID_MAX + 1];
} scan_list;

static bool scan_collect(const char *id, void *ud) {
    scan_list *l = ud;
    if (l->n >= CB_BOT_SCAN_MAX) return false;
    snprintf(l->ids[l->n++], sizeof l->ids[0], "%s", id);
    return true;
}

int cb_bots_scan(cb_ctx *ctx, int64_t now_ms) {
    if (!ctx->store) return CB_E_STATE;
    // Under serve this runs once, a period after start (cb_schedule serve_once): the recovery of
    // games whose bots were due when the last process stopped. Every later drive is started by
    // the commit that left bots due (cb_bots_kick) or by its own wait timer.
    scan_list *l = calloc(1, sizeof *l);
    if (!l) return CB_E_NOMEM;
    // Stale on updated_at (a drive in flight has just touched the row), active
    // on last_commit_at (only a commit moves it, so the scan's own leases never
    // refresh the abandon guard).
    int rc = CB_ROWS_CALL(ctx->store, games_needing_bots, now_ms - cb_bots_ms(ctx, CB_BOT_STALE_MS),
                           now_ms - cb_bots_ms(ctx, CB_BOT_ABANDON_MS),
                           CB_BOT_SCAN_MAX, scan_collect, l);
    if (rc < 0) {
        CB_ERROR("bots", "heartbeat scan failed (%s)", cb_strerror(rc));
        free(l);
        return rc;
    }
    for (int i = 0; i < l->n; i++) cb_bots_kick(ctx, l->ids[i]);
    // An idle minute (or, under serve, an idle 10 s) is not news.
    if (l->n > 0) CB_INFO("bots", "heartbeat dispatched=%d", l->n);
    else CB_DEBUG("bots", "heartbeat dispatched=0");
    rc = l->n;
    free(l);
    return rc;
}

// ---- the kick ------------------------------------------------------------------------

// Whether a kick should start a drive: not while another drive holds the
// game's lease. That drive reloads the row before every cycle, and looks once
// more after it lets go (cb_bots_drive), so the move that asked for the kick
// is driven either way; a second drive would only find the lease taken
// (production, 2026-10-17: about one such drive per human move in a bot
// game). A store that cannot say kicks: a wasted drive beats a stalled game.
static bool kick_wanted(cb_ctx *ctx, const char *game_id) {
    if (!ctx->store) return true;
    int held = in_memory(ctx) ? (mem_lease_held(ctx, game_id) ? 1 : 0)
                              : CB_ROWS_CALL(ctx->store, lease_held, game_id, cb_app_now());
    if (held == 1) {
        CB_DEBUG("bots", "kick %s: a drive holds the lease, it takes this move", game_id);
        return false;
    }
    return true;
}

typedef struct {
    char game_id[CB_GAME_ID_MAX + 1];
} held_kick;

// After the commit, so its lease check sees any drive.
static void held_kick_run(cb_ctx *ctx, void *arg) { cb_bots_kick(ctx, ((held_kick *)arg)->game_id); }

void cb_after_kick(cb_ctx *ctx, const char *game_id) {
    if (!ctx->job_start) return;
    held_kick *k = cb_after_open() ? calloc(1, sizeof *k) : NULL;
    if (k) {
        snprintf(k->game_id, sizeof k->game_id, "%s", game_id);
        if (cb_after_defer(held_kick_run, free, k)) return;
        free(k);
    }
    cb_bots_kick(ctx, game_id);
}

void cb_bots_kick(cb_ctx *ctx, const char *game_id) {
    if (!ctx->job_start || !kick_wanted(ctx, game_id)) return;
    int rc = cb_job_start(ctx, CB_BOTS_JOB, game_id);
    // Under serve no scan follows to catch a lost kick: it is started again from a timer.
    if (rc != CB_OK && rc != CB_E_NOMEM && ctx->job_start_after) rc = cb_job_start_after(ctx, CB_BOTS_JOB, game_id, 1000);
    if (rc != CB_OK) CB_WARN("bots", "kick %s: not started (%s)", game_id, cb_strerror(rc));
}
