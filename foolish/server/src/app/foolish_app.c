// foolish_app.c - foolish as the platform's app (cbase/app.h): its routes
// and the kernel self-test every main runs before it serves.
#include "foolish_app.h"

#include "admin_panels.h"

#include "app_internal.h"
#include "bots.h"
#include "pushes.h"
#include "functions_http.h"
#include "unfurl_http.h"
#include "v1_http.h"

#include "store/rows_foolish.h"
#include "store/journal.h"
#include "store/store_sql.h"
#include "app_internal.h"
#include "config.h"

#include "table.h"
#include "util/clock.h"
#include "util/err.h"
#include "../../../../shared/c/num_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cb_kernel_selftest(char *err, size_t cap) {
    static const char uid[] = "00000000-0000-4000-8000-000000000000";
    static const char gid[] = "selfte";
    enum { ARENA = 65536 };
    Game *g = calloc(1, sizeof *g);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    uint8_t *arena = malloc(ARENA), *env = malloc(ARENA);
    Table t;
    TableCommit c;
    EnvHeader h;
    uint8_t small[16];
    int rc = CB_E_STATE;
    const char *why = "allocation failed";
    if (!g || !snaps || !arena || !env) goto out;
    table_init(&t, g, snaps);
    why = "table_create refused a lobby";
    if (table_create(&t, uid, (int)strlen(uid), "SELFTEST", 8) != TABLE_OK) goto out;
    why = "table_commit_products refused the lobby";
    if (table_commit_products(&t, gid, 6, 1, cb_now_ms(), &c, arena, ARENA) < 0) goto out;
    why = "the committed lobby is not WAITING";
    if (c.status != GAME_STATUS_WAITING || c.num_players != 1) goto out;
    // Reload the durable products into a fresh table, as a request would.
    Table t2;
    table_init(&t2, g, snaps);
    why = "table_load refused the state the commit wrote";
    if (table_load(&t2, arena + c.state.off, c.state.len, arena + c.roster.off, c.roster.len) != TABLE_OK) goto out;
    why = "table_envelope refused seat 0";
    int n = table_envelope(&t2, gid, 6, 0, 1, env, ARENA);
    if (n <= 0) goto out;
    why = "the envelope's header does not read back (seat 0, version 1)";
    if (!env_header_read(env, n, &h) || h.seat != 0 || h.version != 1) goto out;
    why = "the action response is not TABLE_RESP_BYTES (7) long";
    if (table_action_response(TABLE_APPLIED, 0, 1, small, sizeof small) != TABLE_RESP_BYTES || TABLE_RESP_BYTES != 7) goto out;
    why = "the refusal is not ASK_REFUSAL_BYTES (10) long";
    AskRefusal r = { ASK_WHY_NOT_FOUND, 0, 0 };
    if (ask_refusal_encode(&r, small, sizeof small) != ASK_REFUSAL_BYTES || ASK_REFUSAL_BYTES != 10) goto out;
    rc = CB_OK;
    why = "";
out:
    if (rc != CB_OK) snprintf(err, cap, "kernel self-test: %s", why);
    free(g);
    free(snaps);
    free(arena);
    free(env);
    return rc;
}

// ---- the move journal under `cbase serve` (store/journal.h) -----------------------------
//
// CBASE_JOURNAL=0 turns it off (every move commits to the tables, as before). The flush
// rule's three knobs: CBASE_JOURNAL_GAME_AGE_S (600: a game's moves wait at most this long),
// CBASE_JOURNAL_GAME_BYTES (262144: or until they hold this many bytes) and
// CBASE_JOURNAL_TOTAL_BYTES (67108864: every game goes once all of them hold this many);
// CBASE_JOURNAL_SEG_BYTES (8388608) closes a segment; CBASE_JOURNAL_SYNC=1 adds an
// fdatasync to every batch (a move then survives an OS crash too).
static long env_long(const char *name, long dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    int64_t n;
    return num_parse_i64(v, strlen(v), 0, INT32_MAX, &n) ? (long)n : dflt;
}

static int foolish_serve_open(cb_ctx *ctx, char *err, size_t cap) {
    if (!env_long("CBASE_JOURNAL", 1) || !ctx->store || !cb_store_is_sql(ctx->store)) return CB_OK;
    const char *db = ctx->cfg ? ctx->cfg->db_path : NULL;
    if (!db || !*db || *db == ':') return CB_OK;   // an in-memory database has nothing to recover into
    cb_journal_opts o = {
        .path = db,
        .game_age_ms = (int64_t)env_long("CBASE_JOURNAL_GAME_AGE_S", 600) * 1000,
        .game_bytes = env_long("CBASE_JOURNAL_GAME_BYTES", 256 << 10),
        .total_bytes = env_long("CBASE_JOURNAL_TOTAL_BYTES", 64 << 20),
        .seg_bytes = env_long("CBASE_JOURNAL_SEG_BYTES", 8 << 20),
        .sync = env_long("CBASE_JOURNAL_SYNC", 0) != 0,
        .apply = cb_table_replay_move,
        .apply_ud = ctx,
    };
    return cb_journal_open(ctx->store, &o, err, cap);
}

static void foolish_serve_close(cb_ctx *ctx) { cb_journal_close(ctx->store); }

static const cb_route_list *const FOOLISH_ROUTES[] = { &cb_functions_route_list, &cb_v1_route_list,
                                                       &cb_unfurl_route_list };

// The bot drive, started by a kick (bots.h cb_bots_kick), and the scan that
// catches a lost one: EventBridge's literal "scan" under Lambda.
static const cb_job FOOLISH_JOBS[] = { { CB_BOTS_JOB, cb_bots_drive } };
static const cb_schedule FOOLISH_SCHEDULES[] = { { CB_BOTS_SCHEDULE, CB_BOT_SCAN_EVERY_MS, cb_bots_scan, true } };

const char *const cb_foolish_cached_paths[2] = { "/v1/leaderboard", "/v1/bots" };

const cb_app cb_foolish_app = {
    .name = "foolish",
    .routes = FOOLISH_ROUTES,
    .n_route_lists = (int)(sizeof FOOLISH_ROUTES / sizeof FOOLISH_ROUTES[0]),
    .selftest = cb_kernel_selftest,
    .jobs = FOOLISH_JOBS,
    .n_jobs = 1,
    .schedules = FOOLISH_SCHEDULES,
    .n_schedules = 1,
    .topics = cb_foolish_topics,
    .n_topics = 4,
    .cached_paths = cb_foolish_cached_paths,
    .n_cached_paths = 2,
    .store = &cb_foolish_store,
    .delete_account = cb_app_delete_account,
    .admin_calls = cb_foolish_admin_calls,
    .n_admin_calls = 3,
    .admin_count = cb_admin_games_needing_bots,
    .serve_open = foolish_serve_open,
    .serve_close = foolish_serve_close,
};

// foolish is the app every binary of this build serves (cbase/app.h).
const cb_app *const cb_main_app = &cb_foolish_app;
