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
//     the stamp agree to the point. Format 2 seats a signed-up human with a bot: the table waits
//     in its lobby (the bot READY, the human not, nobody dealt) until the human's own ASK_START
//     deals it, and then the bot plays; an id that names no account is refused (400).
#include "t.h"

#include "lib_client.h"
#include "lib_proc.h"

#include "ask.h"
#include "awire.h"
#include "client_table.h"
#include "legal.h"
#include "table.h"
#include "netwire.h"
#include "util/clock.h"

#include "../../../shared/c/hex/hex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ADMIN "dev-bot-table-admin-token-0123456789abcdef"
#define DEV_BOT_TABLE_FORMAT   1   // dev_bot_table.h's; the test speaks the bytes as a client does
#define DEV_BOT_TABLE_FORMAT_2 2
#define DEV_SEAT_BOT   0
#define DEV_SEAT_HUMAN 1

static const char *g_bin;

typedef struct {
    tp_child child;
    int      port;
    char     dir[400];
} server;

static const char *g_think;   // CBASE_BOT_THINK_MS for the next dev server, NULL none

static bool start(server *s, bool dev) {
    if (tp_tempdir(s->dir, sizeof s->dir) != 0) return false;
    static char envdb[2][512];
    snprintf(envdb[dev], sizeof envdb[dev], "CBASE_DB=%s/cbase.sqlite", s->dir);
    static char think[64];
    snprintf(think, sizeof think, "CBASE_BOT_THINK_MS=%s", g_think ? g_think : "");
    char *dev_env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", envdb[1], "CBASE_JWT_SECRET=devbot-secret-devbot-secret-devbot-secret",
                        "CBASE_ADMIN_TOKEN=" ADMIN, g_think ? "CBASE_BOT_SPEEDUP=1" : "CBASE_BOT_SPEEDUP=100", "CBASE_RATED_MIN_AGE=0",
                        g_think ? think : NULL, NULL };
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
static char g_user_id[NW_ID_MAX + 1];

// Signs `name` up as a client does: its access token into g_user_token, its user id into g_user_id.
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
    if (ok) {
        snprintf(g_user_token, sizeof g_user_token, "%.*s", (int)ss.access_len, ss.access);
        snprintf(g_user_id, sizeof g_user_id, "%.*s", (int)ss.user_id_len, ss.user_id);
    }
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

// CBASE_BOT_THINK_MS (dev-only): a bot thinks 1.0 to 1.2 s after each move it was shown, so a
// table of bots alone moves a few times in four seconds instead of every 300 ms (the kernel's
// bots-only pace: some 13 moves in the same time).
static void think_time_paces_the_bots(void) {
    tp_stop(&g_dev.child, 15, 10000);
    g_think = "1000-1200";
    T_CHECK(start(&g_dev, true));
    char ids[2][NW_ID_MAX + 1];
    T_EQ_INT(bots(&g_dev, ids, 2), 2);
    uint8_t b[256];
    size_t n = body_for(b, 1, 2, ids);
    cb_buf out;
    cb_buf_init(&out);
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, b, n, &out), 200);
    char gid[64] = { 0 };
    if (out.len >= 2 && out.p[0] == out.len - 1) memcpy(gid, out.p + 1, out.p[0]);
    cb_buf_free(&out);
    static ClientTable c;
    static ClientSlot slot;
    T_CHECK(game_view(gid, &c, &slot));
    uint32_t v0 = c.view.version;
    usleep(4500000);
    T_CHECK(game_view(gid, &c, &slot));
    uint32_t moved = c.view.version - v0;
    fprintf(stderr, "think: %u commits in 4.5 s\n", moved);
    T_CHECK(moved >= 2);   // it does move
    T_CHECK(moved <= 6);   // at a think's pace, not the kernel's 300 ms
}
static void three_bots_podkidnoy(void) { bot_only_game(2, 3, 0); }
// A full table: the house can seat seven, the first bot seats the eighth.
static void eight_bots_classic(void) { bot_only_game(5, 8, 1); }

// ---- format 2: a human at a table of bots ------------------------------------------------

// The format 2 body: each seat its kind, then its id.
static size_t body2_for(uint8_t *b, int passing, int n, char ids[][NW_ID_MAX + 1], const uint8_t *kinds) {
    size_t at = 0;
    b[at++] = DEV_BOT_TABLE_FORMAT_2;
    b[at++] = (uint8_t)passing;
    b[at++] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(ids[i]);
        b[at++] = kinds[i];
        b[at++] = (uint8_t)l;
        memcpy(b + at, ids[i], l);
        at += l;
    }
    return at;
}

// The human's own view of gid over GET /v1/games/<gid>, as the web UI fetches it.
static bool human_view(const char *gid, ClientTable *c, ClientSlot *slot) {
    char path[128];
    snprintf(path, sizeof path, "/v1/games/%s", gid);
    cb_buf out;
    cb_buf_init(&out);
    bool ok = false;
    uint32_t version;
    NwBlob env;
    if (req(&g_dev, "GET", path, g_user_token, NULL, 0, &out) == 200 && nw_view_decode(out.p, (int)out.len, &version, &env) == NW_OK) {
        client_init(c, slot);
        ok = client_adopt_envelope(c, env.p, (int)env.len) >= 0;
    }
    cb_buf_free(&out);
    return ok;
}

// The human's Start (ASK_START over /v1/meta, the lobby's button). The status.
static int human_start(const char *gid) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = ASK_START;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    cb_buf out;
    cb_buf_init(&out);
    int st = n > 0 ? req(&g_dev, "POST", "/v1/meta", g_user_token, body, (size_t)n, &out) : -1;
    cb_buf_free(&out);
    return st;
}

// One legal move of the human's on c's board, as the web UI offers one, over /v1/action. The
// table's version after it into *version; false when the human has none or it was not applied.
static bool human_move(const char *gid, ClientTable *c, uint32_t *version) {
    static LegalMoves moves;
    const int seat = c->view.my_seat;
    if (seat < 0 || c->view.status != GAME_STATUS_PLAYING) return false;
    calculate_legal_moves(c->g, seat, &moves);
    for (int k = 0; k < moves.n; k++) {
        const LegalMove *m = &moves.moves[k];
        if (m->type == MOVE_WAIT) continue;
        AwireAction a;
        memset(&a, 0, sizeof a);
        a.kind = m->type;
        a.n = m->n_cards;
        for (int i = 0; i < m->n_cards; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
        uint8_t wire[128], buf[256];
        int wn = awire_encode(&a, wire, sizeof wire);
        if (wn <= 0 || client_validate(c, &c->view, wire, wn) != 0) continue;
        int gl = (int)strlen(gid);
        memcpy(buf, gid, (size_t)gl);
        memcpy(buf + gl, wire, (size_t)wn);
        int n = table_request_encode(buf, sizeof buf, gl, wn, true, c->view.version);
        cb_buf out;
        cb_buf_init(&out);
        TableResponse r;
        bool ok = n > 0 && req(&g_dev, "POST", "/v1/action", g_user_token, buf, (size_t)n, &out) == 200 &&
                  table_response_decode(out.p, (int)out.len, &r) == 0 && r.status == TABLE_STATUS_APPLIED;
        if (ok) *version = r.version;
        cb_buf_free(&out);
        return ok;
    }
    return false;
}

// One human and one bot: seated in the order given, the bot READY, the human not, the house
// gone, nothing dealt; the human's Start deals; then the bot plays (a version the human did not
// commit). Under CBASE_BOT_SPEEDUP=100.
static void human_and_bot(void) {
    T_CHECK(signup("DEVBOT_HUMAN"));
    char roster[2][NW_ID_MAX + 1], ids[2][NW_ID_MAX + 1];
    T_EQ_INT(bots(&g_dev, roster, 2), 2);
    snprintf(ids[0], sizeof ids[0], "%s", g_user_id);
    snprintf(ids[1], sizeof ids[1], "%s", roster[1]);
    const uint8_t kinds[2] = { DEV_SEAT_HUMAN, DEV_SEAT_BOT };
    uint8_t b[256];
    size_t bn = body2_for(b, 1, 2, ids, kinds);
    static const char TITLE[] = "Round of 32 - Game 1 of 5";
    b[bn++] = (uint8_t)strlen(TITLE);   // format 2's optional title
    memcpy(b + bn, TITLE, strlen(TITLE));
    bn += strlen(TITLE);
    cb_buf out;
    cb_buf_init(&out);
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, b, bn, &out), 200);
    char gid[64] = { 0 };
    T_CHECK(out.len >= 2 && out.p[0] == out.len - 1);
    if (out.len >= 2 && out.p[0] == out.len - 1) memcpy(gid, out.p + 1, out.p[0]);
    cb_buf_free(&out);
    static ClientTable c;
    static ClientSlot slot;
    // The lobby, as the human's web UI sees it.
    T_CHECK(human_view(gid, &c, &slot));
    T_EQ_INT(c.view.status, GAME_STATUS_WAITING);
    T_EQ_INT(c.view.num_players, 2);
    T_EQ_INT(c.view.my_seat, 0);
    T_EQ_STR(c.view.title, TITLE);   // not "house's Game"
    T_EQ_STR(c.view.seats[0].id, g_user_id);
    T_EQ_STR(c.view.seats[0].name, "DEVBOT_HUMAN");
    T_CHECK(!c.view.seats[0].is_ai);
    T_CHECK(c.view.seats[0].status != PLAYER_STATUS_READY);
    T_EQ_STR(c.view.seats[1].id, ids[1]);
    T_CHECK(c.view.seats[1].is_ai);
    T_EQ_INT(c.view.seats[1].status, PLAYER_STATUS_READY);
    // A moment later still nobody dealt it (no bot loop, no timer readies the human).
    usleep(300000);
    T_CHECK(human_view(gid, &c, &slot));
    T_EQ_INT(c.view.status, GAME_STATUS_WAITING);
    // The human's Start deals.
    T_EQ_INT(human_start(gid), 200);
    T_CHECK(human_view(gid, &c, &slot));
    T_CHECK(c.view.status == GAME_STATUS_PLAYING || c.view.status == GAME_STATUS_GAME_OVER);
    T_EQ_INT(c.view.my_seat, 0);
    T_CHECK(c.view.my_hand_count > 0);
    // The bot plays: a version past every one the human committed. The human moves whenever it
    // has a move, as a player would, so a game the human leads goes on too.
    uint32_t ours = c.view.version;
    bool bot_moved = false;
    int64_t until = cb_now_ms() + 15000;
    while (cb_now_ms() < until && !bot_moved) {
        if (!human_view(gid, &c, &slot)) { usleep(50000); continue; }
        if (c.view.version > ours) { bot_moved = true; break; }
        if (c.view.status != GAME_STATUS_PLAYING) break;
        uint32_t v;
        if (human_move(gid, &c, &v)) ours = v;
        else usleep(50000);
    }
    T_CHECK(bot_moved);
}

// An id that names no account, a bot's id under the human kind, a human's id under the bot kind,
// and a kind that is neither.
static void human_refusals(void) {
    char roster[1][NW_ID_MAX + 1], ids[2][NW_ID_MAX + 1];
    T_EQ_INT(bots(&g_dev, roster, 1), 1);
    uint8_t kinds[2] = { DEV_SEAT_HUMAN, DEV_SEAT_BOT };
    uint8_t b[256];
    cb_buf out;
    cb_buf_init(&out);
    snprintf(ids[0], sizeof ids[0], "00000000-0000-4000-8000-0000000000aa");
    snprintf(ids[1], sizeof ids[1], "%s", roster[0]);
    size_t bn = body2_for(b, 1, 2, ids, kinds);
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, b, bn, &out), 400);
    T_EQ_INT(refusal_why(&out), ASK_WHY_MALFORMED);
    snprintf(ids[0], sizeof ids[0], "%s", roster[0]);
    bn = body2_for(b, 1, 2, ids, kinds);
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, b, bn, &out), 400);
    T_EQ_INT(refusal_why(&out), ASK_WHY_MALFORMED);
    snprintf(ids[0], sizeof ids[0], "%s", g_user_id);
    kinds[0] = DEV_SEAT_BOT;
    bn = body2_for(b, 1, 2, ids, kinds);
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, b, bn, &out), 400);
    T_EQ_INT(refusal_why(&out), ASK_WHY_NO_BOT);
    kinds[0] = 2;
    bn = body2_for(b, 1, 2, ids, kinds);
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, b, bn, &out), 400);
    T_EQ_INT(refusal_why(&out), ASK_WHY_MALFORMED);
    // A title over ROSTER_TITLE_MAX bytes, and one whose length runs past the body.
    kinds[0] = DEV_SEAT_HUMAN;
    uint8_t big[512];
    size_t gn = body2_for(big, 1, 2, ids, kinds);
    big[gn++] = ROSTER_TITLE_MAX + 1;
    memset(big + gn, 'a', ROSTER_TITLE_MAX + 1);
    gn += ROSTER_TITLE_MAX + 1;
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, big, gn, &out), 400);
    T_EQ_INT(refusal_why(&out), ASK_WHY_MALFORMED);
    gn = body2_for(big, 1, 2, ids, kinds);
    big[gn++] = 5;
    big[gn++] = 'a';
    T_EQ_INT(req(&g_dev, "POST", "/dev/bot_table", ADMIN, big, gn, &out), 400);
    T_EQ_INT(refusal_why(&out), ASK_WHY_MALFORMED);
    cb_buf_free(&out);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s build/cbase\n", argv[0]); return 2; }
    g_bin = argv[1];
    T_RUN(absent_without_dev);
    T_RUN(gate_and_bytes);
    T_RUN(two_bots_classic);
    T_RUN(three_bots_podkidnoy);
    T_RUN(eight_bots_classic);
    T_RUN(human_and_bot);
    T_RUN(human_refusals);
    T_RUN(think_time_paces_the_bots);
    tp_stop(&g_dev.child, 15, 10000);
    return T_DONE();
}
