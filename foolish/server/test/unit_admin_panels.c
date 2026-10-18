// unit_admin_panels.c - foolish's console panels (app/admin_panels.c) over the
// plain structs core_json.c writes: the games, game and bots answers byte for
// byte against the pre-split goldens (fixture_admin_panels.h), a roster that
// does not decode (seats null, roster_error), a table of MAX_PLAYERS, and a
// game browsed but gone by the read.
#include "t.h"
#include "cbase/app.h"
#include "cbase/provider.h"

#include "admin/admin.h"
#include "app/app_internal.h"
#include "fixture_admin_panels.h"
#include "handler.h"
#include "net/router.h"
#include "store/rows_foolish.h"
#include "util/err.h"
#include "util/json.h"

#include "roster.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ADMIN "panels-golden-token"
#define NOW   1760000123456LL

static cb_ctx g_ctx;
static cb_config g_cfg;
static cb_buf g_body;

// ---- the inputs ----------------------------------------------------------------------

static uint8_t g_state[64 * 1024], g_roster[4096];
static int g_state_len, g_roster_len;

// A lobby of MAX_PLAYERS: four humans (names with a quote, a backslash and
// non-ASCII) and four bots, as the kernel writes it.
static void make_full_table(void) {
    cb_section *s = cb_section_new();
    table_init(&s->t, &s->g, &s->snaps);
    static const char *const H[4][2] = {
        { "11111111-1111-4111-8111-111111111111", "H\xc3\xb4st \"H\"" },
        { "22222222-2222-4222-8222-222222222222", "back\\slash" },
        { "33333333-3333-4333-8333-333333333333", "\xd0\x9e\xd0\xbb\xd1\x8f" },
        { "44444444-4444-4444-8444-444444444444", "dee" },
    };
    static const char *const B[4][3] = {
        { "2c601cde-63d2-5291-8fcb-735f5860b1ec", "%Miami 1", "random" },
        { "98d81d2b-94d3-540f-a5c9-106293670763", "%Miami 2", "random" },
        { "5847bff1-2330-534e-87f3-19af55d66e60", "%New York 1", "handwritten" },
        { "3e2ad143-95b7-58c9-bbce-e46f6fc0b545", "%Seoul 1", "robusta" },
    };
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 1);
    int rc = table_create(&s->t, H[0][0], (int)strlen(H[0][0]), H[0][1], (int)strlen(H[0][1]));
    for (int i = 1; rc >= 0 && i < 4; i++) rc = table_join(&s->t, H[i][0], (int)strlen(H[i][0]), H[i][1], (int)strlen(H[i][1]));
    for (int i = 0; rc >= 0 && i < 4; i++)
        rc = table_add_bot(&s->t, H[0][0], (int)strlen(H[0][0]), B[i][0], (int)strlen(B[i][0]), B[i][1], (int)strlen(B[i][1]),
                           B[i][2], (int)strlen(B[i][2]), seed);
    if (rc < 0) { fprintf(stderr, "table: %d\n", rc); exit(1); }
    rc = table_commit_products(&s->t, "f00d01", 6, 7, NOW, &s->c, s->arena, sizeof s->arena);
    if (rc < 0) { fprintf(stderr, "commit: %d\n", rc); exit(1); }
    g_state_len = s->c.state.len;
    g_roster_len = s->c.roster.len;
    memcpy(g_state, s->arena + s->c.state.off, (size_t)g_state_len);
    memcpy(g_roster, s->arena + s->c.roster.off, (size_t)g_roster_len);
    cb_section_free(s);
}

static uint8_t *dup(const void *p, size_t n) {
    uint8_t *d = malloc(n ? n : 1);
    memcpy(d, p, n);
    return d;
}

// f00d01 the full table; f00d02 bytes the kernel will not load (a roster that
// does not decode); f00d03 no roster at all; f00d04 browsed but gone by the read.
static int f_game_get(cb_store *s, const char *id, cb_game_row *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "%s", id);
    if (strcmp(id, "f00d01") == 0) {
        out->status = 0;
        out->version = 7;
        out->round_epoch = 2;
        out->needs_bots = true;
        out->created_at = NOW - 5000;
        out->updated_at = NOW - 10;
        out->last_commit_at = NOW - 20;
        out->state = dup(g_state, (size_t)g_state_len);
        out->state_len = (size_t)g_state_len;
        out->roster = dup(g_roster, (size_t)g_roster_len);
        out->roster_len = (size_t)g_roster_len;
        out->logs = dup("\x01\x02\x03", 3);
        out->logs_len = 3;
        out->has_seed = true;
        for (int i = 0; i < 32; i++) out->seed[i] = (uint8_t)i;
        return CB_OK;
    }
    if (strcmp(id, "f00d02") == 0) {
        out->status = 1;
        out->version = 3;
        out->created_at = 1;
        out->updated_at = 2;
        out->last_commit_at = 0;
        out->state = dup("\x01\x02\x03\x04", 4);
        out->state_len = 4;
        out->roster = dup(g_roster, (size_t)g_roster_len);   // ROSTER_BYTES long, an unknown format byte
        out->roster[0] = 0xee;
        out->roster_len = (size_t)g_roster_len;
        return CB_OK;
    }
    if (strcmp(id, "f00d03") == 0) {
        out->status = 2;
        out->version = 1;
        out->round_epoch = -1;
        return CB_OK;
    }
    return CB_E_NOT_FOUND;
}

static int f_browse(cb_store *s, const char *c, const char *p, const char *cur, int l, cb_admin_row_fn fn, void *ud,
                    char *next, size_t cap) {
    const char *n1[] = { "id" };
    static const char *const ids[] = { "f00d01", "f00d02", "f00d03", "f00d04" };
    for (int i = 0; i < 4; i++) {
        const char *v[] = { ids[i] };
        if (!fn(1, n1, v, ud)) break;
    }
    snprintf(next, cap, "%s", "next\"page");
    return 4;
}

static int f_bots_list(cb_store *s, cb_bot_fn fn, void *ud) {
    cb_bot_row b[3];
    memset(b, 0, sizeof b);
    snprintf(b[0].id, sizeof b[0].id, "2c601cde-63d2-5291-8fcb-735f5860b1ec");
    snprintf(b[0].nickname, sizeof b[0].nickname, "%%Miami 1");
    snprintf(b[0].strategy_key, sizeof b[0].strategy_key, "random");
    b[0].elo_rating = 1000;
    b[0].previous_elo = -1;
    snprintf(b[1].id, sizeof b[1].id, "6127a054-561f-5354-adac-db89739d26a7");
    snprintf(b[1].nickname, sizeof b[1].nickname, "%%Q\"uo\\te \xc3\xa9");
    snprintf(b[1].strategy_key, sizeof b[1].strategy_key, "cordite");
    b[1].elo_rating = 1873;
    b[1].previous_elo = 1860;
    b[1].games_played = 412;
    b[1].created_at = 5;
    b[1].updated_at = NOW;
    snprintf(b[2].id, sizeof b[2].id, "4b6ecbc2-45cf-5a0f-be01-4eb716ffb6c0");
    snprintf(b[2].nickname, sizeof b[2].nickname, "%%Moscow 1");
    snprintf(b[2].strategy_key, sizeof b[2].strategy_key, "octogen");
    b[2].elo_rating = 0;
    b[2].previous_elo = 2400;
    b[2].games_played = 1;
    b[2].updated_at = -3;
    for (int i = 0; i < 3; i++)
        if (!fn(&b[i], ud)) break;
    return 3;
}

static const cb_store_ops OPS = { .name = "sqlite", .admin_browse = f_browse };
static const cb_rows_ops ROWS = { .game_get = f_game_get, .bots_list = f_bots_list };
static const cb_store_app APP = { .rows = &ROWS };
static cb_store g_store = { .ops = &OPS, .app = &APP };

static void setup(void) {
    if (g_ctx.router) cb_router_free(g_ctx.router);
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&g_cfg, 0, sizeof g_cfg);
    snprintf(g_cfg.admin_token, sizeof g_cfg.admin_token, ADMIN);
    snprintf(g_cfg.table, sizeof g_cfg.table, "cbase");
    snprintf(g_cfg.allowed_origins, sizeof g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = &cb_provider_local;
    g_ctx.app = cb_main_app;   // foolish's panels
    g_ctx.version = "test";
    g_ctx.router = cb_router_new();
    g_ctx.store = &g_store;
    T_EQ_INT(cb_admin_routes(&g_ctx), CB_OK);
}

static int call(const char *path, const char *query) {
    cb_http_req q;
    memset(&q, 0, sizeof q);
    snprintf(q.method, sizeof q.method, "GET");
    q.path = path;
    q.query = query ? query : "";
    q.minor = 1;
    q.headers[q.n_headers++] = (cb_http_header){ "Authorization", "Bearer " ADMIN };
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
static int64_t i64_at(const char *path) {
    int64_t v = -12345;
    if (!cb_json_get_i64(B(), g_body.len, path, &v)) v = -12345;
    return v;
}
static cb_json_type type_at(const char *path) { return cb_json_find(B(), g_body.len, path, NULL); }

// Every answer is the pre-split binary's, byte for byte.
static void answers_are_the_presplit_bytes(void) {
    setup();
    static const struct { const char *name, *path, *query; } C[] = {
        { "games", "/admin/api/games", "limit=10" },
        { "game", "/admin/api/game", "id=f00d01" },
        { "game_bad", "/admin/api/game", "id=f00d02" },
        { "game_none", "/admin/api/game", "id=f00d03" },
        { "bots", "/admin/api/bots", NULL },
    };
    T_EQ_INT(ADMIN_PANEL_GOLDEN_N, (int)(sizeof C / sizeof C[0]));
    for (int i = 0; i < ADMIN_PANEL_GOLDEN_N; i++) {
        T_EQ_STR(C[i].name, ADMIN_PANEL_GOLDEN[i].name);
        T_EQ_INT(call(C[i].path, C[i].query), ADMIN_PANEL_GOLDEN[i].status);
        T_EQ_STR(B(), ADMIN_PANEL_GOLDEN[i].body);
    }
}

// A roster the kernel will not decode shows the row with seats null and the
// decoder's code; no roster at all is ROSTER_E_LENGTH.
static void a_roster_that_does_not_decode(void) {
    setup();
    T_EQ_INT(call("/admin/api/game", "id=f00d02"), 200);
    T_EQ_INT(type_at("seats"), CB_JSON_NULL);
    T_EQ_INT(i64_at("roster_error"), ROSTER_E_VERSION);
    T_EQ_INT(type_at("title"), CB_JSON_MISSING);
    T_EQ_INT(i64_at("roster_len"), g_roster_len);
    T_EQ_INT(call("/admin/api/game", "id=f00d03"), 200);
    T_EQ_INT(type_at("seats"), CB_JSON_NULL);
    T_EQ_INT(i64_at("roster_error"), ROSTER_E_LENGTH);
    // The same in the list.
    T_EQ_INT(call("/admin/api/games", NULL), 200);
    T_EQ_INT(type_at("games.1.seats"), CB_JSON_NULL);
    T_EQ_INT(i64_at("games.1.roster_error"), ROSTER_E_VERSION);
    T_EQ_INT(type_at("games.0.roster_error"), CB_JSON_MISSING);
}

// Every one of MAX_PLAYERS seats, in seat order, humans and bots told apart.
static void a_full_table(void) {
    setup();
    for (int pass = 0; pass < 2; pass++) {
        const char *pre = pass == 0 ? "" : "games.0.";
        char path[64];
        T_EQ_INT(pass == 0 ? call("/admin/api/game", "id=f00d01") : call("/admin/api/games", NULL), 200);
        snprintf(path, sizeof path, "%sseats", pre);
        T_EQ_INT(cb_json_array_len(B(), g_body.len, path), MAX_PLAYERS);
        char v[64];
        snprintf(path, sizeof path, "%sseats.%d.id", pre, MAX_PLAYERS - 1);
        T_CHECK(cb_json_get_str(B(), g_body.len, path, v, sizeof v));
        T_EQ_STR(v, "3e2ad143-95b7-58c9-bbce-e46f6fc0b545");
        bool bot = false;
        snprintf(path, sizeof path, "%sseats.%d.bot", pre, MAX_PLAYERS - 1);
        T_CHECK(cb_json_get_bool(B(), g_body.len, path, &bot) && bot);
        snprintf(path, sizeof path, "%sseats.3.bot", pre);
        T_CHECK(cb_json_get_bool(B(), g_body.len, path, &bot) && !bot);
        snprintf(path, sizeof path, "%sseats.0.name", pre);
        T_CHECK(cb_json_get_str(B(), g_body.len, path, v, sizeof v));
        T_EQ_STR(v, "H\xc3\xb4st \"H\"");
    }
    // The envelope is the spectator's, at the row's version.
    T_EQ_INT(call("/admin/api/game", "id=f00d01"), 200);
    T_EQ_INT(i64_at("envelope_version"), 7);
    T_EQ_INT(type_at("envelope_hex"), CB_JSON_STR);
}

// The list reads each browsed game; one gone by the read is left out, and
// the browse's cursor is passed on.
static void the_list_skips_a_game_gone_since_the_browse(void) {
    setup();
    T_EQ_INT(call("/admin/api/games", NULL), 200);
    T_EQ_INT(cb_json_array_len(B(), g_body.len, "games"), 3);
    char v[32];
    T_CHECK(cb_json_get_str(B(), g_body.len, "games.2.id", v, sizeof v));
    T_EQ_STR(v, "f00d03");
    T_CHECK(cb_json_get_str(B(), g_body.len, "next", v, sizeof v));
    T_EQ_STR(v, "next\"page");
}

int main(void) {
    make_full_table();
    T_RUN(answers_are_the_presplit_bytes);
    T_RUN(a_roster_that_does_not_decode);
    T_RUN(a_full_table);
    T_RUN(the_list_skips_a_game_gone_since_the_browse);
    cb_router_free(g_ctx.router);
    cb_buf_free(&g_body);
    return T_DONE();
}
