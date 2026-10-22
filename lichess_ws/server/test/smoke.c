// smoke.c - the gateway end to end against a real `lws serve`, a real Redis and a real MongoDB:
// the boot handshake, a site socket's ping, a lobby socket's auth and forwards, a game played
// through a player's and a watcher's sockets with lila played by this test over Redis, the
// resync rules, the fen watchers, the rate limit, the close codes.
//
//   build/test/smoke build/lws build/wsbench
//
// Needs Redis on LWS_TEST_REDIS_PORT (default 9679) and MongoDB on LWS_TEST_MONGO_PORT (default
// 27117), both empty of anything that matters (the test seeds its own documents under fixed ids).
#include "bson.h"
#include "lib_client.h"
#include "lib_proc.h"
#include "lib_redis.h"
#include "mongo.h"
#include "net/ws.h"
#include "t.h"
#include "util/clock.h"
#include "util/err.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_bin;
static int g_redis_port = 9679, g_mongo_port = 27117;
static char g_db[512];

static int env_port(const char *name, int dflt) { const char *v = getenv(name); return v && *v ? atoi(v) : dflt; }

static int start_server(tp_child *c) {
    char db_env[600], rp[64], mp[64];
    snprintf(db_env, sizeof db_env, "CBASE_DB=%s", g_db);
    snprintf(rp, sizeof rp, "LWS_REDIS_PORT=%d", g_redis_port);
    snprintf(mp, sizeof mp, "LWS_MONGO_PORT=%d", g_mongo_port);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_LOG_LEVEL=info", "CBASE_REQ_WORKERS=2",
                    "LWS_CSRF_ORIGIN=http://localhost:9663", db_env, rp, mp, NULL };
    char *argv[] = { (char *)g_bin, "serve", NULL };
    if (tp_start(c, argv, env) != 0) return -1;
    char line[512];
    if (tp_wait_line(c, "listening on 127.0.0.1:", 20000, line, sizeof line) != 0) return -1;
    const char *p = strstr(line, "listening on 127.0.0.1:");
    return atoi(p + strlen("listening on 127.0.0.1:"));
}

// ---- seeding MongoDB the way lila's documents look ----

static int seed(void) {
    mongo_cfg cfg = { .host = "127.0.0.1", .port = g_mongo_port, .db = "lichess", .timeout_ms = 3000 };
    mongo_conn *m = NULL;
    if (mongo_connect(&m, &cfg) != CB_OK) return -1;
    cb_buf d;
    cb_buf_init(&d);
    bson_w w;
    struct { const char *id, *name; bool enabled; } users[] = { { "alice", "Alice", true }, { "bob", "Bob", true }, { "carol", "Carol", false } };
    for (size_t i = 0; i < 3; i++) {
        cb_buf_reset(&d);
        bson_w_init(&w, &d); bson_w_begin(&w);
        bson_w_str(&w, "_id", users[i].id); bson_w_str(&w, "username", users[i].name); bson_w_bool(&w, "enabled", users[i].enabled);
        bson_w_end(&w);
        int rc = mongo_insert_one(m, "user4", &d);
        if (rc != CB_OK && rc != CB_E_CONFLICT) { mongo_close(m); return -1; }
    }
    struct { const char *sid, *user; bool up; } sessions[] = { { "sidalice", "alice", true }, { "sidbob", "bob", true }, { "sidcarol", "carol", true }, { "sidold", "alice", false } };
    for (size_t i = 0; i < 4; i++) {
        cb_buf_reset(&d);
        bson_w_init(&w, &d); bson_w_begin(&w);
        bson_w_str(&w, "_id", sessions[i].sid); bson_w_str(&w, "user", sessions[i].user); bson_w_bool(&w, "up", sessions[i].up);
        bson_w_end(&w);
        int rc = mongo_insert_one(m, "security", &d);
        if (rc != CB_OK && rc != CB_E_CONFLICT) { mongo_close(m); return -1; }
    }
    // the game: alice white (player id wwww), bob black (bbbb)
    cb_buf_reset(&d);
    bson_w_init(&w, &d); bson_w_begin(&w);
    bson_w_str(&w, "_id", "smokegm1"); bson_w_str(&w, "is", "wwwwbbbb");
    bson_w_array(&w, "us"); bson_w_str(&w, "0", "alice"); bson_w_str(&w, "1", "bob"); bson_w_end(&w);
    bson_w_int32(&w, "s", 20);
    bson_w_end(&w);
    int rc = mongo_insert_one(m, "game5", &d);
    cb_buf_free(&d);
    mongo_close(m);
    return rc == CB_OK || rc == CB_E_CONFLICT ? 0 : -1;
}

// ---- helpers ----

static int ws_text(int fd, int timeout_ms, cb_buf *out) {   // the next TEXT frame, skipping PINGs; -1 otherwise
    for (int i = 0; i < 8; i++) {
        int op = tc_ws_recv(fd, timeout_ms, out);
        if (op < 0) return -1;
        if (op == CB_WS_OP_TEXT) return 0;
        if (op == CB_WS_OP_CLOSE) return -2;
    }
    return -1;
}
static int send_text(int fd, const char *s) { return tc_ws_send(fd, CB_WS_OP_TEXT, s, strlen(s)); }
static bool got(const cb_buf *b, const char *want) { return b->len == strlen(want) && memcmp(b->p, want, b->len) == 0; }
static bool starts(const cb_buf *b, const char *want) { return b->len >= strlen(want) && memcmp(b->p, want, strlen(want)) == 0; }

#define COOKIE_ALICE "Cookie: lila2=sessionId=sidalice\r\n"
#define COOKIE_BOB "Cookie: lila2=sessionId=sidbob\r\n"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: smoke <lws binary> [wsbench]\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    g_bin = argv[1];
    g_redis_port = env_port("LWS_TEST_REDIS_PORT", 9679);
    g_mongo_port = env_port("LWS_TEST_MONGO_PORT", 27117);
    char dir[256];
    if (tp_tempdir(dir, sizeof dir) != 0) return 2;
    snprintf(g_db, sizeof g_db, "%s/lws.sqlite", dir);

    if (seed() != 0) { fprintf(stderr, "smoke: cannot seed MongoDB on 127.0.0.1:%d\n", g_mongo_port); return 1; }
    int sub = tr_connect(g_redis_port), pub = tr_connect(g_redis_port);
    if (sub < 0 || pub < 0 || tr_psubscribe(sub, "*-in*") != 0) { fprintf(stderr, "smoke: no Redis on 127.0.0.1:%d\n", g_redis_port); return 1; }

    tp_child srv;
    int port = start_server(&srv);
    T_CHECK_MSG(port > 0, "server did not start");
    if (port <= 0) return t_done("smoke");
    cb_buf m, chan_dummy;
    cb_buf_init(&m); cb_buf_init(&chan_dummy);
    char chan[64];

    // 1. the boot handshake: `boot` on every -in channel, the round's on a lane
    T_CHECK(tr_wait(sub, 5000, "site-in", "boot", &m) == 0);
    T_CHECK(tr_wait(sub, 2000, "lobby-in", "boot", &m) == 0);
    T_CHECK(tr_wait_prefix(sub, 2000, "r-in:", "boot", chan, sizeof chan, &m) == 0);
    T_STREQ(chan, "r-in:2");   // "boot".hashCode is 3029746; 3029746 % 16 = 2 (unit_lila_wire checks the rule)

    // 2. a site socket: pings, anonymous
    int site = tc_ws_open(port, "/socket/v5?sri=sri0site0001", NULL);
    T_CHECK(site >= 0);
    T_CHECK(send_text(site, "p") == 0);
    T_CHECK(ws_text(site, 2000, &m) == 0 && got(&m, "0"));
    T_CHECK(send_text(site, "{\"t\":\"p\",\"l\":42}") == 0);
    T_CHECK(ws_text(site, 2000, &m) == 0 && got(&m, "0"));
    // `null` is a ping too (old clients)
    T_CHECK(send_text(site, "null") == 0);
    T_CHECK(ws_text(site, 2000, &m) == 0 && got(&m, "0"));
    // a site socket needs an sri
    T_CHECK(tc_ws_open(port, "/socket/v5", NULL) < 0);
    // a wrong origin is refused
    T_CHECK(tc_ws_open(port, "/socket/v5?sri=x", "Origin: http://evil.example\r\n") < 0);
    T_CHECK(tc_ws_open(port, "/nothing/here?sri=x", NULL) < 0);

    // 3. a lobby socket as alice: connect/sris, the lobby pong, a forwarded join, lags
    int lobby = tc_ws_open(port, "/lobby/socket/v5?sri=sri0lobby001", COOKIE_ALICE);
    T_CHECK(lobby >= 0);
    T_CHECK(tr_wait(sub, 3000, "lobby-in", "connect/sris", &m) == 0 && got(&m, "connect/sris sri0lobby001 alice"));
    T_CHECK(send_text(lobby, "{\"t\":\"p\",\"l\":31}") == 0);
    T_CHECK(ws_text(lobby, 2000, &m) == 0 && starts(&m, "{\"t\":\"n\",\"d\":"));
    T_CHECK(tr_wait(sub, 3000, "site-in", "lags", &m) == 0 && got(&m, "lags alice:31"));
    const char *join = "{\"t\":\"join\",\"d\":{\"id\":\"abc\",\"color\":\"random\"}}";
    T_CHECK(tc_ws_send(lobby, CB_WS_OP_TEXT, join, strlen(join)) == 0);
    T_CHECK(tr_wait(sub, 3000, "lobby-in", "tell/sri", &m) == 0);
    T_CHECK_MSG(got(&m, "tell/sri sri0lobby001 alice {\"t\":\"join\",\"d\":{\"id\":\"abc\",\"color\":\"random\"}}"), "got %s", cb_buf_cstr(&m));
    // lila tells the lobby and the sri back
    T_CHECK(tr_publish(pub, "lobby-out", "tell/lobby {\"t\":\"reload_seeks\"}") == 0);
    T_CHECK(ws_text(lobby, 2000, &m) == 0 && got(&m, "{\"t\":\"reload_seeks\"}"));
    T_CHECK(tr_publish(pub, "lobby-out", "lobby/pairings sri0lobby001:smokegm1wwww") == 0);
    T_CHECK(ws_text(lobby, 2000, &m) == 0 && got(&m, "{\"t\":\"redirect\",\"d\":{\"id\":\"smokegm1wwww\",\"url\":\"/smokegm1wwww\"}}"));
    T_CHECK(tr_publish(pub, "site-out", "tell/users alice,nobody {\"t\":\"hello\"}") == 0);
    T_CHECK(ws_text(lobby, 2000, &m) == 0 && got(&m, "{\"t\":\"hello\"}"));
    // a disabled account is anonymous; a session that is not up too
    int carol = tc_ws_open(port, "/lobby/socket/v5?sri=sri0lobbycar", "Cookie: lila2=sessionId=sidcarol\r\n");
    T_CHECK(carol >= 0);
    T_CHECK(tr_wait(sub, 3000, "lobby-in", "connect/sris", &m) == 0 && got(&m, "connect/sris sri0lobbycar"));
    close(carol);

    // 4. the round: alice plays white, an anonymous watcher, lila (this test) on Redis.
    // First the challenge that became this game versions its own events under the same id on the
    // challenge channel (lila does: a challenge's id is its game's): that room is not the game's,
    // so the players below still get the game's crowd and the mover still loses the dests.
    T_CHECK(tr_publish(pub, "chal-out", "tell/room/version smokegm1 1 - {\"t\":\"reload\",\"d\":null}") == 0);
    T_CHECK(tr_publish(pub, "chal-out", "tell/room/version smokegm1 2 - {\"t\":\"reload\",\"d\":null}") == 0);
    T_CHECK(tc_ws_open(port, "/play/smokegm1wwww/v6?sri=sri0bob0001", COOKIE_BOB) < 0);    // bob is not white
    T_CHECK(tc_ws_open(port, "/play/smokegm1wwww/v6?sri=sri0anon001", NULL) < 0);          // nor is anyone anonymous
    T_CHECK(tc_ws_open(port, "/play/nogame00wwww/v6?sri=sri0anon002", NULL) < 0);          // no such game
    int play = tc_ws_open(port, "/play/smokegm1wwww/v6?sri=sri0play001", COOKIE_ALICE);
    T_CHECK(play >= 0);
    int watch = tc_ws_open(port, "/watch/smokegm1/white/v6?sri=sri0watch01", NULL);
    T_CHECK(watch >= 0);
    T_CHECK(tr_wait_prefix(sub, 3000, "r-in:", "r/ons", chan, sizeof chan, &m) == 0);
    T_CHECK_MSG(got(&m, "r/ons smokegm1+-"), "got %s", cb_buf_cstr(&m));
    T_CHECK(ws_text(play, 2000, &m) == 0 && got(&m, "{\"t\":\"crowd\",\"d\":{\"white\":true,\"black\":false,\"watchers\":{}}}"));
    T_CHECK(ws_text(watch, 2000, &m) == 0 && got(&m, "{\"t\":\"crowd\",\"d\":{\"white\":true,\"black\":false,\"watchers\":{}}}"));
    // the site socket watches the game's position
    T_CHECK(send_text(site, "{\"t\":\"startWatching\",\"d\":\"smokegm1 othergm1\"}") == 0);
    // lila publishes white's move: the mover and the watcher get it without dests
    const char *ev = "r/ver smokegm1 1 W move {\"uci\":\"e2e4\",\"san\":\"e4\",\"fen\":\"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR\",\"ply\":1,"
                     "\"dests\":{\"a7\":\"a6a5\",\"b8\":\"a6c6\"},\"clock\":{\"white\":180,\"black\":180.5,\"lag\":1}}";
    T_CHECK(tr_publish(pub, "r-out:3", ev) == 0);
    const char *nodests = "{\"t\":\"move\",\"v\":1,\"d\":{\"uci\":\"e2e4\",\"san\":\"e4\",\"fen\":\"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR\",\"ply\":1,"
                          "\"clock\":{\"white\":180,\"black\":180.5,\"lag\":1}}}";
    T_CHECK(ws_text(play, 2000, &m) == 0);
    T_CHECK_MSG(got(&m, nodests), "got %s", cb_buf_cstr(&m));
    T_CHECK(ws_text(watch, 2000, &m) == 0 && got(&m, nodests));
    T_CHECK(ws_text(site, 2000, &m) == 0);
    T_CHECK_MSG(got(&m, "{\"t\":\"fen\",\"d\":{\"id\":\"smokegm1\",\"lm\":\"e2e4\",\"fen\":\"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b\",\"wc\":180,\"bc\":180}}"),
                "got %s", cb_buf_cstr(&m));
    // black's move (lila's event): white, who did not move, gets the dests
    const char *ev2 = "r/ver smokegm1 2 B move {\"uci\":\"e7e5\",\"san\":\"e5\",\"fen\":\"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR\",\"ply\":2,\"dests\":{\"g1\":\"f3\"}}";
    T_CHECK(tr_publish(pub, "r-out:3", ev2) == 0);
    T_CHECK(ws_text(play, 2000, &m) == 0);
    T_CHECK_MSG(got(&m, "{\"t\":\"move\",\"v\":2,\"d\":{\"uci\":\"e7e5\",\"san\":\"e5\",\"fen\":\"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR\",\"ply\":2,\"dests\":{\"g1\":\"f3\"}}}"),
                "got %s", cb_buf_cstr(&m));
    T_CHECK(ws_text(watch, 2000, &m) == 0 && starts(&m, "{\"t\":\"move\",\"v\":2,") && !strstr(cb_buf_cstr(&m), "dests"));
    T_CHECK(ws_text(site, 2000, &m) == 0 && got(&m, "{\"t\":\"fen\",\"d\":{\"id\":\"smokegm1\",\"lm\":\"e7e5\",\"fen\":\"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w\"}}"));   // no clock in that event
    // an owners-only event: the watcher gets the skip form
    T_CHECK(tr_publish(pub, "r-out:3", "r/ver smokegm1 3 p takebackOffers {\"white\":true}") == 0);
    T_CHECK(ws_text(play, 2000, &m) == 0 && got(&m, "{\"t\":\"takebackOffers\",\"v\":3,\"d\":{\"white\":true}}"));
    T_CHECK(ws_text(watch, 2000, &m) == 0 && got(&m, "{\"v\":3}"));
    // the player moves: a PING frame with the clock, an ack, and r/move toward lila
    T_CHECK(send_text(play, "{\"t\":\"move\",\"d\":{\"u\":\"g1f3\",\"b\":1,\"l\":30,\"s\":\"1a\",\"a\":7}}") == 0);
    {
        int op = tc_ws_recv(play, 2000, &m);
        T_CHECK_MSG(op == CB_WS_OP_PING && m.len == 8, "op %d len %zu", op, m.len);
        if (op == CB_WS_OP_PING) {
            uint8_t pong[8]; memcpy(pong, m.p, 8);
            T_CHECK(tc_ws_send(play, CB_WS_OP_PONG, pong, 8) == 0);
        }
    }
    T_CHECK(ws_text(play, 2000, &m) == 0 && got(&m, "{\"t\":\"ack\",\"d\":7}"));
    T_CHECK(tr_wait_prefix(sub, 3000, "r-in:", "r/move", chan, sizeof chan, &m) == 0);
    T_CHECK_MSG(got(&m, "r/move smokegm1wwww g1f3 + 3 46 -"), "got %s", cb_buf_cstr(&m));
    // the next move carries the frame lag measured by that PONG (0 ms here: "-" means none, 0 means a measured zero)
    T_CHECK(send_text(play, "{\"t\":\"move\",\"d\":{\"u\":\"d2d4\"}}") == 0);
    T_CHECK(tr_wait_prefix(sub, 3000, "r-in:", "r/move", chan, sizeof chan, &m) == 0);
    T_CHECK_MSG(starts(&m, "r/move smokegm1wwww d2d4 - - - ") && !strstr(cb_buf_cstr(&m), "- - - -"), "got %s", cb_buf_cstr(&m));
    while (tc_ws_recv(play, 300, &m) >= 0) {}   // the PING and the ack
    // the watcher cannot move, but can flag; a player's actions are forwarded whole
    T_CHECK(send_text(watch, "{\"t\":\"flag\",\"d\":\"black\"}") == 0);
    T_CHECK(tr_wait_prefix(sub, 3000, "r-in:", "r/flag", chan, sizeof chan, &m) == 0 && got(&m, "r/flag smokegm1 b -"));
    T_CHECK(send_text(play, "{\"t\":\"resign\"}") == 0);
    T_CHECK(tr_wait_prefix(sub, 3000, "r-in:", "r/do", chan, sizeof chan, &m) == 0 && got(&m, "r/do smokegm1wwww {\"t\":\"resign\"}"));
    T_CHECK(send_text(play, "{\"t\":\"talk\",\"d\":\"gg wp\"}") == 0);
    T_CHECK(tr_wait_prefix(sub, 3000, "r-in:", "chat/say", chan, sizeof chan, &m) == 0 && got(&m, "chat/say smokegm1 alice gg wp"));
    T_CHECK(send_text(watch, "{\"t\":\"talk\",\"d\":\"hi\"}") == 0);   // anonymous watcher: nothing
    // gone / resync are for the right player
    T_CHECK(tr_publish(pub, "r-out:3", "r/gone smokegm1bbbb +") == 0);
    T_CHECK(ws_text(play, 2000, &m) == 0 && got(&m, "{\"t\":\"gone\",\"d\":true}"));
    T_CHECK(tr_publish(pub, "r-out:3", "r/resync/player smokegm1wwww") == 0);
    T_CHECK(ws_text(play, 2000, &m) == 0 && got(&m, "{\"t\":\"resync\"}"));
    T_CHECK(tr_publish(pub, "r-out:3", "tell/room smokegm1 {\"t\":\"tourStanding\",\"d\":1}") == 0);
    T_CHECK(ws_text(watch, 2000, &m) == 0 && got(&m, "{\"t\":\"tourStanding\",\"d\":1}"));

    // 5. resync: a reconnecting watcher at v=1 gets 2 and 3; at v=3 nothing; at v=-5 a resync; with no v the last 5
    int w2 = tc_ws_open(port, "/watch/smokegm1/white/v6?sri=sri0watch02&v=1", NULL);
    T_CHECK(w2 >= 0);
    T_CHECK(ws_text(w2, 2000, &m) == 0 && starts(&m, "{\"t\":\"move\",\"v\":2,"));
    T_CHECK(ws_text(w2, 2000, &m) == 0 && got(&m, "{\"v\":3}"));
    close(w2);
    int w3 = tc_ws_open(port, "/watch/smokegm1/white/v6?sri=sri0watch03&v=3", NULL);
    T_CHECK(w3 >= 0);
    T_CHECK(ws_text(w3, 1500, &m) == 0 && starts(&m, "{\"t\":\"crowd\""));   // only the crowd, no events
    close(w3);
    int w4 = tc_ws_open(port, "/watch/smokegm1/white/v6?sri=sri0watch04&v=-5", NULL);
    T_CHECK(w4 >= 0);
    T_CHECK(ws_text(w4, 2000, &m) == 0 && got(&m, "{\"t\":\"resync\"}"));
    close(w4);
    int w5 = tc_ws_open(port, "/watch/smokegm1/white/v6?sri=sri0watch05", NULL);
    T_CHECK(w5 >= 0);
    T_CHECK(ws_text(w5, 2000, &m) == 0 && starts(&m, "{\"t\":\"move\",\"v\":1,"));
    T_CHECK(ws_text(w5, 2000, &m) == 0 && starts(&m, "{\"t\":\"move\",\"v\":2,"));
    T_CHECK(ws_text(w5, 2000, &m) == 0 && got(&m, "{\"v\":3}"));
    close(w5);

    // 6. the game ends: the fen watcher hears it; lila stops the room: the sockets are closed (4010)
    T_CHECK(tr_publish(pub, "r-out:3", "r/finish smokegm1 w alice,bob") == 0);
    T_CHECK(ws_text(site, 2000, &m) == 0 && got(&m, "{\"t\":\"finish\",\"d\":{\"id\":\"smokegm1\",\"win\":\"w\"}}"));
    while (tc_ws_recv(watch, 200, &m) >= 0) {}
    T_CHECK(tr_publish(pub, "r-out:3", "room/stop smokegm1") == 0);
    {
        int op = tc_ws_recv(watch, 2000, &m);
        T_CHECK_MSG(op == CB_WS_OP_CLOSE && m.len >= 2 && ((m.p[0] << 8) | m.p[1]) == 4010, "op %d", op);
    }
    while (tc_ws_recv(play, 200, &m) >= 0) {}
    close(play); close(watch);
    // lila's boot handshake, after a stop: our versions, then versioning-ready
    T_CHECK(tr_publish(pub, "r-out", "boot") == 0);
    T_CHECK(tr_wait_prefix(sub, 3000, "r-in:", "room/versions", chan, sizeof chan, &m) == 0);
    T_CHECK_MSG(got(&m, "room/versions -"), "got %s", cb_buf_cstr(&m));   // the stopped room is forgotten
    T_CHECK(tr_publish(pub, "r-out", "r/versioning-ready") == 0);

    // 7. the rate limit (lobby: 30 per 30 s) closes the socket with 1008; an unknown message with 1011
    int rl = tc_ws_open(port, "/lobby/socket/v5?sri=sri0ratelim1", NULL);
    T_CHECK(rl >= 0);
    for (int i = 0; i < 31; i++) send_text(rl, "{\"t\":\"poolOut\",\"d\":\"1\"}");
    {
        int op = -1;
        for (int i = 0; i < 40 && op != CB_WS_OP_CLOSE; i++) op = tc_ws_recv(rl, 2000, &m);
        T_CHECK_MSG(op == CB_WS_OP_CLOSE && m.len >= 2 && ((m.p[0] << 8) | m.p[1]) == 1008, "op %d", op);
    }
    close(rl);
    int ux = tc_ws_open(port, "/socket/v5?sri=sri0unexpect", NULL);
    T_CHECK(ux >= 0);
    T_CHECK(send_text(ux, "{\"t\":\"nonsense\"}") == 0);
    {
        int op = tc_ws_recv(ux, 2000, &m);
        T_CHECK_MSG(op == CB_WS_OP_CLOSE && m.len >= 2 && ((m.p[0] << 8) | m.p[1]) == 1011, "op %d", op);
    }
    close(ux);
    int bin = tc_ws_open(port, "/socket/v5?sri=sri0binary01", NULL);
    T_CHECK(bin >= 0);
    T_CHECK(tc_ws_send(bin, CB_WS_OP_BIN, "xx", 2) == 0);
    {
        int op = tc_ws_recv(bin, 2000, &m);
        T_CHECK_MSG(op == CB_WS_OP_CLOSE && m.len >= 2 && ((m.p[0] << 8) | m.p[1]) == 1003, "op %d", op);
    }
    close(bin);

    // 8. the api socket: anonymous by design, and a kick closes a user's sockets
    int api = tc_ws_open(port, "/api/socket", NULL);
    T_CHECK(api >= 0);
    T_CHECK(send_text(api, "p") == 0);
    T_CHECK(ws_text(api, 2000, &m) == 0 && got(&m, "0"));
    T_CHECK(tr_publish(pub, "site-out", "tell/flag api {\"t\":\"apionly\"}") == 0);
    T_CHECK(ws_text(api, 2000, &m) == 0 && got(&m, "{\"t\":\"apionly\"}"));
    T_CHECK(tc_ws_recv(lobby, 300, &m) < 0);   // not the lobby socket
    close(api);
    T_CHECK(tr_publish(pub, "site-out", "disconnect/user alice") == 0);
    {
        int op = -1;
        for (int i = 0; i < 10 && op != CB_WS_OP_CLOSE; i++) op = tc_ws_recv(lobby, 2000, &m);
        T_CHECK_MSG(op == CB_WS_OP_CLOSE && m.len >= 2 && ((m.p[0] << 8) | m.p[1]) == 4010, "op %d", op);
    }
    close(lobby);
    T_CHECK(tr_wait(sub, 3000, "lobby-in", "disconnect/sris", &m) == 0 && starts(&m, "disconnect/sris sri0"));

    // 9. the stats line
    tc_response r;
    T_CHECK(tc_http(port, "GET", "/lws/stats", NULL, NULL, 0, &r) == 0 && r.status == 200);
    T_CHECK_MSG(strstr(cb_buf_cstr(&r.body), "lila=online") && strstr(cb_buf_cstr(&r.body), "redis=up"), "%s", cb_buf_cstr(&r.body));
    tc_response_free(&r);

    close(site);
    cb_buf_free(&m); cb_buf_free(&chan_dummy);
    close(sub); close(pub);
    int st = tp_stop(&srv, SIGTERM, 10000);
    T_CHECK_MSG(st == 0, "server exit status %d", st);
    return t_done("smoke");
}
