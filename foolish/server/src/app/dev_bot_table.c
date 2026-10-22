// dev_bot_table.c - dev_bot_table.h: POST /dev/bot_table, DEV-ONLY (the header says why it
// exists and what it may never become).
#include "dev_bot_table.h"

#include "bots.h"
#include "table_ops.h"
#include "admin/admin.h"
#include "store/rows_foolish.h"
#include "util/err.h"
#include "util/log.h"

#include "ask.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OCTETS "application/octet-stream"

static void refusal(cb_http_res *res, int status, int why, int code, int detail) {
    AskRefusal r = { (uint8_t)why, code, detail };
    uint8_t body[ASK_REFUSAL_BYTES];
    int n = ask_refusal_encode(&r, body, sizeof body);
    if (n > 0) cb_res_send(res, status, OCTETS, body, (size_t)n);
    else cb_router_error(res, 500, "refusal did not encode");
}

typedef struct {
    int         n, cap;
    cb_bot_row *rows;
    bool        oom;
} bot_rows;

static bool collect(const cb_bot_row *b, void *ud) {
    bot_rows *l = ud;
    if (l->n == l->cap) {
        int cap = l->cap ? 2 * l->cap : 64;
        void *p = realloc(l->rows, (size_t)cap * sizeof *l->rows);
        if (!p) { l->oom = true; return false; }
        l->rows = p;
        l->cap = cap;
    }
    l->rows[l->n++] = *b;
    return true;
}

typedef struct {
    int               passing;
    int               n;
    const cb_bot_row *bot[MAX_PLAYERS];
} seat_plan;

// THE LOBBY, as a client's asks would make it, in the one kernel section of one operation.
static int run_seat(Table *t, const cb_game_row *row, const uint8_t seed[32], void *ud) {
    (void)row;
    const seat_plan *p = ud;
    const int hl = (int)strlen(DEV_HOUSE_ID);
    int rc = table_set_rules(t, DEV_HOUSE_ID, hl, p->passing);
    if (rc < 0) return rc;   // TABLE_MOOT: the rules the lobby already has
    // The house holds a seat while it seats bots, so it seats at most MAX_PLAYERS - 1 of them.
    const int by_house = p->n < MAX_PLAYERS ? p->n : MAX_PLAYERS - 1;
    for (int i = 0; i < by_house; i++) {
        const cb_bot_row *b = p->bot[i];
        rc = table_add_bot(t, DEV_HOUSE_ID, hl, b->id, (int)strlen(b->id), b->nickname, (int)strlen(b->nickname),
                           b->strategy_key, (int)strlen(b->strategy_key), seed);
        if (rc < 0) return rc;
    }
    if ((rc = table_leave(t, DEV_HOUSE_ID, hl, DEV_HOUSE_ID, hl)) < 0) return rc;
    // Every seat is a READY bot now. The first bot seats the rest (a full table: the eighth), the
    // last of which deals; with none left it readies, which deals. Either is the kernel's deal.
    const char *first = p->bot[0]->id;
    const int fl = (int)strlen(first);
    if (by_house == p->n) return table_ready(t, first, fl, seed);
    for (int i = by_house; i < p->n; i++) {
        const cb_bot_row *b = p->bot[i];
        rc = table_add_bot(t, first, fl, b->id, (int)strlen(b->id), b->nickname, (int)strlen(b->nickname), b->strategy_key,
                           (int)strlen(b->strategy_key), seed);
        if (rc < 0) return rc;
    }
    return rc;
}

static void h_bot_table(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!cb_admin_authorized(ctx, req, res)) return;
    if (strcmp(req->method, "POST") != 0) { refusal(res, 405, ASK_WHY_METHOD, 0, 0); return; }
    if (!ctx->store) { refusal(res, 503, ASK_WHY_INTERNAL, 0, 0); return; }
    // The body, read whole: a byte short or past the end is malformed.
    const uint8_t *b = req->body;
    size_t len = req->body_len, at = 3;
    seat_plan plan;
    memset(&plan, 0, sizeof plan);
    char ids[MAX_PLAYERS][CB_UUID_LEN + 1];
    if (len < 3 || b[0] != DEV_BOT_TABLE_FORMAT || b[1] > 1 || b[2] < 2 || b[2] > MAX_PLAYERS) {
        refusal(res, 400, ASK_WHY_MALFORMED, 0, 0);
        return;
    }
    plan.passing = b[1];
    plan.n = b[2];
    for (int i = 0; i < plan.n; i++) {
        if (at >= len || b[at] == 0 || b[at] > CB_UUID_LEN || at + 1 + b[at] > len) { refusal(res, 400, ASK_WHY_MALFORMED, 0, 0); return; }
        memcpy(ids[i], b + at + 1, b[at]);
        ids[i][b[at]] = 0;
        at += 1 + (size_t)b[at];
    }
    if (at != len) { refusal(res, 400, ASK_WHY_MALFORMED, 0, 0); return; }
    bot_rows rows = { 0 };
    int rc = CB_ROWS_CALL(ctx->store, bots_list, collect, &rows);
    if (rc < 0 || rows.oom) { free(rows.rows); refusal(res, 500, ASK_WHY_INTERNAL, 0, 0); return; }
    for (int i = 0; i < plan.n; i++) {
        for (int j = 0; j < rows.n && !plan.bot[i]; j++)
            if (strcmp(rows.rows[j].id, ids[i]) == 0) plan.bot[i] = &rows.rows[j];
        if (!plan.bot[i]) { free(rows.rows); refusal(res, 400, ASK_WHY_NO_BOT, 0, 0); return; }
    }
    cb_user house;
    memset(&house, 0, sizeof house);
    snprintf(house.id, sizeof house.id, "%s", DEV_HOUSE_ID);
    snprintf(house.username, sizeof house.username, "%s", DEV_HOUSE_NAME);
    cb_table_result r;
    char gid[CB_GAME_ID_MAX + 1] = { 0 };
    rc = cb_table_create_id(ctx, &house, &r, gid);
    free(r.envelope);
    if (rc != CB_OK) {
        free(rows.rows);
        CB_ERROR("dev", "bot table: create failed (%s)", cb_strerror(rc));
        refusal(res, 500, ASK_WHY_INTERNAL, 0, 0);
        return;
    }
    cb_table_op op = { .game_id = gid, .run = run_seat, .ud = &plan, .no_envelope = true };
    rc = cb_table_run(ctx, &op, &r);
    free(r.envelope);
    free(rows.rows);
    if (rc != CB_OK) { refusal(res, 500, ASK_WHY_INTERNAL, 0, 0); return; }
    if (r.result < 0) { refusal(res, 400, ASK_WHY_REFUSED, r.result, r.detail); return; }
    if (r.committed && r.needs_bots) cb_after_kick(ctx, gid);
    uint8_t out[1 + CB_GAME_ID_MAX];
    size_t gl = strlen(gid);
    out[0] = (uint8_t)gl;
    memcpy(out + 1, gid, gl);
    cb_res_send(res, 200, OCTETS, out, 1 + gl);
}

static const cb_route DEV_ROUTES[] = { { "*", "/dev/bot_table", CB_ROUTE_POOL, h_bot_table, 0 } };

cb_route_list cb_dev_bot_table_route_list = { DEV_ROUTES, 0 };

void cb_dev_bot_table_enable(bool dev) { cb_dev_bot_table_route_list.n = dev ? 1 : 0; }
