/* cc_app.c - c_chess as the platform's app (cbase/app.h): its routes, its topic families, its
 * tables, and serve_open, where the journal is read back into memory, the journal's writer and the
 * clock thread start, and the site is loaded, all before the first request is served.
 *
 * Settings, beside the platform's CBASE_* (config.h):
 *   CC_WEB_DIR   the built site (web/out); unset: no site, the API alone */
#include "cbase/app.h"

#include "api.h"
#include "cc_server.h"
#include "journal.h"
#include "pushes.h"
#include "web.h"

#include "store/store.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>

extern const char cc_schema_sql[];
int cc_state_start_clock(void);

static const cb_store_app CC_STORE = {
    .sql_schema = cc_schema_sql,
    .sql_stmts = cc_sql_stmts,
    .n_sql_stmts = CC_SQL_N,
};

static int cc_serve_open(cb_ctx *ctx, char *err, size_t cap) {
    if (cc_state_init(ctx) != 0) { snprintf(err, cap, "c_chess: no memory for the state"); return CB_E_NOMEM; }
    cc_pushes_init(ctx);
    int rc = cc_journal_recover(ctx, err, cap);
    if (rc != CB_OK) return rc;
    if ((rc = cc_journal_start(ctx)) != CB_OK) { snprintf(err, cap, "c_chess: the journal's writer did not start"); return rc; }
    if (cc_state_start_clock() != 0) { snprintf(err, cap, "c_chess: the clock thread did not start"); return CB_E_NOMEM; }
    cc_web_load(getenv("CC_WEB_DIR"));
    return CB_OK;
}

static void cc_serve_close(cb_ctx *ctx) {
    (void)ctx;
    cc_state_free();     /* the clock thread first: it may still queue a finish */
    cc_journal_stop();   /* then everything queued is committed */
    cc_web_free();
}

static const cb_route_list *const CC_ROUTES[] = { &cc_api_routes, &cc_web_routes };
static const cb_topic_family *const CC_TOPICS[] = { &cc_fam_room, &cc_fam_user };

static const cb_app CC_APP = {
    .name = "c_chess",
    .routes = CC_ROUTES,
    .n_route_lists = 2,
    .topics = CC_TOPICS,
    .n_topics = 2,
    .store = &CC_STORE,
    .serve_open = cc_serve_open,
    .serve_close = cc_serve_close,
};

const cb_app *const cb_main_app = &CC_APP;
