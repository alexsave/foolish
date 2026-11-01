// unit_inline_moves.c - moves on the network loop's thread (net/server.h inline dispatch) against the
// real SQLite store and its journal, over a loopback `cbase serve` server: a move the loop can make
// alone is made there, and every step that would wait (table_ops.c "THE LOOP NEVER WAITS") sends the
// request to the pool instead, with the loop serving everyone else meanwhile.
#include "t.h"
#include "lib_created_game.h"
#include "app/app_internal.h"
#include "app/functions_http.h"
#include "app/table_ops.h"
#include "app/bots.h"
#include "app/fanout.h"
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
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static const char UID_A[] = "aaaaaaaa-0000-4000-8000-000000000001";
static const char UID_B[] = "bbbbbbbb-0000-4000-8000-000000000002";

static char g_dir[256], g_db[300];
static cb_config g_cfg;
static cb_ctx g_ctx;
static int g_port;
static pthread_t g_loop_thr;

// The app's clock, faked (a bot drive waits out the kernel's pacing on it: no real sleep).
static _Atomic int64_t g_now = 1760000000000LL;
static int64_t fake_now(void) { return atomic_load(&g_now); }
static void fake_sleep(int64_t ms) { atomic_fetch_add(&g_now, ms); }

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

static bool all_games(const cb_view_row *row, void *ud) {
    struct { int n; char (*ids)[CB_GAME_ID_MAX + 1]; } *g = ud;
    snprintf(g->ids[g->n++], CB_GAME_ID_MAX + 1, "%s", row->game_id);
    return true;
}

static void two_humans_dealt(char gid[CB_GAME_ID_MAX + 1]) {
    reply r;
    call("create", "A", NULL, 0, &r);
    T_EQ_INT(r.status, 200);
    T_CHECK(tc_created_game(r.body, r.len, gid));
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
    cb_app_now = fake_now;
    cb_app_sleep = fake_sleep;
    cb_limits_reset();
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(1, "tinm");
}

static size_t g_seg_bytes;   // the journal's seg_bytes for the next open (0: its default)
static bool g_sync;          // and its sync
static void journal_open(void) {
    cb_journal_opts o = { .path = g_db, .game_age_ms = 3600000, .seg_bytes = (int64_t)g_seg_bytes, .sync = g_sync, .loop = g_ctx.loop,
                          .apply = cb_table_replay_move, .apply_ud = &g_ctx };
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
    // Served once the loop runs: until then the journal writes a commit on the committing thread
    // (there is no loop to hand it to), which a test counting the loop's writes would see - a bot
    // that moves first drives on this thread before any request needs the loop.
    for (int i = 0; i < 5000 && !cb_loop_running(g_ctx.loop); i++) usleep(1000);
    T_CHECK(cb_loop_running(g_ctx.loop));
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
    T_CHECK(c1.took_us < 500000);   // the loop answered: it is not the one waiting (it would be until the unlock)
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

// A game played to its end over the socket: every move on the loop, the one that rates it too
// (its seats' ratings were read at the deal, on the pool: journal.h cb_journal_seat_ratings, and
// its changes are a journal record), and the leaderboard's tables have them after the flush; a
// move after the end, while the replay job has not run, goes to the pool (it makes the replay
// first).
static void the_rated_end_stays_on_the_loop_and_a_pending_replay_goes_to_the_pool(void) {
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
    T_EQ_INT((int)(c1.to_pool - c0.to_pool), 0);
    T_EQ_INT(atomic_load(&g_ratings_reads), 0);   // read at the deal, before the counters
    T_EQ_INT(atomic_load(&g_ratings_on_loop), 0);
    // The end's ratings reach the tables at its game's flush (kicked by the end itself).
    T_EQ_INT(cb_journal_flush(g_ctx.store), CB_OK);
    const char *ids[2] = { UID_A, UID_B };
    int32_t rt[2];
    T_EQ_INT(cb_rows_sql.ratings_for(g_ctx.store, ids, 2, rt), CB_OK);
    T_CHECK(rt[0] != rt[1] && rt[0] + rt[1] == 2 * TABLE_RATING_START);
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

// ---- the journal's one writer: the loop (journal.c "the writer") ---------------------------

static cb_journal_stats jstats(void) {
    cb_journal_stats st;
    cb_journal_stats_get(g_ctx.store, &st);
    return st;
}

// A human (A) against a bot, dealt.
static void a_human_and_a_bot_dealt(char gid[CB_GAME_ID_MAX + 1]) {
    reply r;
    call("create", "A", NULL, 0, &r);
    T_EQ_INT(r.status, 200);
    T_CHECK(tc_created_game(r.body, r.len, gid));
    meta("A", ASK_ADD_BOT, gid, &r);
    meta("A", ASK_START, gid, &r);
    T_CHECK(gid[0] != 0);
}

// A's move over the socket when A has one. true when one was applied.
static bool a_moves(const char *gid) {
    const char *b;
    uint8_t body[256];
    int n = next_move(gid, &b, body, sizeof body);
    if (n <= 0 || strcmp(b, "A") != 0) return false;
    TableResponse resp;
    return post_action(b, body, n, &resp) == 200 && resp.status == TABLE_STATUS_APPLIED;
}

// While the loop runs it is the only writer: a bot drive's cycles, committed on another thread,
// are handed to it and written there, as the human's moves are.
static void a_drives_commits_are_written_by_the_loop(void) {
    world_open();
    journal_open();
    char gid[CB_GAME_ID_MAX + 1];
    a_human_and_a_bot_dealt(gid);
    serve();
    cb_journal_stats j0 = jstats();
    int drives = 0, moves = 0;
    for (int round = 0; round < 40; round++) {
        if (a_moves(gid)) moves++;
        cb_game_row row;
        T_EQ_INT(CB_ROWS_CALL(g_ctx.store, game_get, gid, &row), CB_OK);
        const bool playing = row.status == GAME_STATUS_PLAYING;
        cb_game_row_free(&row);
        if (!playing) break;
        T_EQ_INT(cb_bots_drive(&g_ctx, gid), CB_OK);   // this thread, not the loop's
        drives++;
    }
    cb_journal_stats j1 = jstats();
    T_CHECK(moves > 0 && drives > 0);
    T_CHECK(j1.writes - j0.writes > moves);           // the cycles were written too
    T_EQ_INT((int)(j1.loop_writes - j0.loop_writes), (int)(j1.writes - j0.writes));
    world_close();
}

// THREE THREADS (OPTIMIZED_DESIGN.md step 10): the loop, one request worker and one job worker
// (CBASE_REQ_WORKERS=1, CBASE_BOT_WORKERS=1). A bot drive runs on the job worker. A human's move
// made while the drive commits - the game's lock held by the job worker, its pushes going out -
// is not waited for on the loop (cb_table_trylock): it goes to the one request worker, which waits
// for the lock there, and commits after the drive. One made while the drive computes (no lock
// held) commits inline at once, and the drive, fenced, loses its commit and drives again.
typedef struct { const char *gid; atomic_bool done; int rc; } drive_job;
static void drive_run(void *ud) {
    drive_job *d = ud;
    d->rc = cb_bots_drive(&g_ctx, d->gid);
    atomic_store(&d->done, true);
}
static const char *g_race_gid;
static mover g_race;
static pthread_t g_race_th;
static atomic_int g_race_when;   // 1: post the move from the drive's commit; 2: from its compute; 0: done
static atomic_int g_race_attempts;
static void race_on_commit(const cb_fanout *f) {
    if (atomic_load(&g_race_when) != 1 || strcmp(f->game_id, g_race_gid) != 0) return;
    atomic_store(&g_race_when, 0);
    memset(&g_race, 0, sizeof g_race);
    g_race.n = next_move(g_race_gid, &g_race.bearer, g_race.body, sizeof g_race.body);
    if (g_race.n <= 0) return;
    pthread_create(&g_race_th, NULL, move_thread, &g_race);
    usleep(100000);   // the move reaches the loop, which finds the game locked and hands it on
}
static void race_on_cycle(const char *game_id, int attempt, int prefs_len) {
    atomic_fetch_add(&g_race_attempts, attempt > 1);
    if (atomic_load(&g_race_when) != 2 || strcmp(game_id, g_race_gid) != 0) return;
    atomic_store(&g_race_when, 0);
    memset(&g_race, 0, sizeof g_race);
    g_race.n = next_move(g_race_gid, &g_race.bearer, g_race.body, sizeof g_race.body);
    if (g_race.n > 0) g_race.status = post_action(g_race.bearer, g_race.body, g_race.n, &g_race.resp);   // inline: no lock is held
    atomic_store(&g_race.done, true);
}

// A, B and a bot, dealt.
static void two_humans_and_a_bot_dealt(char gid[CB_GAME_ID_MAX + 1]) {
    reply r;
    cb_limits_reset();   // a game after a game: the create limit is not what is tested
    call("create", "A", NULL, 0, &r);
    T_EQ_INT(r.status, 200);
    T_CHECK(tc_created_game(r.body, r.len, gid));
    meta("B", ASK_JOIN, gid, &r);
    meta("A", ASK_ADD_BOT, gid, &r);
    meta("A", ASK_START, gid, &r);
    meta("B", ASK_START, gid, &r);
    T_CHECK(gid[0] != 0);
}

static bool bots_due_at(const char *gid) {
    cb_game_row row;
    if (CB_ROWS_CALL(g_ctx.store, game_get, gid, &row) != CB_OK) return false;
    Game *g = calloc(1, sizeof *g);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    Table t;
    table_init(&t, g, snaps);
    const bool due = row.status == GAME_STATUS_PLAYING &&
                     table_load(&t, row.state, (int)row.state_len, row.roster, (int)row.roster_len) == TABLE_OK && table_bots_due(&t);
    free(g);
    free(snaps);
    cb_game_row_free(&row);
    return due;
}

static void a_humans_move_during_a_drive_commits_on_one_worker_each(void) {
    world_open();
    journal_open();
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_and_a_bot_dealt(gid);
    serve();
    g_ctx.job_pool = cb_pool_new(1, "tinj");   // the one job worker beside the one request worker
    g_race_gid = gid;
    atomic_store(&g_race_attempts, 0);
    cb_fanout_observer = race_on_commit;
    cb_bots_cycle_observer = race_on_cycle;
    int pooled = 0, inline_raced = 0;
    // Raced until each way landed twice and a fenced drive drove again (whether a bot is still due
    // after the human's move is the deal's: a race may leave it nothing to retry).
    for (int round = 0; round < 2000 && (pooled < 2 || inline_raced < 2 || atomic_load(&g_race_attempts) < 1); round++) {
        cb_game_row row;
        T_EQ_INT(CB_ROWS_CALL(g_ctx.store, game_get, gid, &row), CB_OK);
        const bool playing = row.status == GAME_STATUS_PLAYING;
        cb_game_row_free(&row);
        if (!playing) {   // the next game, with the same seats
            cb_fanout_observer = NULL;
            cb_bots_cycle_observer = NULL;
            two_humans_and_a_bot_dealt(gid);
            cb_fanout_observer = race_on_commit;
            cb_bots_cycle_observer = race_on_cycle;
            continue;
        }
        const char *b;
        uint8_t body[256];
        if (!bots_due_at(gid) || next_move(gid, &b, body, sizeof body) <= 0) {
            // Nobody races here: a human moves, or the bot drives, plainly.
            if (next_move(gid, &b, body, sizeof body) > 0) post_action(b, body, next_move(gid, &b, body, sizeof body), NULL);
            else if (bots_due_at(gid)) { drive_job d = { gid, false, 0 }; drive_run(&d); }
            continue;
        }
        const int when = pooled < 2 && pooled <= inline_raced ? 1 : 2;
        atomic_store(&g_race_when, when);
        const counters c0 = stats();
        drive_job d = { .gid = gid };
        T_EQ_INT(cb_pool_submit(g_ctx.job_pool, drive_run, &d), CB_OK);
        for (int i = 0; i < 20000 && !atomic_load(&d.done); i++) usleep(1000);
        T_CHECK(atomic_load(&d.done));
        T_EQ_INT(d.rc, CB_OK);
        if (atomic_load(&g_race_when) != 0) { atomic_store(&g_race_when, 0); continue; }   // the drive found nothing to commit
        if (when == 1) {
            if (g_race.n <= 0) continue;
            pthread_join(g_race_th, NULL);
            const counters c1 = stats();
            T_EQ_INT(g_race.status, 200);
            T_CHECK(c1.to_pool > c0.to_pool);   // the loop handed it on rather than wait for the lock
            pooled++;
        } else {
            if (g_race.n <= 0) continue;
            T_EQ_INT(g_race.status, 200);
            inline_raced++;
        }
    }
    cb_fanout_observer = NULL;
    cb_bots_cycle_observer = NULL;
    T_CHECK(pooled >= 2);
    T_CHECK(inline_raced >= 2);
    T_CHECK(atomic_load(&g_race_attempts) >= 1);   // a drive whose compute a move overtook drove again
    cb_pool_free(g_ctx.job_pool);
    g_ctx.job_pool = NULL;
    world_close();
}

// An admin's write holds the journal's writer (store.h cb_store_admin_query) for as long as it runs.
// A move then is not written on the loop, and the loop does not wait for the writer: the move goes
// to the pool, which waits there, while the loop answers everyone else. The admin rule then refuses
// the move (its base was read before the admin's write: a conflict, as ever), and the next is the
// loop's again.
static atomic_int g_admin_in;
static int slow_admin(void *ud) {
    atomic_store(&g_admin_in, 1);
    usleep(1500000);
    return CB_OK;
}
static void *admin_thread(void *ud) {
    g_ctx.store->app_exclusive(g_ctx.store, slow_admin, NULL);
    return NULL;
}

static void a_held_writer_never_holds_the_loop(void) {
    world_open();
    journal_open();
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    serve();
    T_CHECK(g_ctx.store->app_exclusive != NULL);
    cb_journal_stats j0 = jstats();
    counters c0 = stats();
    mover m;
    memset(&m, 0, sizeof m);
    m.n = next_move(gid, &m.bearer, m.body, sizeof m.body);
    T_CHECK(m.n > 0);
    atomic_store(&g_admin_in, 0);
    pthread_t adm, th;
    pthread_create(&adm, NULL, admin_thread, NULL);
    for (int i = 0; i < 2000 && !atomic_load(&g_admin_in); i++) usleep(1000);
    T_CHECK(atomic_load(&g_admin_in));
    pthread_create(&th, NULL, move_thread, &m);
    usleep(100000);
    counters c1 = stats();
    T_CHECK(c1.ok && c1.took_us < 500000);   // the loop answers while the writer is held (1.5 s)
    T_EQ_INT((int)(c1.to_pool - c0.to_pool), 1);
    T_CHECK(!atomic_load(&m.done));           // the move waits, on the pool
    pthread_join(adm, NULL);
    pthread_join(th, NULL);
    T_CHECK(m.status == 200 || m.status == 500);   // applied, or refused by the admin rule
    // The writer free again: the next move is written on the loop.
    T_CHECK(a_moves(gid) || next_move(gid, &m.bearer, m.body, sizeof m.body) > 0);
    m.n = next_move(gid, &m.bearer, m.body, sizeof m.body);
    T_CHECK(m.n > 0);
    T_EQ_INT(post_action(m.bearer, m.body, m.n, &m.resp), 200);
    T_EQ_INT(m.resp.status, TABLE_STATUS_APPLIED);
    counters c2 = stats();
    cb_journal_stats j1 = jstats();
    T_EQ_INT((int)(j1.loop_writes - j0.loop_writes), (int)(j1.writes - j0.writes));
    T_CHECK(c2.to_pool - c1.to_pool <= 1);   // A's move above may be the one refused and asked again
    world_close();
}

// The loop makes no segment file: segments rotate after every write here (seg_bytes 1), and a
// dozen moves of a dozen games, pipelined on one connection, are made in one loop pass, back to
// back - faster than the flush thread makes the next segment ahead. A move that finds none made yet
// goes to the pool, the flush thread is asked for it and tells the loop when it is made, and the
// loop writes what waited; every write is the loop's, every move lands, and no file was made on it.
static void the_loop_makes_no_segment(void) {
    enum { N = 12 };
    world_open();
    g_seg_bytes = 1;
    journal_open();
    g_seg_bytes = 0;
    char gids[N][CB_GAME_ID_MAX + 1];
    reply r;
    for (int i = 0; i < N; i++) {
        call("create", "A", NULL, 0, &r);
        T_EQ_INT(r.status, 200);
    }
    // The dashboard lists A's games (they are all A's): each is dealt.
    struct { int n; char (*ids)[CB_GAME_ID_MAX + 1]; } got = { 0, gids };
    T_EQ_INT(cb_table_views_of_user(&g_ctx, UID_A, N, all_games, &got), N);
    for (int i = 0; i < N; i++) {
        meta("B", ASK_JOIN, gids[i], &r);
        meta("A", ASK_START, gids[i], &r);
        meta("B", ASK_START, gids[i], &r);
    }
    serve();
    cb_journal_stats j0 = jstats();
    // One write holding the dozen requests.
    char *wire = malloc(N * 512);
    size_t wl = 0;
    for (int i = 0; i < N; i++) {
        const char *b;
        uint8_t body[256];
        int n = next_move(gids[i], &b, body, sizeof body);
        T_CHECK(n > 0);
        wl += (size_t)snprintf(wire + wl, 512, "POST /v1/action HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\n"
                               "Content-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n", b, n);
        memcpy(wire + wl, body, (size_t)n);
        wl += (size_t)n;
    }
    int fd = tc_connect(g_port);
    T_CHECK(write(fd, wire, wl) == (ssize_t)wl);
    free(wire);
    // The dozen answers, in order: each a head with its Content-Length and the kernel's answer.
    cb_buf in;
    cb_buf_init(&in);
    size_t at = 0;
    for (int i = 0; i < N; i++) {
        char *he = NULL;
        int st = 0;
        size_t cl = 0;
        for (;;) {
            he = in.len > at ? strstr((char *)in.p + at, "\r\n\r\n") : NULL;
            if (he) {
                const char *c = strcasestr((char *)in.p + at, "content-length:");
                cl = c && c < he ? strtoul(c + 15, NULL, 10) : 0;
                if ((size_t)(he + 4 - (char *)in.p) + cl <= in.len) break;
            }
            struct pollfd p = { fd, POLLIN, 0 };
            char buf[4096];
            ssize_t n = poll(&p, 1, 5000) == 1 ? recv(fd, buf, sizeof buf, 0) : -1;
            if (n <= 0) break;
            cb_buf_append(&in, buf, (size_t)n);
        }
        T_CHECK(he != NULL);
        if (!he) break;
        T_CHECK(sscanf((char *)in.p + at, "HTTP/1.1 %d", &st) == 1 && st == 200);
        const size_t body = (size_t)(he + 4 - (char *)in.p);
        TableResponse tr;
        T_EQ_INT(table_response_decode(in.p + body, (int)cl, &tr), 0);
        T_EQ_INT(tr.status, TABLE_STATUS_APPLIED);
        at = body + cl;
    }
    cb_buf_free(&in);
    close(fd);
    cb_journal_stats j1 = jstats();
    T_EQ_INT((int)(j1.writes - j0.writes), N);
    T_EQ_INT((int)(j1.loop_writes - j0.loop_writes), N);
    T_EQ_INT((int)j1.loop_seg_opens, 0);
    T_CHECK(j1.segments >= N);
    world_close();
}

// With every write synced (opts.sync: an msync per batch, a wait on the disk) the loop writes
// nothing: every move goes to the pool, which writes it.
static void a_synced_journal_writes_nothing_on_the_loop(void) {
    world_open();
    g_sync = true;
    journal_open();
    g_sync = false;
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    serve();
    counters c0 = stats();
    cb_journal_stats j0 = jstats();
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
    cb_journal_stats j1 = jstats();
    T_EQ_INT((int)(c1.to_pool - c0.to_pool), 3);
    T_EQ_INT((int)(j1.writes - j0.writes), 3);
    T_EQ_INT((int)(j1.loop_writes - j0.loop_writes), 0);
    world_close();
}

int main(void) {
    T_RUN(a_move_on_a_locked_game_waits_on_the_pool);
    T_RUN(a_game_not_resident_is_read_on_the_pool);
    T_RUN(the_rated_end_stays_on_the_loop_and_a_pending_replay_goes_to_the_pool);
    T_RUN(the_store_is_never_read_or_written_on_the_loop);
    T_RUN(a_drives_commits_are_written_by_the_loop);
    T_RUN(a_humans_move_during_a_drive_commits_on_one_worker_each);
    T_RUN(a_held_writer_never_holds_the_loop);
    T_RUN(the_loop_makes_no_segment);
    T_RUN(a_synced_journal_writes_nothing_on_the_loop);
    return T_DONE();
}
