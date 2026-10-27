// dev_bot_table.c - dev_bot_table.h: POST /dev/bot_table, DEV-ONLY (the header says why it
// exists and what it may never become).
#include "dev_bot_table.h"

#include "bots.h"
#include "table_ops.h"
#include "admin/admin.h"
#include "auth/auth.h"
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

// One seat: a bot (its row) or a human (the account's id and stored username).
typedef struct {
    const cb_bot_row *bot;   // NULL for a human
    char              id[CB_UUID_LEN + 1];
    char              name[CB_USERNAME_MAX + 1];
} seat;

typedef struct {
    int  passing;
    int  n;
    bool human;   // any seat is a human's: the route readies nobody
    int  title_len;   // format 2's title, 0 for the lobby's own ("house's Game")
    char title[ROSTER_TITLE_MAX + 1];
    seat s[MAX_PLAYERS];
} seat_plan;

// Seats p->s[i]: a bot by `actor` (who must sit), a human as its own ASK_JOIN (UNREADY).
static int seat_one(Table *t, const seat_plan *p, int i, const char *actor, const uint8_t seed[32]) {
    const seat *s = &p->s[i];
    if (!s->bot) return table_join(t, s->id, (int)strlen(s->id), s->name, (int)strlen(s->name));
    const cb_bot_row *b = s->bot;
    return table_add_bot(t, actor, (int)strlen(actor), b->id, (int)strlen(b->id), b->nickname, (int)strlen(b->nickname),
                         b->strategy_key, (int)strlen(b->strategy_key), seed);
}

// THE LOBBY, as a client's asks would make it, in the one kernel section of one operation.
static int run_seat(Table *t, const cb_game_row *row, const uint8_t seed[32], void *ud) {
    (void)row;
    const seat_plan *p = ud;
    const int hl = (int)strlen(DEV_HOUSE_ID);
    int rc = table_set_rules(t, DEV_HOUSE_ID, hl, p->passing);
    if (rc < 0) return rc;   // TABLE_MOOT: the rules the lobby already has
    // The house holds a seat while it seats the others, so it seats at most MAX_PLAYERS - 1. It
    // is never ready, so nothing deals while it sits.
    const int by_house = p->n < MAX_PLAYERS ? p->n : MAX_PLAYERS - 1;
    for (int i = 0; i < by_house; i++)
        if ((rc = seat_one(t, p, i, DEV_HOUSE_ID, seed)) < 0) return rc;
    // The title, as a seated player's ASK_UPDATE_NAME sets it (the kernel's title rule decides).
    if (p->title_len && (rc = table_retitle(t, DEV_HOUSE_ID, hl, p->title, p->title_len)) < 0) return rc;
    // With bots alone left, the leave answers TABLE_EMPTY (the kernel's rule for a lobby no human
    // can deal), which would have the host delete the row; it never reaches the host, because the
    // first bot readies or seats the rest below, in this same operation, and that answer is the one
    // returned. With a human seated it is TABLE_OK and the lobby waits for the human.
    if ((rc = table_leave(t, DEV_HOUSE_ID, hl, DEV_HOUSE_ID, hl)) < 0) return rc;
    // The first seat seats the rest (a full table: the eighth). With every seat a READY bot, the
    // last add deals, or with none left the first bot readies, which deals: the kernel's deal.
    // With a human seated (UNREADY) neither deals: the table waits in its lobby for the human's
    // Start, and nobody is readied here.
    const char *first = p->s[0].id;
    if (by_house == p->n) return p->human ? rc : table_ready(t, first, (int)strlen(first), seed);
    for (int i = by_house; i < p->n; i++)
        if ((rc = seat_one(t, p, i, first, seed)) < 0) return rc;
    return rc;
}

// The body: format 1 (every seat a bot) or format 2 (each seat names its kind) into plan, ids
// into the plan's seats. false when it is malformed.
static bool parse(const uint8_t *b, size_t len, seat_plan *plan, uint8_t kind[MAX_PLAYERS]) {
    if (len < 3 || (b[0] != DEV_BOT_TABLE_FORMAT && b[0] != DEV_BOT_TABLE_FORMAT_2) || b[1] > 1 || b[2] < 2 || b[2] > MAX_PLAYERS)
        return false;
    const bool kinds = b[0] == DEV_BOT_TABLE_FORMAT_2;
    plan->passing = b[1];
    plan->n = b[2];
    size_t at = 3;
    for (int i = 0; i < plan->n; i++) {
        kind[i] = DEV_SEAT_BOT;
        if (kinds) {
            if (at >= len || b[at] > DEV_SEAT_HUMAN) return false;
            kind[i] = b[at++];
        }
        if (at >= len || b[at] == 0 || b[at] > CB_UUID_LEN || at + 1 + b[at] > len) return false;
        memcpy(plan->s[i].id, b + at + 1, b[at]);
        plan->s[i].id[b[at]] = 0;
        at += 1 + (size_t)b[at];
        if (kind[i] == DEV_SEAT_HUMAN) plan->human = true;
    }
    // Format 2 may end with a title { u8 len, bytes }: 0 keeps the lobby's own.
    if (kinds && at < len) {
        if (b[at] > ROSTER_TITLE_MAX || at + 1 + b[at] != len) return false;
        plan->title_len = b[at];
        memcpy(plan->title, b + at + 1, b[at]);
        plan->title[b[at]] = 0;
        at = len;
    }
    return at == len;
}

static void h_bot_table(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!cb_admin_authorized(ctx, req, res)) return;
    if (strcmp(req->method, "POST") != 0) { refusal(res, 405, ASK_WHY_METHOD, 0, 0); return; }
    if (!ctx->store || !ctx->auth) { refusal(res, 503, ASK_WHY_INTERNAL, 0, 0); return; }
    // The body, read whole: a byte short or past the end is malformed.
    seat_plan plan;
    memset(&plan, 0, sizeof plan);
    uint8_t kind[MAX_PLAYERS];
    if (!parse(req->body, req->body_len, &plan, kind)) { refusal(res, 400, ASK_WHY_MALFORMED, 0, 0); return; }
    bot_rows rows = { 0 };
    int rc = CB_ROWS_CALL(ctx->store, bots_list, collect, &rows);
    if (rc < 0 || rows.oom) { free(rows.rows); refusal(res, 500, ASK_WHY_INTERNAL, 0, 0); return; }
    for (int i = 0; i < plan.n; i++) {
        seat *s = &plan.s[i];
        if (kind[i] == DEV_SEAT_HUMAN) {
            // An account, by its user id, under the name it signed up with (as ASK_JOIN seats it).
            cb_user_row u;
            rc = cb_auth_user_row(ctx->auth, s->id, &u);
            if (rc == CB_E_NOT_FOUND || (rc == CB_OK && !u.username[0])) { free(rows.rows); refusal(res, 400, ASK_WHY_MALFORMED, 0, 0); return; }
            if (rc != CB_OK) { free(rows.rows); refusal(res, 500, ASK_WHY_INTERNAL, 0, 0); return; }
            snprintf(s->name, sizeof s->name, "%s", u.username);
            continue;
        }
        for (int j = 0; j < rows.n && !s->bot; j++)
            if (strcmp(rows.rows[j].id, s->id) == 0) s->bot = &rows.rows[j];
        if (!s->bot) { free(rows.rows); refusal(res, 400, ASK_WHY_NO_BOT, 0, 0); return; }
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
