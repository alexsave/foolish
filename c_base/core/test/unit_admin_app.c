// unit_admin_app.c - the console's dispatch of an app's own panels
// (cbase/app.h admin_calls, admin_count, delete_account) for an app that is
// not foolish: an app call runs after the platform's, a name and method the
// platform has shadow the app's, a wrong method on an app call is 405, the
// overview's counter is null when the app has none or it cannot say, and
// users/delete is 501 for an app that deletes no accounts.
#include "t.h"
#include "cbase/app.h"
#include "cbase/provider.h"

#include "admin/admin.h"
#include "handler.h"
#include "net/router.h"
#include "store/store.h"
#include "util/err.h"
#include "util/json.h"

#include <stdio.h>
#include <string.h>

#define ADMIN "admin-app-token"

static cb_ctx g_ctx;
static cb_config g_cfg;
static cb_buf g_body;

// ---- a store with one user ----------------------------------------------------------

static int f_user_get(cb_store *s, const char *id, cb_user_row *out) {
    if (strcmp(id, "u1") != 0) return CB_E_NOT_FOUND;
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "u1");
    return CB_OK;
}
static const cb_store_ops OPS = { .name = "sqlite", .user_get = f_user_get };
static cb_store g_store = { .ops = &OPS };

// ---- the app ----------------------------------------------------------------------------

static int g_ran[3];   // which of the app's handlers ran, and how often

static void send_text(cb_http_res *res, const char *text) { cb_res_send(res, 200, "text/plain", text, strlen(text)); }
static void h_panel(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { g_ran[0]++; send_text(res, "panel"); }
static void h_post(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { g_ran[1]++; send_text(res, "posted"); }
static void h_shadowed(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { g_ran[2]++; send_text(res, "shadowed"); }

static const cb_admin_call CALLS[] = {
    { "GET", "panel", h_panel },
    { "POST", "panel/act", h_post },
    { "GET", "overview", h_shadowed },   // the platform's name and method: never reached
};

static int g_count, g_count_calls;
static int count(cb_ctx *ctx, int64_t now_ms) { g_count_calls++; return g_count; }

static cb_app g_app;

static void setup(const cb_app *app, cb_store *store) {
    if (g_ctx.router) cb_router_free(g_ctx.router);
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&g_cfg, 0, sizeof g_cfg);
    snprintf(g_cfg.admin_token, sizeof g_cfg.admin_token, ADMIN);
    snprintf(g_cfg.table, sizeof g_cfg.table, "cbase");
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, "/nonexistent-cbase-admin-app/cbase.sqlite");
    snprintf(g_cfg.allowed_origins, sizeof g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = &cb_provider_local;
    g_ctx.app = app;
    g_ctx.version = "test";
    g_ctx.router = cb_router_new();
    g_ctx.store = store;
    T_EQ_INT(cb_admin_routes(&g_ctx), CB_OK);
    memset(g_ran, 0, sizeof g_ran);
    g_count_calls = 0;
}

static int call(const char *method, const char *path, const char *body) {
    cb_http_req q;
    memset(&q, 0, sizeof q);
    snprintf(q.method, sizeof q.method, "%s", method);
    q.path = path;
    q.query = "";
    q.minor = 1;
    q.headers[q.n_headers++] = (cb_http_header){ "Authorization", "Bearer " ADMIN };
    if (body) { q.body = (const uint8_t *)body; q.body_len = strlen(body); }
    cb_http_res r;
    cb_res_init(&r);
    cb_handle(&g_ctx, &q, &r);
    cb_buf_reset(&g_body);
    cb_buf_append(&g_body, r.body.p, r.body.len);
    int st = r.status;
    cb_res_free(&r);
    return st;
}

static const char *B(void) { return cb_buf_cstr(&g_body); }

static void app_with_calls(void) {
    memset(&g_app, 0, sizeof g_app);
    g_app.name = "testapp";
    g_app.admin_calls = CALLS;
    g_app.n_admin_calls = (int)(sizeof CALLS / sizeof CALLS[0]);
}

// The app's calls are found after the platform's, behind the same gate.
static void app_calls_run_after_the_platforms(void) {
    app_with_calls();
    setup(&g_app, &g_store);
    T_EQ_INT(call("GET", "/admin/api/panel", NULL), 200);
    T_EQ_STR(B(), "panel");
    T_EQ_INT(call("POST", "/admin/api/panel/act", "{}"), 200);
    T_EQ_STR(B(), "posted");
    T_EQ_INT(g_ran[0], 1);
    T_EQ_INT(g_ran[1], 1);
    // The platform's own calls still answer.
    T_EQ_INT(call("GET", "/admin/api/config", NULL), 200);
    // Not without the console's token.
    cb_http_req q;
    memset(&q, 0, sizeof q);
    snprintf(q.method, sizeof q.method, "GET");
    q.path = "/admin/api/panel";
    q.query = "";
    q.minor = 1;
    cb_http_res r;
    cb_res_init(&r);
    cb_handle(&g_ctx, &q, &r);
    T_EQ_INT(r.status, 401);
    cb_res_free(&r);
    T_EQ_INT(g_ran[0], 1);
}

// A name and method the platform has is the platform's: the app's is never run.
static void the_platforms_name_shadows_the_apps(void) {
    app_with_calls();
    setup(&g_app, &g_store);
    T_EQ_INT(call("GET", "/admin/api/overview", NULL), 200);
    T_EQ_INT(g_ran[2], 0);
    T_CHECK(strstr(B(), "\"uptime_s\"") != NULL);
    T_CHECK(strstr(B(), "shadowed") == NULL);
}

// An app call asked with another method is 405, as a platform call is; a
// name nobody has is 404; an app with no calls has only the platform's.
static void a_wrong_method_on_an_app_call_is_405(void) {
    app_with_calls();
    setup(&g_app, &g_store);
    T_EQ_INT(call("POST", "/admin/api/panel", "{}"), 405);
    T_EQ_INT(call("GET", "/admin/api/panel/act", NULL), 405);
    T_EQ_INT(call("DELETE", "/admin/api/panel", NULL), 405);
    T_EQ_INT(g_ran[0] + g_ran[1], 0);
    T_EQ_INT(call("GET", "/admin/api/panelx", NULL), 404);
    T_EQ_INT(call("GET", "/admin/api/pane", NULL), 404);
    memset(&g_app, 0, sizeof g_app);
    setup(&g_app, &g_store);
    T_EQ_INT(call("GET", "/admin/api/panel", NULL), 404);
    T_EQ_INT(call("GET", "/admin/api/games", NULL), 404);   // foolish's, not this app's
    setup(NULL, &g_store);
    T_EQ_INT(call("GET", "/admin/api/panel", NULL), 404);
    T_EQ_INT(call("GET", "/admin/api/config", NULL), 200);
}

static cb_json_type counter_type(int64_t *v) {
    *v = -12345;
    cb_json_get_i64(B(), g_body.len, "games_needing_bots", v);
    return cb_json_find(B(), g_body.len, "games_needing_bots", NULL);
}

// The overview's counter line: the app's admin_count, null when the app has
// none, when it cannot say (< 0), and when there is no store to ask.
static void the_overview_counter_is_the_apps(void) {
    int64_t v;
    memset(&g_app, 0, sizeof g_app);
    setup(&g_app, &g_store);
    T_EQ_INT(call("GET", "/admin/api/overview", NULL), 200);
    T_EQ_INT(counter_type(&v), CB_JSON_NULL);
    setup(NULL, &g_store);
    T_EQ_INT(call("GET", "/admin/api/overview", NULL), 200);
    T_EQ_INT(counter_type(&v), CB_JSON_NULL);

    g_app.admin_count = count;
    setup(&g_app, &g_store);
    g_count = 5;
    T_EQ_INT(call("GET", "/admin/api/overview", NULL), 200);
    T_EQ_INT(counter_type(&v), CB_JSON_NUM);
    T_EQ_INT(v, 5);
    g_count = 0;
    T_EQ_INT(call("GET", "/admin/api/overview", NULL), 200);
    T_EQ_INT(counter_type(&v), CB_JSON_NUM);
    T_EQ_INT(v, 0);
    g_count = -1;
    T_EQ_INT(call("GET", "/admin/api/overview", NULL), 200);
    T_EQ_INT(counter_type(&v), CB_JSON_NULL);
    g_count = -7;
    T_EQ_INT(call("GET", "/admin/api/overview", NULL), 200);
    T_EQ_INT(counter_type(&v), CB_JSON_NULL);
    T_EQ_INT(g_count_calls, 4);
    // No store: the app is not asked.
    setup(&g_app, NULL);
    g_count = 5;
    T_EQ_INT(call("GET", "/admin/api/overview", NULL), 200);
    T_EQ_INT(counter_type(&v), CB_JSON_NULL);
    T_EQ_INT(g_count_calls, 0);
}

static int g_deletes;
static int delete_account(cb_ctx *ctx, const char *user_id, int *code, int *detail) {
    g_deletes++;
    return CB_OK;
}

// users/delete runs the app's delete_account; an app with none is 501, and
// nothing is deleted. The user is looked up first either way.
static void users_delete_needs_the_apps_delete_account(void) {
    memset(&g_app, 0, sizeof g_app);
    setup(&g_app, &g_store);
    g_deletes = 0;
    T_EQ_INT(call("POST", "/admin/api/users/delete", "{\"id\":\"u1\"}"), 501);
    T_CHECK(strstr(B(), "this app deletes no accounts") != NULL);
    setup(NULL, &g_store);
    T_EQ_INT(call("POST", "/admin/api/users/delete", "{\"id\":\"u1\"}"), 501);
    T_EQ_INT(call("POST", "/admin/api/users/delete", "{\"id\":\"u2\"}"), 404);   // no such user
    g_app.delete_account = delete_account;
    setup(&g_app, &g_store);
    T_EQ_INT(call("POST", "/admin/api/users/delete", "{\"id\":\"u2\"}"), 404);
    T_EQ_INT(g_deletes, 0);
    T_EQ_INT(call("POST", "/admin/api/users/delete", "{\"id\":\"u1\"}"), 200);
    T_EQ_INT(g_deletes, 1);
}

int main(void) {
    T_RUN(app_calls_run_after_the_platforms);
    T_RUN(the_platforms_name_shadows_the_apps);
    T_RUN(a_wrong_method_on_an_app_call_is_405);
    T_RUN(the_overview_counter_is_the_apps);
    T_RUN(users_delete_needs_the_apps_delete_account);
    cb_router_free(g_ctx.router);
    cb_buf_free(&g_body);
    return T_DONE();
}
