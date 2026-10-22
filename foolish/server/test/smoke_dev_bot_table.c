// smoke_dev_bot_table.c - POST /dev/bot_table (app/dev_bot_table.h), end to end against a real
// `cbase serve`.
//
//   build/test/smoke_dev_bot_table build/cbase
//
// Two servers:
//   PRODUCTION-SHAPED (no CBASE_DEV): the path is not routed. With the admin token it answers
//     exactly what an unrouted path answers, and so does every other caller.
//   DEV (CBASE_DEV=1, the bots at CBASE_BOT_SPEEDUP=100): no token, a user's token and a wrong
//     admin token are refused (401); malformed bodies and an unknown bot are refused (400); with
//     the admin token a table of two bots and one of three deal, play to a fool with nobody human
//     seated, and are rated: the finished game's stamp, the bots' rows on the leaderboard and
//     the stamp agree to the point.
#include "t.h"

#include "lib_client.h"
#include "lib_proc.h"

#include "ask.h"
#include "client_table.h"
#include "netwire.h"
#include "util/clock.h"

#include "../../../shared/c/hex/hex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADMIN "dev-bot-table-admin-token-0123456789abcdef"
#define DEV_BOT_TABLE_FORMAT 1   // dev_bot_table.h's; the test speaks the bytes as a client does

static const char *g_bin;

typedef struct {
    tp_child child;
    int      port;
    char     dir[400];
} server;

static bool start(server *s, bool dev) {
    if (tp_tempdir(s->dir, sizeof s->dir) != 0) return false;
    static char envdb[2][512];
    snprintf(envdb[dev], sizeof envdb[dev], "CBASE_DB=%s/cbase.sqlite", s->dir);
    char *dev_env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", envdb[1], "CBASE_JWT_SECRET=devbot-secret-devbot-secret-devbot-secret",
                        "CBASE_ADMIN_TOKEN=" ADMIN, "CBASE_BOT_SPEEDUP=100", "CBASE_RATED_MIN_AGE=0", NULL };
    char *prod_env[] = { "CBASE_LISTEN=127.0.0.1:0", envdb[0], "CBASE_JWT_SECRET=devbot-secret-devbot-secret-devbot-secret",
                         "CBASE_PUBLIC_URL=http://127.0.0.1", "CBASE_ADMIN_TOKEN=" ADMIN, NULL };
    char *argv[] = { (char *)g_bin, "serve", NULL };
    if (tp_start(&s->child, argv, dev ? dev_env : prod_env) != 0) return false;
    char line[512];
    if (tp_wait_line(&s->child, "listening on", 20000, line, sizeof line) != 0) return false;
    const char *colon = strrchr(line, ':');
    s->port = colon ? atoi(colon + 1) : 0;
    return s->port > 0;
}

// One request; the status, and the body into out when given. `bearer` NULL: none.
static int req(const server *s, const char *method, const char *path, const char *bearer, const void *body, size_t n, cb_buf *out) {
    char headers[1024];
    int h = 0;
    if (bearer) h += snprintf(headers + h, sizeof headers - (size_t)h, "Authorization: Bearer %s\r\n", bearer);
    if (body) h += snprintf(headers + h, sizeof headers - (size_t)h, "Content-Type: application/octet-stream\r\n");
    headers[h] = 0;
    tc_response r;
    if (tc_http(s->port, method, path, headers, body, n, &r) != 0) return -1;
    int st = r.status;
    if (out) { cb_buf_reset(out); cb_buf_append(out, r.body.p, r.body.len); }
    tc_response_free(&r);
    return st;
}

// The request body for `n` bot ids.
static size_t body_for(uint8_t *b, int passing, int n, char ids[][NW_ID_MAX + 1]) {
    size_t at = 0;
    b[at++] = DEV_BOT_TABLE_FORMAT;
    b[at++] = (uint8_t)passing;
    b[at++] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(ids[i]);
        b[at++] = (uint8_t)l;
        memcpy(b + at, ids[i], l);
        at += l;
    }
    return at;
}

static int bots(const server *s, char ids[][NW_ID_MAX + 1], int cap) {
    cb_buf out;
    cb_buf_init(&out);
    int n = 0;
    if (req(s, "GET", "/v1/bots", NULL, NULL, 0, &out) == 200) {
        int k = nw_list_open(out.p, (int)out.len, NW_BOTS);
        for (int i = 0; i < k && n < cap; i++) {
            NwBot b;
            if (nw_get_bot(out.p, (int)out.len, i, &b) == NW_OK) snprintf(ids[n++], NW_ID_MAX + 1, "%.*s", (int)b.id_len, b.id);
        }
    }
    cb_buf_free(&out);
    return n;
}

// ---- production-shaped: not routed -------------------------------------------------------

static void absent_without_dev(void) {
    server s;
    memset(&s, 0, sizeof s);
    T_CHECK(start(&s, false));
    char ids[2][NW_ID_MAX + 1];
    T_EQ_INT(bots(&s, ids, 2), 2);
    uint8_t b[256];
    size_t n = body_for(b, 1, 2, ids);
    cb_buf unrouted, out;
    cb_buf_init(&unrouted);
    cb_buf_init(&out);
    int st_unrouted = req(&s, "POST", "/dev/no_such_route", ADMIN, b, n, &unrouted);
    T_EQ_INT(st_unrouted, 404);
    T_EQ_INT(req(&s, "POST", "/dev/bot_table", ADMIN, b, n, &out), st_unrouted);
    T_EQ_MEM(out.p, out.len, unrouted.p, unrouted.len);
    T_EQ_INT(req(&s, "POST", "/dev/bot_table", NULL, b, n, &out), 404);
    T_EQ_INT(req(&s, "GET", "/dev/bot_table", ADMIN, NULL, 0, &out), 404);
    cb_buf_free(&unrouted);
    cb_buf_free(&out);
    tp_stop(&s.child, 15, 10000);
}

// ---- dev: the gate, the bytes, and a bot-only game to its fool --------------------------

static server g_dev;

static char g_user_token[NW_ACCESS_MAX + 1];

static bool signup(const char *name) {
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNUP;
    q.name_len = (uint8_t)strlen(name);
    memcpy(q.name, name, q.name_len);
    q.password_len = 14;
    memcpy(q.password, "devbot-pass-12", 14);
    uint8_t b[256];
    int n = nw_request_encode(&q, b, sizeof b);
    cb_buf out;
    cb_buf_init(&out);
    static NwSession ss;
    bool ok = n > 0 && req(&g_dev, "POST", "/v1/auth/signup", NULL, b, (size_t)n, &out) == 200 &&
              nw_session_decode(out.p, (int)out.len, &ss) == NW_OK;
    if (ok) snprintf(g_user_token, sizeof g_user_token, "%.*s", (int)ss.access_len, ss.access);
    cb_buf_free(&out);
    return ok;
}

static int refusal_why(const cb_buf *b) {
    AskRefusal r;
    return ask_refusal_decode(b->p, (int)b->len, &r) == ASK_OK ? r.why : -1;
}

static void gate_and_bytes(void) {
    T_CHECK(start(&g_dev, true));
    T_CHECK(signup("DEVBOT_U"));
    char ids[3][NW_ID_MAX + 1];
    T_EQ_INT(bots(&g_dev, ids, 3), 3);
    uint8_t b[256];
    size_t n = body_for(b, 1, 2, ids);
    cb_buf out;
    cb_buf_init(&out);
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", NULL, b, n, &out), 401);
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", g_user_token, b, n, &out), 401);
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN "x", b, n, &out), 401);
    // The bytes: a short body, another format, one seat, a byte past the end, an unknown bot.
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, b, 2, &out), 400);
    T_EQ_INT(refusal_why(&out), ASK_WHY_MALFORMED);
    uint8_t bad[256];
    memcpy(bad, b, n);
    bad[0] = 9;
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, bad, n, &out), 400);
    T_EQ_INT(refusal_why(&out), ASK_WHY_MALFORMED);
    size_t one = body_for(bad, 1, 1, ids);
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, bad, one, &out), 400);
    memcpy(bad, b, n);
    bad[n] = 0;
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, bad, n + 1, &out), 400);
    T_EQ_INT(refusal_why(&out), ASK_WHY_MALFORMED);
    char ghost[2][NW_ID_MAX + 1];
    snprintf(ghost[0], sizeof ghost[0], "%s", ids[0]);
    snprintf(ghost[1], sizeof ghost[1], "00000000-0000-4000-8000-000000000000");
    size_t gn = body_for(bad, 1, 2, ghost);
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, bad, gn, &out), 400);
    T_EQ_INT(refusal_why(&out), ASK_WHY_NO_BOT);
    cb_buf_free(&out);
}

// The spectator envelope the admin console reads of a game, into c. false when it cannot.
static bool game_view(const char *gid, ClientTable *c, ClientSlot *slot) {
    char path[128];
    snprintf(path, sizeof path, "/admin/api/game?id=%s", gid);
    cb_buf out;
    cb_buf_init(&out);
    bool ok = false;
    if (req(&g_dev, "GET", path, ADMIN, NULL, 0, &out) == 200) {
        cb_buf_append(&out, "", 1);
        const char *k = strstr((const char *)out.p, "\"envelope_hex\":\"");
        if (k) {
            k += strlen("\"envelope_hex\":\"");
            const char *e = strchr(k, '"');
            size_t hn = e ? (size_t)(e - k) : 0;
            uint8_t *env = malloc(hn / 2 + 1);
            int en = env ? hex_decode(k, hn, env, hn / 2 + 1) : -1;
            client_init(c, slot);
            ok = en > 0 && client_adopt_envelope(c, env, en) >= 0;
            free(env);
        }
    }
    cb_buf_free(&out);
    return ok;
}

typedef struct { uint32_t rating, games; } board_row;

static bool board_of(const char *id, board_row *out) {
    cb_buf b;
    cb_buf_init(&b);
    bool found = false;
    if (req(&g_dev, "GET", "/v1/leaderboard", NULL, NULL, 0, &b) == 200) {
        int k = nw_list_open(b.p, (int)b.len, NW_BOARD);
        for (int i = 0; i < k && !found; i++) {
            NwBoardRow r;
            if (nw_get_board(b.p, (int)b.len, i, &r) == NW_OK && r.is_bot && r.id_len == strlen(id) && memcmp(r.id, id, r.id_len) == 0) {
                out->rating = r.rating;
                out->games = r.games;
                found = true;
            }
        }
    }
    cb_buf_free(&b);
    return found;
}

// A table of `n` bots (taken from offset `first` of the roster) to its fool.
static void bot_only_game(int first, int n, int passing) {
    char all[16][NW_ID_MAX + 1], ids[MAX_PLAYERS][NW_ID_MAX + 1];
    T_CHECK(bots(&g_dev, all, 16) >= first + n);
    for (int i = 0; i < n; i++) snprintf(ids[i], sizeof ids[i], "%s", all[first + i]);
    uint8_t b[512];
    size_t bn = body_for(b, passing, n, ids);
    cb_buf out;
    cb_buf_init(&out);
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, b, bn, &out), 200);
    char gid[64] = { 0 };
    T_CHECK(out.len >= 2 && out.p[0] == out.len - 1);
    if (out.len >= 2 && out.p[0] == out.len - 1) memcpy(gid, out.p + 1, out.p[0]);
    cb_buf_free(&out);
    static ClientTable c;
    static ClientSlot slot;
    // Dealt at once, every seat a bot, under the rules asked for.
    T_CHECK(game_view(gid, &c, &slot));
    T_CHECK(c.view.status == GAME_STATUS_PLAYING || c.view.status == GAME_STATUS_GAME_OVER);
    T_EQ_INT(c.view.num_players, n);
    for (int i = 0; i < c.view.num_players; i++) {
        T_CHECK(c.view.seats[i].is_ai);
        T_EQ_STR(c.view.seats[i].id, ids[i]);
    }
    T_EQ_INT(c.view.passing, passing);
    // Played by the bot loop, nobody asking, to its fool.
    int64_t until = cb_now_ms() + 60000;
    while (cb_now_ms() < until && !(game_view(gid, &c, &slot) && c.view.status == GAME_STATUS_GAME_OVER && c.view.rated)) usleep(100000);
    T_EQ_INT(c.view.status, GAME_STATUS_GAME_OVER);
    T_CHECK(c.view.fool >= -1 && c.view.fool < n);
    T_CHECK(c.view.rated);
    // The stamp is the leaderboard's: each bot's rating is its before plus its delta, one game more.
    int moved = 0;
    for (int i = 0; i < n; i++) {
        board_row r = { 0, 0 };
        T_CHECK(board_of(ids[i], &r));
        T_EQ_INT(r.rating, c.view.rating_after[i]);
        T_EQ_INT(r.rating, c.view.rating_before[i] + c.view.rating_delta[i]);
        T_EQ_INT(r.games, 1);
        if (c.view.rating_delta[i] != 0) moved++;
    }
    T_CHECK(moved > 0);
    if (c.view.fool >= 0) T_CHECK(c.view.rating_delta[c.view.fool] < 0);
}

static void two_bots_classic(void) { bot_only_game(0, 2, 1); }
static void three_bots_podkidnoy(void) { bot_only_game(2, 3, 0); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s build/cbase\n", argv[0]); return 2; }
    g_bin = argv[1];
    T_RUN(absent_without_dev);
    T_RUN(gate_and_bytes);
    T_RUN(two_bots_classic);
    T_RUN(three_bots_podkidnoy);
    tp_stop(&g_dev.child, 15, 10000);
    return T_DONE();
}
