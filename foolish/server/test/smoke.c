// smoke.c - `cbase serve` end to end, as a client sees it.
//
//   build/test/smoke build/cbase
//
// Starts the real binary on a free port with a fresh database directory, waits
// for its "listening on" line, and talks to it over HTTP. ALWAYS it proves the
// skeleton: the process starts (the kernel self-test passed), answers
// GET /health, and drains on SIGTERM with exit status 0.
//
// THE GAME SCENARIO (the brief's), always, unless CBASE_SMOKE_GAME=0, over
// the binary surface (DESIGN.md "Binary surface") as the clients speak it,
// every request built and every answer read by the kernel's client library
// (netclient.h): three humans sign up; A creates, B joins, A adds a bot, both
// start; each holds a stream socket: A on its own seat's casts, B on its
// views, C, who never sits, on the game's spectator casts. The humans play legal moves (chosen on their own masked
// envelope with the kernel's client table, as the browser does) while the bot
// pool drives the bot. Once mid-game, when only the bots owe a move, the
// server is killed and restarted with nobody bumping: the heartbeat must
// resume the bot after the dead process's lease runs out. At the end: game
// over, every push stream strictly increasing in version (B's last view push
// is B's stored view), the ratings changed (the leaderboard), one replay
// stored (A's history), and after a clean restart on the same database the
// game reloads at the same version with the same envelope.
//
// Every module is real here: auth (real JWTs), the SQLite store, the /v1
// routes, realtime (rt_local, through rt_deps' production table),
// the app.
//
// THE BOTS' TIME RUNS SMOKE_SPEEDUP TIMES FASTER (CBASE_BOT_SPEEDUP, config.h):
// the pace, the lease, the scan's staleness window and serve's scan period are
// each production's over SMOKE_SPEEDUP, so every ratio between them holds and
// the game, the crash's lease expiry and the idle human's rest each take
// seconds. unit_app holds production's values and the division itself.
#include "t.h"
#include "config.h"
#include "store/game_id.h"

#include "lib_client.h"
#include "lib_proc.h"
#include "fixture_unfurl.h"

#include "anim_plan.h"
#include "awire.h"
#include "client_table.h"
#include "evwire.h"
#include "legal.h"
#include "table.h"
#include "net/ws.h"
#include "util/clock.h"
#include "app/bots.h"

#include "ask.h"
#include "netclient.h"
#include "netwire.h"
#include "stream_frame.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../../../shared/c/rng.h"
#include "../../../shared/c/clock.h"
#include "../../../shared/c/fnv.h"

#define SMOKE_SPEEDUP 20
#define SMOKE_ADMIN_TOKEN "smoke-admin-token-0123456789abcdef0123"   // the console's: live_game reads through it
static const char *g_bin;
static tp_child g_child;
static int g_port;
static char g_dbdir[400];

static void start_server(void) {
    char line[512];
    static char envdb[512];
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/cbase.sqlite", g_dbdir);
    // Fresh accounts, rated at once (CBASE_RATED_MIN_AGE, table_ops.c): the scenario checks the leaderboard moves.
    static char speedup[48];
    snprintf(speedup, sizeof speedup, "CBASE_BOT_SPEEDUP=%d", SMOKE_SPEEDUP);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", envdb, "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret",
                    "CBASE_RATED_MIN_AGE=0", speedup, "CBASE_ADMIN_TOKEN=" SMOKE_ADMIN_TOKEN, NULL };
    char *argv[] = { (char *)g_bin, "serve", NULL };
    g_port = 0;
    T_EQ_INT(tp_start(&g_child, argv, env), 0);
    T_EQ_INT(tp_wait_line(&g_child, "listening on", 20000, line, sizeof line), 0);
    const char *colon = strrchr(line, ':');
    g_port = colon ? atoi(colon + 1) : 0;
    T_CHECK(g_port > 0);
}

static void starts_and_listens(void) {
    if (tp_tempdir(g_dbdir, sizeof g_dbdir) != 0) { T_CHECK(0); return; }
    start_server();
}

static void health(void) {
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/health", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    T_CHECK(strncmp(cb_buf_cstr(&r.body), "ok cbase ", 9) == 0);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "mode=serve") != NULL);
    tc_response_free(&r);
    // A route nobody registered yet is a clean 404, not a crash.
    T_EQ_INT(tc_http(g_port, "GET", "/nope", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 404);
    tc_response_free(&r);
}

// GET and HEAD /m/<payload>: the link preview a crawler fetches, with no key.
static void unfurl(void) {
    char path[4096];
    snprintf(path, sizeof path, "/m/%s", UNFURL_GOLDENS[0].payload);
    tc_response g, h;
    T_EQ_INT(tc_http(g_port, "GET", path, NULL, NULL, 0, &g), 0);
    T_EQ_INT(g.status, 200);
    char v[128];
    T_EQ_STR(tc_header(&g, "content-type", v, sizeof v), "text/html; charset=utf-8");
    T_EQ_STR(tc_header(&g, "cache-control", v, sizeof v), "public, max-age=300");
    for (int k = 0; k < UNFURL_N_TAGS; k++) T_CHECK(strstr(cb_buf_cstr(&g.body), UNFURL_GOLDENS[0].tags[k]) != NULL);
    T_EQ_INT(tc_http(g_port, "HEAD", path, NULL, NULL, 0, &h), 0);
    T_EQ_INT(h.status, 200);
    char cl[32];
    snprintf(cl, sizeof cl, "%zu", g.body.len);
    T_EQ_STR(tc_header(&h, "content-length", v, sizeof v), cl);
    T_EQ_INT(h.body.len, 0);
    tc_response_free(&g);
    tc_response_free(&h);
    // HEAD on any GET route: /health answered 405 to it before.
    T_EQ_INT(tc_http(g_port, "HEAD", "/health", NULL, NULL, 0, &h), 0);
    T_EQ_INT(h.status, 200);
    T_EQ_INT(h.body.len, 0);
    tc_response_free(&h);
}

// The drain names who sent the signal (main_serve.c): here, this test.
static void drains_on_sigterm(void) {
    char want[64];
    snprintf(want, sizeof want, "SIGTERM from pid %d ", (int)getpid());
    T_EQ_INT(kill(g_child.pid, SIGTERM), 0);
    T_EQ_INT(tp_wait_line(&g_child, want, 15000, NULL, 0), 0);
    T_EQ_INT(tp_stop(&g_child, 0, 15000), 0);
}

// ---- the game scenario ---------------------------------------------------------------

// A signed-in user: the client library's session, saved between turns (it
// holds one session at a time, as one app does).
typedef struct {
    char    name[32];
    char    id[64];
    uint8_t blob[NC_SESSION_BLOB_MAX];
    int     blob_len;
} smoke_user;

static int64_t mono_ms(void) {
    return host_mono_ms();
}

// The last answer, as the host received it.
static int g_status;
static uint8_t g_body[1 << 20];
static int g_len;
static char g_retry_after[16];   // its Retry-After ("" none)
// Header lines every request carries besides its own ("" none): the edge's, for a server behind it.
static const char *g_via = "";

// One request over HTTP, as a host's transport sends it.
#define SMOKE_ANSWER_MS 120000

static int send_req(const NcRequest *q) {
    g_status = -1;
    g_len = 0;
    if (!q) return -1;
    char path[NC_PATH_MAX + 1], headers[NC_AUTH_MAX + 256];
    memcpy(path, q->path, q->path_len);
    path[q->path_len] = 0;
    int h = 0;
    if (q->auth_len) h += snprintf(headers + h, sizeof headers - (size_t)h, "Authorization: %.*s\r\n", (int)q->auth_len, q->auth);
    if (q->method == NC_POST) h += snprintf(headers + h, sizeof headers - (size_t)h, "Content-Type: application/octet-stream\r\n");
    h += snprintf(headers + h, sizeof headers - (size_t)h, "%s", g_via);
    headers[h] = 0;
    // A HANG GUARD, NOT A SPEED CLAIM: the answer may take as long as a
    // starved machine makes it. lib_client's 10 s once failed a move the
    // server answered correctly, late, on a Mac at load ~700 (a lost answer to
    // a move is the client's INTERNAL, a non-200 here); reproduced by freezing
    // the server with SIGSTOP for 11 s mid-request.
    tc_response r;
    int fd = tc_connect(g_port);
    if (fd < 0) return -1;
    int rc = tc_request(fd, q->method == NC_GET ? "GET" : "POST", path, headers, q->body, q->body_len, SMOKE_ANSWER_MS, &r);
    close(fd);
    if (rc != 0) return -1;
    g_status = r.status;
    if (!tc_header(&r, "retry-after", g_retry_after, sizeof g_retry_after)) g_retry_after[0] = 0;
    g_len = r.body.len < sizeof g_body ? (int)r.body.len : 0;
    memcpy(g_body, r.body.p, (size_t)g_len);
    tc_response_free(&r);
    return g_status;
}

// u's intent, through the library's pump as a host runs it: the session
// loaded, every request it names sent (a refresh first when it asks for one)
// until the intent's ticket is done, the session saved again. The status of
// the answer that finished it (g_body holds it), -1 when none did.
static int call(smoke_user *u, int kind, const char *gid, const void *bytes, int n, int64_t intent) {
    T_EQ_INT(nc_session_load(u->blob, u->blob_len), NW_OK);
    NcIntent in;
    memset(&in, 0, sizeof in);
    in.kind = (uint8_t)kind;
    if (gid) { in.gid_len = (uint8_t)strlen(gid); memcpy(in.gid, gid, in.gid_len); }
    if (n > 0) { in.bytes_len = (uint16_t)n; memcpy(in.bytes, bytes, (size_t)n); }
    in.intent = intent;
    int st = -1;
    uint32_t t = nc_do(&in, cb_now_ms());
    for (const NcRequest *q; t && (q = nc_next_request(cb_now_ms()));) {
        uint32_t id = q->id;
        int got = send_req(q);
        const NcDone *d = nc_on_response(id, got < 0 ? 0 : got, g_body, g_len, cb_now_ms());
        if (d && d->ticket == t) { st = got; break; }
    }
    u->blob_len = nc_session_save(u->blob, sizeof u->blob);
    return st;
}

static int signup(smoke_user *u, const char *name) {
    snprintf(u->name, sizeof u->name, "%s", name);
    nc_reset();
    send_req(nc_req_signup(name, (int)strlen(name), "smoke-pass-123", 14));
    int why = nc_session_read(g_status, g_body, g_len);
    if (why) { fprintf(stderr, "signup %s: %d why %d\n", name, g_status, why); return -1; }
    const NcSessionInfo *i = nc_session_info();
    snprintf(u->id, sizeof u->id, "%.*s", (int)i->user_id_len, i->user_id);
    u->blob_len = nc_session_save(u->blob, sizeof u->blob);
    return u->blob_len > 0 ? 0 : -1;
}

// An ask; the asker's envelope into env. The status.
static int ask(smoke_user *u, int kind, const char *gid, cb_buf *env) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = (uint8_t)kind;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    int st = n > 0 ? call(u, NC_DO_ASK, NULL, body, n, -1) : -1;
    cb_buf_reset(env);
    cb_buf_append(env, g_body, (size_t)g_len);
    return st;
}

// u's view of the game: its version, the envelope into env. false when there is none.
static bool game_view(smoke_user *u, const char *gid, int64_t *version, cb_buf *env) {
    if (call(u, NC_DO_OPEN_GAME, gid, NULL, 0, -1) != 200) return false;
    const NcGameRow *row = nc_view_read(g_status, g_body, g_len);
    if (!row) return false;
    *version = row->version;
    if (env) {
        cb_buf_reset(env);
        cb_buf_append(env, g_body + row->env_off, row->env_len);
    }
    return true;
}

typedef struct {
    ClientSlot  slot;
    ClientTable c;
    LegalMoves  moves;
} smoke_client;

// A legal move for the viewer on their envelope, as the browser would offer
// one: the kernel's legal moves on the masked board, each judged again by
// client_validate. Its wire length, or 0 when the viewer has none.
static int choose_move(smoke_client *sc, const cb_buf *env, uint8_t *wire, int cap, uint64_t *rng) {
    client_init(&sc->c, &sc->slot);
    if (client_adopt_envelope(&sc->c, env->p, (int)env->len) < 0) return 0;
    int seat = sc->c.view.my_seat;
    if (seat < 0 || sc->c.view.status != GAME_STATUS_PLAYING) return 0;
    calculate_legal_moves(sc->c.g, seat, &sc->moves);
    int start = sc->moves.n ? (int)(rng_u32(rng) % (uint32_t)sc->moves.n) : 0;
    for (int k = 0; k < sc->moves.n; k++) {
        const LegalMove *m = &sc->moves.moves[(start + k) % sc->moves.n];
        if (m->type == MOVE_WAIT) continue;
        AwireAction a;
        memset(&a, 0, sizeof a);
        a.kind = m->type;
        a.n = m->n_cards;
        for (int i = 0; i < m->n_cards; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
        int n = awire_encode(&a, wire, cap);
        if (n > 0 && client_validate(&sc->c, &sc->c.view, wire, n) == 0) return n;
    }
    return 0;
}

// One stream socket and what it heard: the versions of the casts and of the
// views pushed, in order. It speaks the socket's frames (shared/c/stream_frame.h)
// on /ws, as the clients do, and each push is a netwire.h frame.
typedef struct {
    int      fd;
    char     token[NW_ACCESS_MAX + 1];
    char     uid[64];
    char     topic[128];          // what it listens to, named as the old topics were: u/<id>/v (views), u/<id>/c/<game> (seat casts), s/<game> (spectators')
    int      n_cast;
    uint32_t cast_v[4096];
    int      n_view;
    uint32_t view_v[4096];
    int      refused;             // SUBACKs that refused, and closes
    int64_t  last_ping;           // when this socket last pinged
    // n_cast at each reopen (the crash's): a socket that was closed missed what
    // was cast meanwhile, so the versions may jump there and only there.
    int      n_reopen;
    int      reopen_at[16];
} listener;

// Every cast follows the one before it (version + 1), but across a reopen.
// A commit the stream never casts is a gap the kernel client takes for a lost
// push, and it fetches the game whole (netclient.c NW_WS_CAST).
static int cast_gaps(const listener *l) {
    int gaps = 0;
    for (int i = 1; i < l->n_cast; i++) {
        bool reopened = false;
        for (int k = 0; k < l->n_reopen; k++) reopened |= l->reopen_at[k] == i;
        if (!reopened && l->cast_v[i] != l->cast_v[i - 1] + 1) {
            if (gaps < 4) fprintf(stderr, "smoke: a cast at v%u follows v%u\n", l->cast_v[i], l->cast_v[i - 1]);
            gaps++;
        }
    }
    return gaps;
}

static void mq_send(listener *l, const uint8_t *b, int n) {
    if (n > 0) tc_ws_send(l->fd, CB_WS_OP_BIN, b, (size_t)n);
}

// One frame; a REPLY's status into *suback (0 granted, 1 refused).
static void hear(listener *l, const uint8_t *p, size_t n, int *suback) {
    if (sf_is_push(p, n)) {
        static NwFrame f;
        if (nw_frame_decode(p, (int)n, &f) != NW_OK) return;
        if (f.kind == (l->topic[0] == 's' ? NW_WS_SPECT : NW_WS_CAST) && l->n_cast < 4096) l->cast_v[l->n_cast++] = f.version;
        else if (f.kind == NW_WS_VIEW && f.blob.len && l->n_view < 4096) l->view_v[l->n_view++] = f.version;
        return;
    }
    sf_frame sf;
    if (sf_parse(p, n, &sf) != 0 || sf.kind != SF_REPLY) return;
    if (sf.status != 200) l->refused++;
    if (suback) *suback = sf.status == 200 ? 0 : 1;
}

// Reads every message that arrives within timeout_ms, then each next one that
// arrives within gap_ms of the last. The game loop takes only what is already
// there (0, 0): what is still in flight is read on the next turn. Like the
// clients, pings every 30 s inside its 60 s keep-alive.
static void drain_ws(listener *l, int timeout_ms, int gap_ms) {
    if (l->fd >= 0 && mono_ms() - l->last_ping > NC_PING_MS) {
        uint8_t b[2];
        mq_send(l, b, sf_put_bare(b, sizeof b, SF_PING));
        l->last_ping = mono_ms();
    }
    cb_buf f;
    cb_buf_init(&f);
    int op;
    while (l->fd >= 0 && (op = tc_ws_recv(l->fd, timeout_ms, &f)) >= 0) {
        if (op == CB_WS_OP_CLOSE) { l->refused++; break; }
        hear(l, f.p, f.len, NULL);
        cb_buf_reset(&f);
        timeout_ms = gap_ms;
    }
    cb_buf_free(&f);
}

// Opens the socket, says HELLO with the token and subscribes; true once the
// subscription is granted. Pushes that arrive first are heard.
static bool ws_open(listener *l) {
    if (l->n_cast > 0 && l->n_reopen < 16) l->reopen_at[l->n_reopen++] = l->n_cast;
    l->fd = tc_ws_open(g_port, "/ws", NULL);
    l->last_ping = mono_ms();
    if (l->fd < 0) return false;
    static uint8_t b[NW_ACCESS_MAX + 256];
    mq_send(l, b, sf_put_hello(b, sizeof b, l->token, strlen(l->token)));
    // The topic names what is heard: views are the user's own (nothing to say), a game's casts are
    // subscribed to (a seat's come with the user anyway, the spectators' only on the subscription).
    const char *g = strstr(l->topic, "/c/");
    g = g ? g + 3 : (strncmp(l->topic, "s/", 2) == 0 ? l->topic + 2 : NULL);
    bool views = g == NULL;
    mq_send(l, b, g ? sf_put_sub(b, sizeof b, SF_SUB, 0x80000001u, NW_SUB_GAME, g, strlen(g))
                    : sf_put_sub(b, sizeof b, SF_SUB, 0x80000001u, NW_SUB_GAMES, "", 0));
    (void)views;
    cb_buf in;
    cb_buf_init(&in);
    int code = -1;
    for (int i = 0; i < 50 && code < 0; i++) {
        int op = tc_ws_recv(l->fd, 5000, &in);
        if (op < 0 || op == CB_WS_OP_CLOSE) break;
        hear(l, in.p, in.len, &code);
        cb_buf_reset(&in);
    }
    cb_buf_free(&in);
    return code == 0;
}

// The listener for u: its bearer token (what a built request carries) and its one topic.
static void ws_for(listener *l, smoke_user *u, const char *topic) {
    T_EQ_INT(nc_session_load(u->blob, u->blob_len), NW_OK);
    const NcRequest *q = nc_req_games(cb_now_ms());
    T_CHECK(q && !q->is_refresh && q->auth_len > 7);
    if (q) snprintf(l->token, sizeof l->token, "%.*s", (int)q->auth_len - 7, q->auth + 7);
    snprintf(l->uid, sizeof l->uid, "%s", u->id);
    snprintf(l->topic, sizeof l->topic, "%s", topic);
    l->fd = -1;
}

// The game's row as the server holds it NOW: the console's game panel (admin_panels.c
// api_game), which reads the store through the move journal's overlay (journal.h: "the
// store's reads see them first") and never flushes it. Not the database file: under the
// journal a plain move or a bot cycle does not write the tables until the flush rule says
// so (game end, 10 min, 256 KiB), so the file's row is minutes behind a game in play.
// The panel's answer is the console's JSON (DESIGN.md "Where JSON is tolerated"); this
// reads three keys from it. at (may be NULL) gets updated_at, the scan's staleness clock:
// only a commit moves it (serve keeps a bot drive's lease in memory, app/bots.c).
typedef struct {
    int64_t needs_bots, version;
    char    at[40];
} live_row;

static bool json_int(const char *js, const char *key, int64_t *out) {
    const char *p = strstr(js, key);
    if (!p) return false;
    p += strlen(key);
    if (strncmp(p, "true", 4) == 0) { *out = 1; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = 0; return true; }
    char *e;
    long long v = strtoll(p, &e, 10);
    if (e == p) return false;
    *out = v;
    return true;
}

static bool live_game(const char *gid, live_row *g) {
    char path[96];
    snprintf(path, sizeof path, "/admin/api/game?id=%s", gid);
    tc_response r;
    if (tc_http(g_port, "GET", path, "Authorization: Bearer " SMOKE_ADMIN_TOKEN "\r\n", NULL, 0, &r) != 0) return false;
    const char *js = cb_buf_cstr(&r.body);
    bool ok = r.status == 200 && json_int(js, "\"needs_bots\":", &g->needs_bots) && json_int(js, "\"version\":", &g->version);
    const char *at = ok ? strstr(js, "\"updated_at\":\"") : NULL;
    if (ok && at) snprintf(g->at, sizeof g->at, "%.*s", (int)strcspn(at + 14, "\""), at + 14);
    else if (ok) snprintf(g->at, sizeof g->at, "null");
    tc_response_free(&r);
    return ok;
}

// THE IDLE HUMAN (production, 2026-10-17: 1,037 of 1,149 bot cycles in a week
// drove nothing). One human against one bot; the human owes the move and walks
// away. No bot can act, so the server's row must not say the bots are owed one,
// and serve's heartbeat (a scan every CB_BOT_SCAN_EVERY_MS, a game stale after
// CB_BOT_STALE_MS, both over SMOKE_SPEEDUP) must never start a drive for it:
// nothing is committed (the version and the row's updated_at stay), across two
// scans past the staleness window.
static void an_idle_human_turn_is_never_kicked(void) {
    smoke_user D;
    if (signup(&D, "SMOKED") != 0) { T_CHECK(0); return; }
    smoke_client *sc = calloc(1, sizeof *sc);
    cb_buf env;
    cb_buf_init(&env);
    T_EQ_INT(call(&D, NC_DO_CREATE, NULL, NULL, 0, -1), 200);
    client_init(&sc->c, &sc->slot);
    T_CHECK(client_adopt_envelope(&sc->c, g_body, g_len) >= 0);
    char gid[32];
    snprintf(gid, sizeof gid, "%.*s", (int)sc->c.gid_len, sc->c.gid);
    T_EQ_INT(ask(&D, ASK_ADD_BOT, gid, &env), 200);
    T_EQ_INT(ask(&D, ASK_START, gid, &env), 200);
    // Until D owes a move: the bot pool plays the bot's (the deal's pacing first).
    uint8_t wire[64];
    uint64_t rng = 11;
    bool owes = false;
    const int64_t t0 = mono_ms();
    while (!owes && mono_ms() - t0 < 30000) {
        int64_t seen;
        if (!game_view(&D, gid, &seen, &env)) break;   // a read, as a client polls; a bump is limited
        owes = choose_move(sc, &env, wire, sizeof wire, &rng) > 0;
        if (!owes) {
            struct timespec ts = { 0, 100 * 1000000 };
            nanosleep(&ts, NULL);
        }
    }
    T_CHECK(owes);
    // While D deliberates a bot may still act (an attacker throws in or says
    // good), paced: the bots are done when the server's row says no
    // bot owes a move (needs_bots). Not "the version held for longer than a
    // pace": a starved bot pool holds the version too (it failed once that
    // way with the server frozen for 12 s, a bot's move still to come).
    live_row g0 = { -1, -1, "" }, g1 = { -1, -1, "" };
    const int64_t t1 = mono_ms();
    while (live_game(gid, &g0) && g0.needs_bots != 0 && mono_ms() - t1 < 30000) {
        struct timespec ts = { 0, 50 * 1000000 };
        nanosleep(&ts, NULL);
    }
    T_CHECK(live_game(gid, &g0));
    T_EQ_INT(g0.needs_bots, 0);
    // Two scans past the staleness window, and a fifth of one more (production's 22 s).
    const int64_t rest = (CB_BOT_STALE_MS + 2 * CB_BOT_SCAN_EVERY_MS + 2000) / SMOKE_SPEEDUP;
    cb_sleep_ms(rest);
    T_CHECK(live_game(gid, &g1));
    T_EQ_INT(g1.version, g0.version);
    T_CHECK(strcmp(g1.at, g0.at) == 0);   // the scan clock: no commit, no lease written to the row
    int64_t seen = -1;
    T_CHECK(game_view(&D, gid, &seen, NULL));   // and what D's client reads agrees
    T_EQ_INT(seen, g0.version);
    fprintf(stderr, "smoke: an idle human's turn rested %lld ms at version %lld, scan clock %s -> %s\n", (long long)rest,
            (long long)g0.version, g0.at, g1.at);
    cb_buf_free(&env);
    free(sc);
}

static void game_scenario(void) {
    smoke_user A, B, C;
    int sa = signup(&A, "SMOKEA"), sb = signup(&B, "SMOKEB"), sc_ = signup(&C, "SMOKEC");
    T_EQ_INT(sa, 0);
    T_EQ_INT(sb, 0);
    T_EQ_INT(sc_, 0);
    if (sa || sb || sc_) return;   // nothing below can pass without the sessions
    smoke_client *sc = calloc(1, sizeof *sc);
    cb_buf env;
    cb_buf_init(&env);

    // create: A's envelope names the game.
    int created = call(&A, NC_DO_CREATE, NULL, NULL, 0, -1);
    T_EQ_INT(created, 200);
    if (created != 200) { cb_buf_free(&env); free(sc); return; }
    client_init(&sc->c, &sc->slot);
    T_CHECK(client_adopt_envelope(&sc->c, g_body, g_len) >= 0);
    char gid[32];
    snprintf(gid, sizeof gid, "%.*s", (int)sc->c.gid_len, sc->c.gid);
    T_CHECK(cb_game_id_ok(gid, strlen(gid)));   // store/game_id.h

    // A hears its own seat's casts, B its views (the games list), C the
    // spectators' casts. Together they cover fanout.c's three pushes.
    listener *LA = calloc(1, sizeof *LA), *LB = calloc(1, sizeof *LB), *LC = calloc(1, sizeof *LC);
    char topic[128];
    snprintf(topic, sizeof topic, "u/%s/c/%s", A.id, gid);
    ws_for(LA, &A, topic);   // A's own seat's casts
    snprintf(topic, sizeof topic, "u/%s/v", B.id);
    ws_for(LB, &B, topic);   // B's views
    snprintf(topic, sizeof topic, "s/%s", gid);
    ws_for(LC, &C, topic);   // the spectators' casts
#define SUBSCRIBE() T_CHECK(ws_open(LA) && ws_open(LB) && ws_open(LC))
    SUBSCRIBE();

    T_EQ_INT(ask(&B, ASK_JOIN, gid, &env), 200);
    T_EQ_INT(ask(&A, ASK_ADD_BOT, gid, &env), 200);
    T_EQ_INT(ask(&A, ASK_START, gid, &env), 200);
    T_EQ_INT(ask(&B, ASK_START, gid, &env), 200);
    client_init(&sc->c, &sc->slot);
    T_CHECK(client_adopt_envelope(&sc->c, env.p, (int)env.len) >= 0);
    T_EQ_INT(sc->c.view.status, GAME_STATUS_PLAYING);

    // Play: each human reads the game for their envelope, as a client does,
    // and moves when they have a move; the bot pool plays the bot (a move
    // that leaves the bot to act wakes it). Not by bumping: a bump is a
    // seated player's nudge, limited (rate_limits.h), not a way to poll.
    //
    // Once, when neither human has a move and the server's row says the bots owe
    // one, the server is KILLED (no drain) and started again on the same file,
    // and nobody moves: only serve's heartbeat (the scan) can restart the
    // drive, and only once the dead process's lease has run out. The
    // stored version moving on proves the heartbeat, the lease expiry and the
    // reload from a crash.
    smoke_user *humans[2] = { &A, &B };
    uint64_t rng = 7;
    int status = GAME_STATUS_PLAYING, moves = 0, crash_tries = 0;
    bool crashed = false, rearranged = false;
    // The game ends when it ends: a long one (many pickups) plus the crash
    // takes minutes. What must not happen is a stall, so the loop gives up
    // only when the version has not moved for STALL_MS (well past the crash's
    // lease wait), under a generous overall cap.
    enum { STALL_MS = 90000, CAP_MS = 900000 };
    const int64_t t_start = mono_ms();
    int64_t last_version = -1, last_progress = t_start;
    while (status == GAME_STATUS_PLAYING && mono_ms() - last_progress < STALL_MS && mono_ms() - t_start < CAP_MS) {
        bool moved = false, answered = false;
        for (int h = 0; h < 2 && status == GAME_STATUS_PLAYING; h++) {
            int64_t seen;
            if (!game_view(humans[h], gid, &seen, &env)) {
                fprintf(stderr, "smoke: %s's game read answered %d (why %d)\n", humans[h]->name, g_status, nc_status_read(g_status, g_body, g_len));
                continue;
            }
            answered = true;
            client_init(&sc->c, &sc->slot);
            if (client_adopt_envelope(&sc->c, env.p, (int)env.len) < 0) continue;
            status = sc->c.view.status;
            uint32_t version = sc->c.view.version;
            if ((int64_t)version != last_version) { last_version = version; last_progress = mono_ms(); }
            // Once, A reverses its hand: a commit that shows nobody anything and
            // must still be cast (the gap check below).
            const int me = sc->c.view.my_seat;
            if (h == 0 && !rearranged && moves >= 4 && me >= 0 && sc->c.g->players[me].hand_count >= 2) {
                TableAsk ra;
                memset(&ra, 0, sizeof ra);
                ra.kind = ASK_REARRANGE_HAND;
                ra.gid_len = (uint8_t)strlen(gid);
                memcpy(ra.gid, gid, ra.gid_len);
                ra.n_idx = (uint8_t)sc->c.g->players[me].hand_count;
                for (int i = 0; i < ra.n_idx; i++) ra.idx[i] = (uint8_t)(ra.n_idx - 1 - i);
                uint8_t body[ASK_BYTES_MAX];
                int bn = ask_encode(&ra, body, sizeof body);
                T_EQ_INT(bn > 0 ? call(&A, NC_DO_ASK, NULL, body, bn, -1) : -1, 200);
                rearranged = true;
                // The board moved on: read it again. A commit like a move, so no crash this round:
                // a rearrangement is written around the journal (journal.h), and the kill is meant
                // to land on moves only the journal holds, which the restart must recover.
                moved = true;
                continue;
            }
            uint8_t wire[64];
            int n = choose_move(sc, &env, wire, sizeof wire, &rng);
            if (n <= 0) continue;
            int64_t t_play = mono_ms();
            int played = call(humans[h], NC_DO_PLAY, gid, wire, n, version);
            if (played != 200)
                fprintf(stderr, "smoke: %s's move at v%u answered %d (last HTTP status %d, why %d) after %lld ms\n", humans[h]->name,
                        version, played, g_status, nc_status_read(g_status, g_body, g_len), (long long)(mono_ms() - t_play));
            T_EQ_INT(played, 200);
            T_CHECK(nc_action_read(g_status, g_body, g_len) != NULL);
            moved = true;
            moves++;
        }
        drain_ws(LA, 0, 0);
        drain_ws(LB, 0, 0);
        drain_ws(LC, 0, 0);
        if (!answered) break;   // the server stopped answering: the asserts below say so
        live_row pre = { 0, 0, "" };
        if (!moved && !crashed && moves >= 4 && crash_tries < 5 && status == GAME_STATUS_PLAYING
            && live_game(gid, &pre) && pre.needs_bots) {
            crash_tries++;
            tp_stop(&g_child, SIGKILL, 5000);
            close(LA->fd);
            close(LB->fd);
            close(LC->fd);
            // The row the new process rebuilt from the tables and the journal, read before the
            // scan can be due (a commit at most a stale window ago): no acknowledged move is lost,
            // and it still owes the bots' move unless a bot moved before the kill landed.
            start_server();
            live_row rec = { 0, -1, "" };
            T_CHECK(live_game(gid, &rec));
            T_CHECK(rec.version >= pre.version);
            const int64_t ver = pre.version;
            const bool owed = rec.needs_bots && rec.version == ver;
            SUBSCRIBE();
            if (!owed) continue;   // the bot moved before the kill landed: try again later
            crashed = true;
            int64_t t0 = mono_ms(), now_v = ver;
            while (mono_ms() - t0 < 75000 && now_v <= ver) {
                struct timespec ts = { 0, 500 * 1000000 };
                nanosleep(&ts, NULL);
                int64_t v;
                if (game_view(&A, gid, &v, NULL)) now_v = v;
            }
            fprintf(stderr, "smoke: after a crash at version %lld the bots moved on to %lld in %lld ms, unbumped\n",
                    (long long)ver, (long long)now_v, (long long)(mono_ms() - t0));
            T_CHECK(now_v > ver);
            last_progress = mono_ms();
            continue;
        }
        if (!moved) {
            // The bots owe a move: give the bot pool a moment.
            struct timespec ts = { 0, 50 * 1000000 };
            nanosleep(&ts, NULL);
        }
    }
    fprintf(stderr, "smoke: %s after %d human moves, %lld s, version %lld\n",
            status == GAME_STATUS_GAME_OVER ? "game over" : "NO PROGRESS", moves, (long long)((mono_ms() - t_start) / 1000),
            (long long)last_version);
    if (status != GAME_STATUS_GAME_OVER) {
        // A stall is a stuck thread until shown otherwise: print every thread's
        // stack of the live server, where gdb is there to do it (CI's Linux).
        char cmd[160];
        snprintf(cmd, sizeof cmd, "command -v gdb >/dev/null && gdb -q -batch -p %d -ex 'thread apply all bt' 1>&2", (int)g_child.pid);
        if (system(cmd) != 0) fprintf(stderr, "smoke: no stacks (gdb is not here, or could not attach)\n");
    }
    T_EQ_INT(status, GAME_STATUS_GAME_OVER);
    T_CHECK(moves > 0);
    T_CHECK(crashed);
    T_CHECK(rearranged);

    // Pushes, each stream strictly increasing in version: A's own casts, the
    // spectators' casts, B's views.
    drain_ws(LA, 1000, 50);
    drain_ws(LB, 1000, 50);
    drain_ws(LC, 1000, 50);
    fprintf(stderr, "smoke: heard %d seat casts, %d spectator casts, %d views\n", LA->n_cast, LC->n_cast, LB->n_view);
    T_CHECK(LA->n_cast > 0);
    T_CHECK(LC->n_cast > 0);
    T_CHECK(LB->n_view > 0);
    // A's connection follows the game in its room (its casts): it is sent no view of it
    // (realtime.h quiet_room), as B's, in no room, is sent every one.
    T_EQ_INT(LA->n_view, 0);
    T_EQ_INT(LA->refused + LB->refused + LC->refused, 0);
    for (int i = 1; i < LA->n_cast; i++) T_CHECK(LA->cast_v[i] > LA->cast_v[i - 1]);
    for (int i = 1; i < LC->n_cast; i++) T_CHECK(LC->cast_v[i] > LC->cast_v[i - 1]);
    // And none is missing: every commit is cast, to the seat and to the
    // spectators, the ones that show nothing (a good over an uncovered table, a
    // hand rearranged) included (table_ops.c).
    T_EQ_INT(cast_gaps(LA), 0);
    T_EQ_INT(cast_gaps(LC), 0);
    for (int i = 1; i < LB->n_view; i++) T_CHECK(LB->view_v[i] > LB->view_v[i - 1]);

    // B's last view push is B's stored view.
    int64_t bv = -1;
    T_CHECK(game_view(&B, gid, &bv, NULL));
    if (LB->n_view > 0 && LB->view_v[LB->n_view - 1] != bv) {
        fprintf(stderr, "smoke: B's view is at version %lld; the pushes heard end", (long long)bv);
        for (int i = LB->n_view > 6 ? LB->n_view - 6 : 0; i < LB->n_view; i++) fprintf(stderr, " %u", LB->view_v[i]);
        fprintf(stderr, "\n");
    }
    T_CHECK(LB->n_view > 0 && LB->view_v[LB->n_view - 1] == bv);
    close(LA->fd);
    close(LB->fd);
    close(LC->fd);
    free(LA);
    free(LB);
    free(LC);

    // The ratings (A is on the board with one game) and the replay (A's history holds one).
    T_EQ_INT(call(&A, NC_DO_LEADERBOARD, NULL, NULL, 0, -1), 200);
    int rows = nc_list_read(g_status, g_body, g_len, NW_BOARD);
    bool on_board = false;
    for (int i = 0; i < rows; i++) {
        const NcBoardRow *r = nc_board_row(g_body, g_len, i);
        if (r && !r->is_bot && r->id_len == strlen(A.id) && memcmp(r->id, A.id, r->id_len) == 0) on_board = r->games == 1;
    }
    T_CHECK(on_board);
    T_EQ_INT(call(&A, NC_DO_HISTORY, NULL, NULL, 0, -1), 200);
    T_EQ_INT(nc_list_read(g_status, g_body, g_len, NW_HISTORY), 1);
    const NcHistoryHead *head = nc_history_head(g_body, g_len);
    T_CHECK(head && head->has_rating);
    // The list carries no moves and no extras: a row is the replay's id and the summary, and the id opens the replay.
    const NcHistoryRow *hrow = nc_history_row(g_body, g_len, 0);
    T_CHECK(hrow && hrow->id_len > 0 && hrow->rank > 0);
    char rid[NW_ID_MAX + 1] = "";
    if (hrow) { memcpy(rid, hrow->id, hrow->id_len); rid[hrow->id_len] = 0; }
    int hist_len = g_len;
    uint8_t hist_body[4096];
    T_CHECK(hist_len > 0 && (size_t)hist_len <= sizeof hist_body);
    memcpy(hist_body, g_body, (size_t)hist_len);
    T_EQ_INT(call(&A, NC_DO_REPLAY, rid, NULL, 0, -1), 200);
    const NcReplay *rp = nc_replay_read(g_status, g_body, g_len);
    T_CHECK(rp && rp->moves_len > 0);
    if (rp) {
        T_CHECK(memmem(hist_body, (size_t)hist_len, g_body + rp->moves_off, rp->moves_len > 16 ? 16 : rp->moves_len) == NULL);
    }
    // The game's id opens it too.
    T_EQ_INT(call(&A, NC_DO_REPLAY, gid, NULL, 0, -1), 200);
    rp = nc_replay_read(g_status, g_body, g_len);
    T_CHECK(rp && rp->moves_len > 0);

    // Normal play never trips the inline valve (net/server.h): the moves ran on the loop thread,
    // and nothing the game did (its deal, its end, the rating, the replay job) stalled it.
    tc_response hr;
    T_EQ_INT(tc_http(g_port, "GET", "/health", NULL, NULL, 0, &hr), 0);
    const char *ht = cb_buf_cstr(&hr.body), *in = strstr(ht, " inline="), *tr = strstr(ht, " tripped=");
    fprintf(stderr, "smoke: %s", ht);
    T_CHECK(in && atoll(in + 8) > 0);
#ifndef CB_INSTRUMENTED
    // A time budget: held by the optimized build only (ASan's and coverage's run several times slower).
    T_CHECK(tr && atoll(tr + 9) == 0);
#else
    T_CHECK(tr != NULL);
#endif
    tc_response_free(&hr);

    // The view, then a restart on the same database: the same view.
    int64_t v1 = -1, v2 = -2;
    cb_buf env2;
    cb_buf_init(&env2);
    T_CHECK(game_view(&A, gid, &v1, &env));
    T_EQ_INT(tp_stop(&g_child, SIGTERM, 15000), 0);
    start_server();
    T_CHECK(game_view(&A, gid, &v2, &env2));
    T_EQ_INT(v2, v1);
    T_EQ_MEM(env2.p, env2.len, env.p, env.len);
    T_CHECK(env.len > 0);

    cb_buf_free(&env);
    cb_buf_free(&env2);
    free(sc);
}


// ---- a rematch rates its own game -----------------------------------------------------------
//
// Table 824ee3 in production (2026-10-28): a rematch on the same table ended, and every seat's
// rating moved by the PREVIOUS game's changes again (the fool gained), and the end screen showed
// that game's numbers beside this game's finish order. The server keeps a game it touched as a
// resident (journal.h), and the resident carried the first game's stamp through the rematch.
// Two humans, two games at one table, everything over HTTP as a client does it: each game's end
// screen (its envelope) shows that game's own stamp - the ratings it began from, which for the
// second game are the ones the first left, and the changes over its own finish order - and the
// leaderboard holds exactly what the stamps say.

// The stamp u's envelope of the finished game shows, by seat (n 0 when unrated), and its finish
// order best first (anim_finish_rows over the board's eliminations and fool).
static bool rematch_stamp(smoke_user *u, const char *gid, smoke_client *sc, cb_buf *env, GameRatings *st,
                          int8_t order[MAX_PLAYERS]) {
    int64_t v;
    memset(st, 0, sizeof *st);
    if (!game_view(u, gid, &v, env)) return false;
    client_init(&sc->c, &sc->slot);
    if (client_adopt_envelope(&sc->c, env->p, (int)env->len) < 0 || sc->c.view.status != GAME_STATUS_GAME_OVER) return false;
    const int np = sc->c.g->num_players;
    if (sc->c.view.rated) {
        st->n = (uint8_t)np;
        for (int s = 0; s < np; s++) { st->before[s] = sc->c.view.rating_before[s]; st->delta[s] = sc->c.view.rating_delta[s]; }
    }
    unsigned char elim[MAX_PLAYERS];
    for (int i = 0; i < sc->c.g->num_eliminated; i++) elim[i] = (unsigned char)sc->c.g->elimination_order[i];
    AnimFinishRow rows[MAX_PLAYERS + 1];
    const int n = anim_finish_rows(elim, sc->c.g->num_eliminated, sc->c.view.fool, np, -1, rows, MAX_PLAYERS + 1);
    if (n != np) return false;
    for (int i = 0; i < n; i++) order[i] = (int8_t)rows[i].seat;
    return true;
}

// The two humans play the dealt game to its end. The game's status.
static int rematch_play(smoke_user *humans[2], const char *gid, smoke_client *sc, cb_buf *env, uint64_t *rng) {
    int status = GAME_STATUS_PLAYING;
    const int64_t t0 = mono_ms();
    while (status == GAME_STATUS_PLAYING && mono_ms() - t0 < 300000) {
        bool moved = false;
        for (int h = 0; h < 2 && status == GAME_STATUS_PLAYING; h++) {
            int64_t seen;
            if (!game_view(humans[h], gid, &seen, env)) continue;
            client_init(&sc->c, &sc->slot);
            if (client_adopt_envelope(&sc->c, env->p, (int)env->len) < 0) continue;
            status = sc->c.view.status;
            uint8_t wire[64];
            const int n = choose_move(sc, env, wire, sizeof wire, rng);
            if (n <= 0) continue;
            T_EQ_INT(call(humans[h], NC_DO_PLAY, gid, wire, n, sc->c.view.version), 200);
            moved = true;
        }
        if (!moved) {
            struct timespec ts = { 0, 20 * 1000000 };
            nanosleep(&ts, NULL);
        }
    }
    return status;
}

// The leaderboard's rating of each of the two humans, by seat (A at 0, B at 1).
static bool rematch_board(smoke_user *A, smoke_user *B, int32_t out[2]) {
    out[0] = out[1] = -1;
    if (call(A, NC_DO_LEADERBOARD, NULL, NULL, 0, -1) != 200) return false;
    const int rows = nc_list_read(g_status, g_body, g_len, NW_BOARD);
    for (int i = 0; i < rows; i++) {
        const NcBoardRow *r = nc_board_row(g_body, g_len, i);
        if (!r || r->is_bot) continue;
        if (r->id_len == strlen(A->id) && memcmp(r->id, A->id, r->id_len) == 0) out[0] = (int32_t)r->rating;
        if (r->id_len == strlen(B->id) && memcmp(r->id, B->id, r->id_len) == 0) out[1] = (int32_t)r->rating;
    }
    return out[0] >= 0 && out[1] >= 0;
}

static void a_rematch_rates_its_own_game(void) {
    smoke_user A, B;
    int sa = signup(&A, "REMATCHA"), sb = signup(&B, "REMATCHB");
    T_EQ_INT(sa, 0);
    T_EQ_INT(sb, 0);
    if (sa || sb) return;
    smoke_client *sc = calloc(1, sizeof *sc);
    cb_buf env;
    cb_buf_init(&env);
    smoke_user *humans[2] = { &A, &B };
    uint64_t rng = 0x5eed;
    T_EQ_INT(call(&A, NC_DO_CREATE, NULL, NULL, 0, -1), 200);
    client_init(&sc->c, &sc->slot);
    T_CHECK(client_adopt_envelope(&sc->c, g_body, g_len) >= 0);
    char gid[32];
    snprintf(gid, sizeof gid, "%.*s", (int)sc->c.gid_len, sc->c.gid);
    T_EQ_INT(ask(&B, ASK_JOIN, gid, &env), 200);

    GameRatings stamp[2];
    int8_t order[2][MAX_PLAYERS];
    int32_t board[3][2];
    // A new account is on no board until it has played: it starts at the kernel's start.
    rematch_board(&A, &B, board[0]);
    for (int s = 0; s < 2; s++) if (board[0][s] < 0) board[0][s] = TABLE_RATING_START;
    for (int g = 0; g < 2; g++) {
        if (g == 1) T_EQ_INT(ask(&B, ASK_CONTINUE, gid, &env), 200);   // the rematch: same table, back to its lobby
        T_EQ_INT(ask(&A, ASK_START, gid, &env), 200);
        T_EQ_INT(ask(&B, ASK_START, gid, &env), 200);
        T_EQ_INT(rematch_play(humans, gid, sc, &env, &rng), GAME_STATUS_GAME_OVER);
        T_CHECK(rematch_stamp(&B, gid, sc, &env, &stamp[g], order[g]));
        T_CHECK(rematch_board(&A, &B, board[g + 1]));
        T_EQ_INT(stamp[g].n, 2);
        int32_t want[MAX_PLAYERS];
        T_EQ_INT(elo_deltas(board[g], order[g], 2, want), 2);
        fprintf(stderr, "smoke: rematch game %d: fool seat %d, stamp %d%+d %d%+d (want %+d %+d), board %d %d -> %d %d\n", g + 1,
                order[g][1], stamp[g].before[0], stamp[g].delta[0], stamp[g].before[1], stamp[g].delta[1], want[0], want[1],
                board[g][0], board[g][1], board[g + 1][0], board[g + 1][1]);
        for (int s = 0; s < 2; s++) {
            T_EQ_INT(stamp[g].before[s], board[g][s]);          // rated from the ratings this game began with
            T_EQ_INT(stamp[g].delta[s], want[s]);               // by its own finish order
            T_EQ_INT(board[g + 1][s], board[g][s] + stamp[g].delta[s]);   // and applied as stamped, once
        }
    }
    // Game 2's stamp is not game 1's (game 1 moved the ratings, so game 2 began elsewhere).
    T_CHECK(stamp[1].before[0] != stamp[0].before[0]);
    cb_buf_free(&env);
    free(sc);
}
// ---- kills at any moment lose no acknowledged move --------------------------------------
//
// THE JOURNAL'S PROMISE, as production meets it (journal.h): a move is acknowledged once its batch
// is in the segment's pages, and a process killed at any moment keeps every acknowledged move. Here
// three tables play at once, a human against two bots, two humans against a bot and two humans
// alone; the humans move, which wakes the bots, and the server is SIGKILLed a few moves in, at
// whatever point the bots' drives have reached. Every version a client was told (a move's answer,
// a read) must be there after the restart, and the version a human last read must read again as
// the same envelope, byte for byte. The games go on after each restart (a recovered board the kernel
// would not take refuses the next move), and an ended one is dealt again.
typedef struct {
    char        gid[32];
    smoke_user *h[2];
    int         nh, bots;
    int64_t     acked;   // the highest version any client was told
    int64_t     seen_v;  // the version h[0] last read, and its envelope's hash
    uint64_t    seen_hash;
    bool        over;
} crash_game;

static bool crash_deal(crash_game *g, smoke_client *sc, cb_buf *env) {
    if (call(g->h[0], NC_DO_CREATE, NULL, NULL, 0, -1) != 200) return false;
    client_init(&sc->c, &sc->slot);
    if (client_adopt_envelope(&sc->c, g_body, g_len) < 0) return false;
    snprintf(g->gid, sizeof g->gid, "%.*s", (int)sc->c.gid_len, sc->c.gid);
    bool ok = true;
    for (int k = 1; k < g->nh; k++) ok &= ask(g->h[k], ASK_JOIN, g->gid, env) == 200;
    for (int b = 0; b < g->bots; b++) ok &= ask(g->h[0], ASK_ADD_BOT, g->gid, env) == 200;
    for (int k = 0; k < g->nh; k++) ok &= ask(g->h[k], ASK_START, g->gid, env) == 200;
    g->acked = g->seen_v = -1;
    g->over = false;
    return ok;
}

// h[k]'s read of the game, noted. false when it does not read.
static bool crash_read(crash_game *g, int k, smoke_client *sc, cb_buf *env) {
    int64_t v;
    if (!game_view(g->h[k], g->gid, &v, env)) return false;
    if (v > g->acked) g->acked = v;
    if (k == 0) {
        g->seen_v = v;
        g->seen_hash = fnv1a64(FNV64_OFFSET, env->p, env->len);
    }
    client_init(&sc->c, &sc->slot);
    if (client_adopt_envelope(&sc->c, env->p, (int)env->len) < 0) return false;
    g->over = sc->c.view.status != GAME_STATUS_PLAYING;
    return true;
}

static void kills_lose_no_acknowledged_move(void) {
    smoke_user U[3];
    for (int i = 0; i < 3; i++) {
        char name[24];   // room for any int (GCC's format-truncation check)
        snprintf(name, sizeof name, "SMOKEK%d", i);
        if (signup(&U[i], name) != 0) { T_CHECK(0); return; }
    }
    crash_game G[3] = {
        { .h = { &U[0] }, .nh = 1, .bots = 2 },
        { .h = { &U[1], &U[2] }, .nh = 2, .bots = 1 },
        { .h = { &U[2], &U[0] }, .nh = 2, .bots = 0 },
    };
    smoke_client *sc = calloc(1, sizeof *sc);
    cb_buf env;
    cb_buf_init(&env);
    for (int i = 0; i < 3; i++) T_CHECK(crash_deal(&G[i], sc, &env));
    // CBASE_SMOKE_KILLS rounds (8; more to hunt a rare loss).
    const char *kv = getenv("CBASE_SMOKE_KILLS");
    const int rounds = kv && atoi(kv) > 0 ? atoi(kv) : 8;
    uint64_t rng = 0x5eed;
    int kills = 0, same = 0, ahead = 0, moves = 0, deals = 3;
    for (int round = 0; round < rounds; round++) {
        // A few human moves, anywhere: each one wakes the bots of its table.
        const int want = 1 + (int)(rng_u32(&rng) % 6);
        for (int made = 0, tries = 0; made < want && tries < 400; tries++) {
            crash_game *g = &G[rng_u32(&rng) % 3];
            const int k = (int)(rng_u32(&rng) % (uint32_t)g->nh);
            if (g->over) {
                T_CHECK(crash_deal(g, sc, &env));
                deals++;
                continue;
            }
            if (!crash_read(g, k, sc, &env)) { T_CHECK(0); continue; }
            uint8_t wire[64];
            const int n = choose_move(sc, &env, wire, sizeof wire, &rng);
            if (n <= 0) {   // not this human's turn: the bots' maybe, a moment for them
                cb_sleep_ms(5);
                continue;
            }
            const int64_t at = sc->c.view.version;
            T_EQ_INT(call(g->h[k], NC_DO_PLAY, g->gid, wire, n, at), 200);
            const NcAction *a = nc_action_read(g_status, g_body, g_len);
            T_CHECK(a != NULL);
            if (a && a->status == TABLE_STATUS_APPLIED && (int64_t)a->version > g->acked) g->acked = a->version;
            made++;
            moves++;
        }
        // The kill up to 400 ms after the last move's answer, while the bots it woke play (their
        // pace is production's over SMOKE_SPEEDUP), wherever their drives are: on even rounds at
        // once after what h[0] reads then (their commits so far acknowledged, the envelope compared).
        cb_sleep_ms(rng_u32(&rng) % 400);
        if (round % 2 == 0)
            for (int i = 0; i < 3; i++) if (!G[i].over) (void)crash_read(&G[i], 0, sc, &env);
        (void)tp_stop(&g_child, SIGKILL, 5000);   // a killed child has no exit status
        kills++;
        start_server();
        if (g_port <= 0) break;
        for (int i = 0; i < 3; i++) {
            crash_game *g = &G[i];
            const int64_t acked = g->acked, seen_v = g->seen_v;
            const uint64_t seen_hash = g->seen_hash;
            T_CHECK(crash_read(g, 0, sc, &env));
            if (g->seen_v < acked)
                fprintf(stderr, "smoke: game %s is at version %lld after the kill, and version %lld was acknowledged\n", g->gid,
                        (long long)g->seen_v, (long long)acked);
            T_CHECK(g->seen_v >= acked);
            ahead += g->seen_v > acked;   // the bots committed after the last answer, before the kill
            if (round % 2 == 0 && g->seen_v == seen_v) {
                T_CHECK(g->seen_hash == seen_hash);
                same++;
            }
        }
    }
    fprintf(stderr, "smoke: %d kills over %d human moves in %d deals; %d reads after a kill matched the last one before it, %d were ahead of it\n",
            kills, moves, deals, same, ahead);
    T_EQ_INT(kills, rounds);
    T_CHECK(moves >= 8);
    T_CHECK(same > 0);
    cb_buf_free(&env);
    free(sc);
}

// The last human leaving a lobby that still seats a bot deletes it, as a client sees it: a lobby
// of bots alone can never be readied, edited or dealt, so a kept row would linger for good.
static void a_lobby_left_to_its_bots_is_deleted(void) {
    smoke_user L;
    T_EQ_INT(signup(&L, "SMOKEL"), 0);
    T_EQ_INT(call(&L, NC_DO_CREATE, NULL, NULL, 0, -1), 200);
    smoke_client *sc = calloc(1, sizeof *sc);
    client_init(&sc->c, &sc->slot);
    T_CHECK(client_adopt_envelope(&sc->c, g_body, g_len) >= 0);
    char gid[32];
    snprintf(gid, sizeof gid, "%.*s", (int)sc->c.gid_len, sc->c.gid);
    cb_buf env;
    cb_buf_init(&env);
    T_EQ_INT(ask(&L, ASK_ADD_BOT, gid, &env), 200);
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = ASK_EXIT;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    a.player_len = (uint8_t)strlen(L.id);
    memcpy(a.player, L.id, a.player_len);
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    T_EQ_INT(n > 0 ? call(&L, NC_DO_ASK, NULL, body, n, -1) : -1, 200);
    int64_t seen = -1;
    T_CHECK(!game_view(&L, gid, &seen, NULL));   // the row is gone: no view for anybody
    T_EQ_INT(call(&L, NC_DO_OPEN_GAME, gid, NULL, 0, -1), 404);
    cb_buf_free(&env);
    free(sc);
}

// ---- guessed game ids (app/misses.h) ---------------------------------------------------------
//
// A script walking six-hex ids for open lobbies, against a second server that sits behind the
// edge (CBASE_ORIGIN_SECRET, which every request here proves), so each request names its
// viewer's address and two addresses can be told apart over one loopback socket. Address A's
// first CB_LIMIT_ID_MISS_IP burst (30) of misses are 404s and the 31st is 429 with Retry-After;
// then A's own game still opens and long-polls while a stranger's is refused, and address B,
// misses and all, is untouched.
#define SMOKE_ORIGIN "smoke-origin-secret-0123456789abcdef0123"
#define SMOKE_VIA(addr) CB_ORIGIN_HEADER ": " SMOKE_ORIGIN "\r\n" CB_VIEWER_HEADER ": " addr "\r\n"

// GET <prefix><id> as u.
static int get_as(smoke_user *u, const char *prefix, const char *id) {
    T_EQ_INT(nc_session_load(u->blob, u->blob_len), NW_OK);
    const NcRequest *b = nc_req_game("000000", 6, cb_now_ms());
    if (!b || b->is_refresh) { T_CHECK(0); return -1; }
    NcRequest q = *b;
    q.path_len = (uint8_t)snprintf(q.path, sizeof q.path, "%s%s", prefix, id);
    return send_req(&q);
}

// u's newest game's id, from the dashboard.
static bool newest_game(smoke_user *u, char gid[CB_GAME_ID_MAX + 1]) {
    if (call(u, NC_DO_CREATE, NULL, NULL, 0, -1) != 200) return false;
    smoke_client *sc = calloc(1, sizeof *sc);
    if (!sc) return false;
    client_init(&sc->c, &sc->slot);
    bool ok = client_adopt_envelope(&sc->c, g_body, g_len) >= 0 && sc->c.gid_len <= CB_GAME_ID_MAX;
    if (ok) snprintf(gid, CB_GAME_ID_MAX + 1, "%.*s", (int)sc->c.gid_len, sc->c.gid);
    free(sc);
    return ok;
}

static void guessed_ids_are_limited_per_address(void) {
    char dir[400], line[512], envdb[512];
    if (tp_tempdir(dir, sizeof dir) != 0) { T_CHECK(0); return; }
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/cbase.sqlite", dir);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", envdb, "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret",
                    "CBASE_ORIGIN_SECRET=" SMOKE_ORIGIN, NULL };
    char *argv[] = { (char *)g_bin, "serve", NULL };
    tp_child child;
    T_EQ_INT(tp_start(&child, argv, env), 0);
    T_EQ_INT(tp_wait_line(&child, "listening on", 20000, line, sizeof line), 0);
    const char *colon = strrchr(line, ':');
    const int keep = g_port;
    g_port = colon ? atoi(colon + 1) : 0;
    T_CHECK(g_port > 0);

    smoke_user a, b;
    char mine[CB_GAME_ID_MAX + 1] = "", theirs[CB_GAME_ID_MAX + 1] = "";
    g_via = SMOKE_VIA("198.51.100.70:40001");
    T_EQ_INT(signup(&a, "GUESSER"), 0);
    T_CHECK(newest_game(&a, mine));
    g_via = SMOKE_VIA("198.51.100.71:40002");
    T_EQ_INT(signup(&b, "BYSTANDER"), 0);
    T_CHECK(newest_game(&b, theirs));

    // Address A: a stranger's game found is free, every miss costs one.
    g_via = SMOKE_VIA("198.51.100.70:40003");
    T_EQ_INT(get_as(&a, "/v1/games/", theirs), 200);
    // Ids no game has: walked from f00000, stepping over the two that are games.
    char guess[8];
    int next = 0xf00000, missed = 0;
    for (int i = 0; i < 30; i++) {
        do snprintf(guess, sizeof guess, "%06x", next++);
        while (strcmp(guess, mine) == 0 || strcmp(guess, theirs) == 0);
        missed += get_as(&a, "/v1/games/", guess) == 404;
    }
    T_EQ_INT(missed, 30);
    do snprintf(guess, sizeof guess, "%06x", next++);
    while (strcmp(guess, mine) == 0 || strcmp(guess, theirs) == 0);
    T_EQ_INT(get_as(&a, "/v1/games/", guess), 429);
    T_EQ_INT(nc_status_read(g_status, g_body, g_len), ASK_WHY_BUSY);
    T_CHECK(atoi(g_retry_after) > 0);
    // A's own game still opens and long-polls; a stranger's is refused unread.
    T_EQ_INT(get_as(&a, "/v1/games/", mine), 200);
    T_EQ_INT(get_as(&a, "/v1/wait/", mine), 200);
    T_EQ_INT(get_as(&a, "/v1/games/", theirs), 429);
    // Address B is not limited: its misses are 404s and A's game opens for it.
    g_via = SMOKE_VIA("[2001:db8:7:7::1]:40004");
    T_EQ_INT(get_as(&b, "/v1/games/", guess), 404);
    T_EQ_INT(get_as(&b, "/v1/games/", mine), 200);

    g_via = "";
    g_port = keep;
    T_EQ_INT(tp_stop(&child, SIGTERM, 15000), 0);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", dir);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: smoke <path to cbase>\n"); return 2; }
    g_bin = argv[1];
    T_RUN(starts_and_listens);
    if (g_port > 0) T_RUN(health);
    if (g_port > 0) T_RUN(unfurl);
    T_RUN(guessed_ids_are_limited_per_address);
    const char *game = getenv("CBASE_SMOKE_GAME");
    if (g_port > 0 && !(game && strcmp(game, "0") == 0)) T_RUN(an_idle_human_turn_is_never_kicked);
    if (g_port > 0 && !(game && strcmp(game, "0") == 0)) T_RUN(a_lobby_left_to_its_bots_is_deleted);
    if (g_port > 0 && !(game && strcmp(game, "0") == 0)) T_RUN(game_scenario);
    if (g_port > 0 && !(game && strcmp(game, "0") == 0)) T_RUN(a_rematch_rates_its_own_game);
    if (g_port > 0 && !(game && strcmp(game, "0") == 0)) T_RUN(kills_lose_no_acknowledged_move);
    else printf("NOTE smoke: the game scenario is off (CBASE_SMOKE_GAME=0)\n");
    T_RUN(drains_on_sigterm);
    if (g_dbdir[0]) {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dbdir);
        if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", g_dbdir);
    }
    return T_DONE();
}
