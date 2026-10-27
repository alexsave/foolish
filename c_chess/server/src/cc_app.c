/* cc_app.c - c_chess as the platform's app (cbase/app.h): its routes, its topic families, its
 * tables, and serve_open, where the journal is read back into memory, the journal's writer and the
 * clock thread start, and the site is loaded, all before the first request is served.
 *
 * Settings, beside the platform's CBASE_* (config.h):
 *   CC_WEB_DIR   the built site (web/out); unset: no site, the API alone
 *   CC_ENGINE    the Stockfish executable (engine.h, with CC_ENGINES and the rest); unset: no computer
 *   CC_PUZZLES   the puzzle file (puzzle.h, from tools/puzzlegen); unset: no puzzles
 *   CC_ARENA_SCHEDULE  0: the site makes no hourly arenas (the tests); unset: it does (arenas.h) */
#include "cbase/app.h"

#include "api.h"
#include "cc_server.h"
#include "journal.h"
#include "pushes.h"
#include "web.h"
#include "analysis.h"
#include "engine.h"
#include "puzzles.h"
#include "arenas.h"

#include "store/store.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char cc_schema_sql[];
int cc_state_start_clock(void);

static const cb_store_app CC_STORE = {
    .sql_schema = cc_schema_sql,
    .sql_stmts = cc_sql_stmts,
    .n_sql_stmts = CC_SQL_N,
};

static int cc_serve_open(cb_ctx *ctx, char *err, size_t cap) {
    if (cc_state_init(ctx) != 0) { snprintf(err, cap, "c_chess: no memory for the state"); return CB_E_NOMEM; }
    const char *sched = getenv("CC_ARENA_SCHEDULE");
    cc_arena_schedule(!sched || strcmp(sched, "0") != 0);
    cc_pushes_init(ctx);
    int rc = cc_puzzles_load(getenv("CC_PUZZLES"), err, cap);   /* before recovery: it rates them */
    if (rc != CB_OK) return rc;
    rc = cc_journal_recover(ctx, err, cap);
    if (rc != CB_OK) return rc;
    if ((rc = cc_journal_start(ctx)) != CB_OK) { snprintf(err, cap, "c_chess: the journal's writer did not start"); return rc; }
    if (cc_state_start_clock() != 0) { snprintf(err, cap, "c_chess: the clock thread did not start"); return CB_E_NOMEM; }
    cc_web_load(getenv("CC_WEB_DIR"));
    if ((rc = cc_engine_start()) != CB_OK) { snprintf(err, cap, "c_chess: the engine did not start (CC_ENGINE)"); return rc; }
    return CB_OK;
}

static void cc_serve_close(cb_ctx *ctx) {
    (void)ctx;
    cc_engine_stop();    /* no answer arrives after this */
    cc_state_free();     /* the clock thread first: it may still queue a finish */
    cc_journal_stop();   /* then everything queued is committed */
    cc_web_free();
    cc_puzzles_free();
}

static const cb_route_list *const CC_ROUTES[] = { &cc_api_routes, &cc_arena_routes, &cc_analysis_routes, &cc_puzzle_routes, &cc_web_routes };
static const cb_topic_family *const CC_TOPICS[] = { &cc_fam_room, &cc_fam_user };

static const cb_app CC_APP = {
    .name = "c_chess",
    .routes = CC_ROUTES,
    .n_route_lists = (int)(sizeof CC_ROUTES / sizeof *CC_ROUTES),
    .topics = CC_TOPICS,
    .n_topics = 2,
    .store = &CC_STORE,
    .serve_open = cc_serve_open,
    .serve_close = cc_serve_close,
};

const cb_app *const cb_main_app = &CC_APP;
