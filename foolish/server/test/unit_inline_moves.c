// unit_inline_moves.c - moves on the network loop's thread (net/server.h inline dispatch) against the
// real SQLite store and its journal, over a loopback `cbase serve` server: a move the loop can make
// alone is made there, and every step that would wait (table_ops.c "THE LOOP NEVER WAITS") sends the
// request to the pool instead, with the loop serving everyone else meanwhile.
#include "t.h"
#include "app/app_internal.h"
#include "app/functions_http.h"
#include "app/table_ops.h"
#include "cbase/provider.h"
#include "handler.h"
#include "lib_client.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "rate_limits.h"
#include "store/journal.h"
#include "store/store_sql.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"
#include "awire.h"
#include "legal.h"
#include "roster.h"
#include "table.h"
#include "../../../shared/c/rng.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char UID_A[] = "aaaaaaaa-0000-4000-8000-000000000001";
static const char UID_B[] = "bbbbbbbb-0000-4000-8000-000000000002";

static char g_dir[256], g_db[300];
static cb_config g_cfg;
static cb_ctx g_ctx;
static int g_port;
static pthread_t g_loop_thr;

static int fake_verify(cb_ctx *ctx, const struct cb_http_req *req, cb_user *out) {
    const char *b = cb_http_bearer(req);
    memset(out, 0, sizeof *out);
    if (!b) return CB_E_AUTH;
    if (strcmp(b, "A") == 0) { snprintf(out->id, sizeof out->id, "%s", UID_A); snprintf(out->username, sizeof out->username, "ALICE"); return CB_OK; }
    if (strcmp(b, "B") == 0) { snprintf(out->id, sizeof out->id, "%s", UID_B); snprintf(out->username, sizeof out->username, "BOB"); return CB_OK; }
    return CB_E_AUTH;
}

// A finished game's replay job (table_ops.h CB_FINALIZE_JOB): held, never run here, so the game
// stays pending for whoever needs its replay next.
static atomic_int g_fin_jobs;
static int held_jobs(cb_ctx *ctx, const char *job, const char *key) {
    if (strcmp(job, CB_FINALIZE_JOB) == 0) atomic_fetch_add(&g_fin_jobs, 1);
    return CB_OK;
}

// The seats' ratings read (a read of the tables): never on the loop thread.
static int (*g_ratings_for)(cb_store *s, const char *const *ids, int n, int32_t *out);
static atomic_int g_ratings_reads, g_ratings_on_loop;
static int spy_ratings_for(cb_store *s, const char *const *ids, int n, int32_t *out) {
    atomic_fetch_add(&g_ratings_reads, 1);
    if (cb_loop_on_thread(g_ctx.loop)) atomic_fetch_add(&g_ratings_on_loop, 1);
    return g_ratings_for(s, ids, n, out);
}

static void add_user(cb_store *s, const char *id, const char *name) {
    cb_user_row u;
    memset(&u, 0, sizeof u);
    snprintf(u.id, sizeof u.id, "%s", id);
    snprintf(u.email, sizeof u.email, "%s@x.io", name);
    snprintf(u.password_hash, sizeof u.password_hash, "$2b$10$abcdefghijklmnopqrstuuabcdefghijklmnopqrstuvwxyz01234");
    snprintf(u.username, sizeof u.username, "%s", name);
    u.created_at = 1;
    CB_STORE_CALL(s, user_create, &u);
}

// ---- requests on the test's thread, before the loop runs (setting a game up) ----------------

typedef struct { int status; uint8_t body[4096]; size_t len; } reply;

static void call(const char *fn, const char *bearer, const void *body, size_t n, reply *out) {
    cb_http_req req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "POST");
    char path[64], auth[64];
    snprintf(path, sizeof path, "/v1/%s", fn);
    req.path = path;
    req.query = "";
    snprintf(auth, sizeof auth, "Bearer %s", bearer);
    req.headers[req.n_headers++] = (cb_http_header){ "Authorization", auth };
    req.headers[req.n_headers++] = (cb_http_header){ "Content-Type", "application/octet-stream" };
    req.body = body;
    req.body_len = n;
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&g_ctx, &req, &res);
    out->status = res.status;
    out->len = res.body.len < sizeof out->body ? res.body.len : sizeof out->body;
    if (out->len) memcpy(out->body, res.body.p, out->len);
    cb_res_free(&res);
}

static void meta(const char *bearer, int kind, const char *gid, reply *out) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = (uint8_t)kind;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    call("meta", bearer, body, n > 0 ? (size_t)n : 0, out);
}

static bool first_game(const cb_view_row *row, void *ud) {
    snprintf((char *)ud, CB_GAME_ID_MAX + 1, "%s", row->game_id);
    return false;
}

static void two_humans_dealt(char gid[CB_GAME_ID_MAX + 1]) {
    reply r;
    call("create", "A", NULL, 0, &r);
    T_EQ_INT(r.status, 200);
    gid[0] = 0;
    cb_table_views_of_user(&g_ctx, UID_A, 1, first_game, gid);
    meta("B", ASK_JOIN, gid, &r);
    meta("A", ASK_START, gid, &r);
    meta("B", ASK_START, gid, &r);
    T_CHECK(gid[0] != 0);
}

// ---- the world: the store, the journal on the loop, the server ------------------------------

static void *run_loop(void *ud) {
    cb_loop_run(g_ctx.loop);
    return NULL;
}

// The store and its users, and the ctx; the loop made (the journal names it) but not running.
static void world_open(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    snprintf(g_dir, sizeof g_dir, "%s/cbase-inl-XXXXXX", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    T_CHECK(mkdtemp(g_dir) != NULL);
    snprintf(g_db, sizeof g_db, "%s/db.sqlite", g_dir);
    cb_store *s = NULL;
    T_EQ_INT(cb_store_open_sqlite(g_db, &s), CB_OK);
    add_user(s, UID_A, "ALICE");
    add_user(s, UID_B, "BOB");
    char e[256];
    cb_config_from_env(&g_cfg, e, sizeof e);
    strcpy(g_cfg.allowed_origins, "*");
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = &cb_provider_local;
    g_ctx.store = s;
    g_ctx.job_start = held_jobs;
    g_ctx.router = cb_router_new();
    T_EQ_INT(cb_functions_routes(&g_ctx), CB_OK);
    cb_functions_verify = fake_verify;
    cb_limits_reset();
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(1, "tinm");
}

static void journal_open(void) {
    cb_journal_opts o = { .path = g_db, .game_age_ms = 3600000, .loop = g_ctx.loop, .apply = cb_table_replay_move, .apply_ud = &g_ctx };
    char err[256];
    T_EQ_INT(cb_journal_open(g_ctx.store, &o, err, sizeof err), CB_OK);
    cb_rows_ops *ops = (cb_rows_ops *)cb_journal_rows();   // the journal's table: its ratings read spied on
    if (ops->ratings_for != spy_ratings_for) {
        g_ratings_for = ops->ratings_for;
        ops->ratings_for = spy_ratings_for;
    }
}

static void h_health(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);
static void serve(void) {
    T_EQ_INT(cb_router_add(g_ctx.router, "GET", "/t/health", CB_ROUTE_LOOP, h_health), CB_OK);
    cb_server_opts o;
    cb_server_opts_default(&o);
    g_ctx.server = cb_server_new(&g_ctx, &o);
    T_EQ_INT(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &g_port), CB_OK);
    pthread_create(&g_loop_thr, NULL, run_loop, NULL);
}

static void drain_post(cb_loop *l, void *ud) { cb_server_drain(g_ctx.server, 2000); }

static void world_close(void) {
    T_EQ_INT(cb_loop_post(g_ctx.loop, drain_post, NULL), CB_OK);
    pthread_join(g_loop_thr, NULL);
    cb_pool_free(g_ctx.req_pool);
    cb_server_free(g_ctx.server);
    cb_journal_close(g_ctx.store);
    cb_store_close(g_ctx.store);
    cb_router_free(g_ctx.router);
    cb_loop_free(g_ctx.loop);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    T_EQ_INT(system(cmd), 0);
    memset(&g_ctx, 0, sizeof g_ctx);
}

// ---- over the socket ------------------------------------------------------------------------

// The server's inline counters, read on the loop (it owns them): a loop route.
static void h_health(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_inline_stats st;
    cb_server_inline_stats(ctx->server, &st);
    char b[128];
    int n = snprintf(b, sizeof b, "%lld %lld %lld", (long long)st.ran, (long long)st.to_pool, (long long)st.tripped);
    cb_res_send(res, 200, "text/plain", b, (size_t)n);
}

typedef struct { long long ran, to_pool, tripped; int64_t took_us; bool ok; } counters;

// The counters, and how long the loop took to answer (it answers only between its other work).
static counters stats(void) {
    counters c = { 0, 0, 0, 0, false };
    int fd = tc_connect(g_port);
    tc_response r;
    int64_t t0 = cb_mono_us();
    if (fd >= 0 && tc_request(fd, "GET", "/t/health", NULL, NULL, 0, 1000, &r) == 0) {
        c.took_us = cb_mono_us() - t0;
        c.ok = r.status == 200 && sscanf(cb_buf_cstr(&r.body), "%lld %lld %lld", &c.ran, &c.to_pool, &c.tripped) == 3;
        tc_response_free(&r);
    }
    if (fd >= 0) close(fd);
    return c;
}

// The move on the stored board for whoever has one, as POST /v1/action's body. Its length, or 0.
static uint64_t g_rng = 4242;
static int next_move(const char *gid, const char **bearer, uint8_t *body, int cap) {
    static const char *const uids[2] = { UID_A, UID_B }, *const bearers[2] = { "A", "B" };
    cb_game_row row;
    if (CB_ROWS_CALL(g_ctx.store, game_get, gid, &row) != CB_OK) return 0;
    int n = 0;
    Game *g = calloc(1, sizeof *g);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    LegalMoves *moves = calloc(1, sizeof *moves);
    Table t;
    table_init(&t, g, snaps);
    if (row.status == GAME_STATUS_PLAYING && table_load(&t, row.state, (int)row.state_len, row.roster, (int)row.roster_len) == TABLE_OK) {
        for (int k = 0; k < 2 && n == 0; k++) {
            const int seat = table_seat_of(&t, uids[k], (int)strlen(uids[k]));
            if (seat < 0) continue;
            calculate_legal_moves(g, seat, moves);
            int live = 0;
            for (int i = 0; i < moves->n; i++) live += moves->moves[i].type != MOVE_WAIT;
            if (!live) continue;
            int want = (int)(rng_u32(&g_rng) % (uint32_t)live);
            for (int i = 0; i < moves->n; i++) {
                const LegalMove *m = &moves->moves[i];
                if (m->type == MOVE_WAIT || want--) continue;
                AwireAction a;
                memset(&a, 0, sizeof a);
                a.kind = m->type;
                a.n = m->n_cards;
                for (int c = 0; c < m->n_cards; c++) { a.cards[c] = m->cards[c]; a.attacks[c] = m->attack_cards[c]; }
                const int gl = (int)strlen(gid);
                memcpy(body, gid, (size_t)gl);
                int wl = awire_encode(&a, body + gl, cap - gl);
                n = wl > 0 ? table_request_encode(body, cap, gl, wl, true, (uint32_t)row.version) : 0;
                *bearer = bearers[k];
                break;
            }
        }
    }
    free(g);
    free(snaps);
    free(moves);
    cb_game_row_free(&row);
    return n;
}

// POST /v1/action over the socket: the HTTP status, and the kernel's answer when it is 200.
static int post_action(const char *bearer, const uint8_t *body, int n, TableResponse *resp) {
    char hdr[128];
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\nContent-Type: application/octet-stream\r\n", bearer);
    tc_response r;
    if (tc_http(g_port, "POST", "/v1/action", hdr, body, (size_t)n, &r) != 0) return -1;
    int st = r.status;
    if (st == 200 && resp && table_response_decode(r.body.p, (int)r.body.len, resp) != 0) st = -2;
    tc_response_free(&r);
    return st;
}

typedef struct { const char *bearer; uint8_t body[256]; int n; int status; TableResponse resp; atomic_bool done; } mover;
static void *move_thread(void *ud) {
    mover *m = ud;
    m->status = post_action(m->bearer, m->body, m->n, &m->resp);
    atomic_store(&m->done, true);
    return NULL;
}

// ---- the tests ------------------------------------------------------------------------------

// A move whose game's lock another thread holds (a bot drive's commit, a pooled request): the
// loop does not wait for it (cb_table_trylock), the move goes to the pool, which waits there, and
// the loop answers everyone else meanwhile; the move lands once the lock is free.
static void a_move_on_a_locked_game_waits_on_the_pool(void) {
    world_open();
    journal_open();
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    serve();
    mover m;
    memset(&m, 0, sizeof m);
    m.n = next_move(gid, &m.bearer, m.body, sizeof m.body);
    T_CHECK(m.n > 0);
    counters c0 = stats();
    T_CHECK(c0.ok);
    cb_game_lock *held = cb_table_lock(gid);
    pthread_t th;
    pthread_create(&th, NULL, move_thread, &m);
    usleep(150000);   // on the pool by now, waiting for the lock
    counters c1 = stats();
    T_CHECK(c1.ok);
    T_CHECK(c1.took_us < 100000);   // the loop answered: it is not the one waiting
    T_EQ_INT((int)(c1.to_pool - c0.to_pool), 1);
    T_CHECK(!atomic_load(&m.done));
    cb_table_unlock(held);
    pthread_join(th, NULL);
    T_EQ_INT(m.status, 200);
    T_EQ_INT(m.resp.status, TABLE_STATUS_APPLIED);
    // The next move finds the lock free: on the loop, nothing sent on.
    m.n = next_move(gid, &m.bearer, m.body, sizeof m.body);
    T_CHECK(m.n > 0);
    T_EQ_INT(post_action(m.bearer, m.body, m.n, &m.resp), 200);
    T_EQ_INT(m.resp.status, TABLE_STATUS_APPLIED);
    counters c2 = stats();
    T_EQ_INT((int)(c2.to_pool - c1.to_pool), 0);
    T_EQ_INT((int)(c2.ran - c1.ran), 1);   // the first one's run on the loop was counted before c1
    world_close();
}

// A game the journal does not hold (made before it opened, as after an eviction): its first move
// would read the tables, so it is made on the pool, which makes the game resident; the moves after
// it are the loop's alone.
static void a_game_not_resident_is_read_on_the_pool(void) {
    world_open();
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);   // no journal yet: the game is the tables' only
    journal_open();
    serve();
    counters c0 = stats();
    for (int i = 0; i < 3; i++) {
        const char *b;
        uint8_t body[256];
        int n = next_move(gid, &b, body, sizeof body);
        T_CHECK(n > 0);
        TableResponse resp;
        T_EQ_INT(post_action(b, body, n, &resp), 200);
        T_EQ_INT(resp.status, TABLE_STATUS_APPLIED);
    }
    counters c1 = stats();
    T_EQ_INT((int)(c1.to_pool - c0.to_pool), 1);
    T_EQ_INT((int)(c1.ran - c0.ran), 3);
    world_close();
}

// A game played to its end over the socket: every move on the loop, but the one that rates it
// (the accounts and the ratings are the tables', read and written: on the pool), and a move after
// the end, while the replay job has not run (it would make the replay first: on the pool too).
static void the_rated_end_and_a_pending_replay_go_to_the_pool(void) {
    world_open();
    journal_open();
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    serve();
    atomic_store(&g_ratings_reads, 0);
    atomic_store(&g_ratings_on_loop, 0);
    atomic_store(&g_fin_jobs, 0);
    counters c0 = stats();
    int moves = 0, last = -1;
    for (; moves < 3000; moves++) {
        const char *b;
        uint8_t body[256];
        int n = next_move(gid, &b, body, sizeof body);
        if (n <= 0) break;
        TableResponse resp;
        T_EQ_INT(post_action(b, body, n, &resp), 200);
        last = resp.status;
    }
    cb_game_row row;
    T_EQ_INT(CB_ROWS_CALL(g_ctx.store, game_get, gid, &row), CB_OK);
    T_EQ_INT(row.status, GAME_STATUS_GAME_OVER);
    cb_game_row_free(&row);
    T_EQ_INT(last, TABLE_STATUS_APPLIED);
    counters c1 = stats();
    T_CHECK(moves > 4);
    T_EQ_INT((int)(c1.ran - c0.ran), moves);
    T_EQ_INT((int)(c1.to_pool - c0.to_pool), 1);
    T_CHECK(atomic_load(&g_ratings_reads) >= 1);
    T_EQ_INT(atomic_load(&g_ratings_on_loop), 0);
    T_EQ_INT(atomic_load(&g_fin_jobs), 1);
    T_EQ_INT((int)c1.tripped, 0);
    // A move on the finished table (MOOT), the replay still a queued job: made first, on the pool.
    const uint8_t pickup[] = { AWIRE_PICKUP, 0 };
    uint8_t body[64];
    const int gl = (int)strlen(gid);
    memcpy(body, gid, (size_t)gl);
    memcpy(body + gl, pickup, sizeof pickup);
    int n = table_request_encode(body, sizeof body, gl, (int)sizeof pickup, true, 0);
    TableResponse resp;
    T_EQ_INT(post_action("A", body, n, &resp), 200);
    T_EQ_INT(resp.status, TABLE_STATUS_MOOT);
    counters c2 = stats();
    T_EQ_INT((int)(c2.to_pool - c1.to_pool), 1);
    T_CHECK(!cb_table_finalize_wait(&g_ctx, gid));   // made, by that move on the pool
    world_close();
}

// The journal's own guards (journal.h opts.loop), called on the loop thread itself: a read of a
// game it does not hold and a write around it (a delete) answer CB_E_WOULD_BLOCK having done
// nothing; a read of a game it holds is answered from memory. Without the journal, the store's
// every read is refused on the loop too (cb_table_view).
typedef struct { const char *gid; int get_missing, del, view; atomic_bool done; } loop_calls;
static void loop_calls_fn(cb_loop *l, void *ud) {
    loop_calls *c = ud;
    cb_game_row row;
    c->get_missing = CB_ROWS_CALL(g_ctx.store, game_get, "nosuchgame", &row);
    if (c->get_missing == CB_OK) cb_game_row_free(&row);
    c->del = cb_journal_of(g_ctx.store) ? CB_ROWS_CALL(g_ctx.store, game_delete, c->gid, 1) : CB_E_WOULD_BLOCK;
    cb_view_row v;
    c->view = cb_table_view(&g_ctx, c->gid, NULL, &v);
    if (c->view == CB_OK) cb_view_row_free(&v);
    atomic_store(&c->done, true);
}

static void on_loop(loop_calls *c) {
    atomic_store(&c->done, false);
    T_EQ_INT(cb_loop_post(g_ctx.loop, loop_calls_fn, c), CB_OK);
    for (int i = 0; i < 2000 && !atomic_load(&c->done); i++) usleep(1000);
    T_CHECK(atomic_load(&c->done));
}

static void the_store_is_never_read_or_written_on_the_loop(void) {
    world_open();
    journal_open();
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    serve();
    loop_calls c = { .gid = gid };
    on_loop(&c);
    T_EQ_INT(c.get_missing, CB_E_WOULD_BLOCK);
    T_EQ_INT(c.del, CB_E_WOULD_BLOCK);
    T_EQ_INT(c.view, CB_OK);   // resident: no read of the tables
    cb_game_row row;
    T_EQ_INT(CB_ROWS_CALL(g_ctx.store, game_get, gid, &row), CB_OK);   // the delete did nothing
    cb_game_row_free(&row);
    T_EQ_INT(CB_ROWS_CALL(g_ctx.store, game_get, "nosuchgame", &row), CB_E_NOT_FOUND);   // off the loop: the tables answer
    world_close();
    // No journal: every read is the store's, so nothing of a game is read on the loop, and every
    // move runs on the pool.
    world_open();
    two_humans_dealt(gid);
    serve();
    c.gid = gid;
    on_loop(&c);
    T_EQ_INT(c.view, CB_E_WOULD_BLOCK);
    counters c0 = stats();
    for (int i = 0; i < 2; i++) {
        const char *b;
        uint8_t body[256];
        int n = next_move(gid, &b, body, sizeof body);
        T_CHECK(n > 0);
        TableResponse resp;
        T_EQ_INT(post_action(b, body, n, &resp), 200);
        T_EQ_INT(resp.status, TABLE_STATUS_APPLIED);
    }
    counters c1 = stats();
    T_EQ_INT((int)(c1.to_pool - c0.to_pool), 2);
    world_close();
}

int main(void) {
    T_RUN(a_move_on_a_locked_game_waits_on_the_pool);
    T_RUN(a_game_not_resident_is_read_on_the_pool);
    T_RUN(the_rated_end_and_a_pending_replay_go_to_the_pool);
    T_RUN(the_store_is_never_read_or_written_on_the_loop);
    return T_DONE();
}
