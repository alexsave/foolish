// foolish_server.c — a DEDICATED, in-memory Foolish server, in C.
//
// A second backend under server/impls (sibling to supabase/), to prove the
// server API is language-agnostic: same game, no TypeScript, no edge runtime,
// no Postgres. A long-lived process holds every game as a `Game` struct in RAM
// (the "in-memory authoritative state" of docs/ARCHITECTURE_AS_A_PATTERN.md),
// guarded by one mutex, and the C KERNEL drives all of it — this file only
// starts a socket, routes requests, and hands them to the kernel. Every rule
// (deal, legality, apply, refill, who-is-the-fool, the masked per-seat view)
// is c/src/*.c, exactly as the wasm/edge build uses it. Swap Postgres for a
// hash table and the edge runtime for a thread pool and the game is unchanged.
//
// POC scope: HTTP/1.1 (hand-rolled — a real deployment would drop in mongoose
// or civetweb), token auth in a memory map (no JWT), thread-per-connection with
// a global lock (single-writer per store op). Endpoints mirror the contract:
//   POST /auth/signup {username}         -> {token,user_id}
//   POST /auth/signin {username}         -> {token,user_id}
//   POST /create            (bearer)     -> {game_id}
//   POST /meta {type,game_id[,strategy]} (bearer)  type: join|start|add-bot
//   POST /action {game_id,move:{...}}    (bearer)  applies + runs bots
//   GET  /state?game_id=..&seat=..       -> the kernel's masked view JSON
//   GET  /health

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "game.h"
#include "legal.h"
#include "view.h"
#include "awire.h"
#include "bot_drive.h"
#include "bot_roster.h"

// --------------------------------------------------------------------------
// In-memory store (the "fake DB"): games + users, one global lock.
// --------------------------------------------------------------------------

#define MAX_GAMES 256
#define MAX_USERS 512
#define ID_LEN 12

typedef struct {
    bool used;
    char token[33];
    char user_id[ID_LEN + 1];
    char username[24];
} User;

typedef struct {
    bool used;
    char id[ID_LEN + 1];
    Game game;                      // THE kernel state — incl. its own lifecycle status
    // Lobby roster (identity lives beside the state blob, never in it — game.h).
    char owner[ID_LEN + 1];
    char seat_user[MAX_PLAYERS][ID_LEN + 1];  // "" for a bot
    char seat_name[MAX_PLAYERS][24];
    bool seat_ready[MAX_PLAYERS];             // lobby "hit ready" — host state; kind (human/bot) lives in the kernel's strategy_key
    // One per-game trampoline thread paces the bots (see bot_thread). It waits on
    // `cond` when a human is owed; /action signals it. `bot_running` guards
    // against spawning a second driver.
    pthread_cond_t cond;
    bool bot_running;
} GameSlot;

static User     g_users[MAX_USERS];
static GameSlot g_games[MAX_GAMES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_seq = 0;

// --------------------------------------------------------------------------
// Small utilities
// --------------------------------------------------------------------------

static void gen_id(char *out, int n) {
    static const char hex[] = "0123456789abcdef";
    unsigned long v = (++g_seq) ^ ((unsigned long)rand() << 8) ^ (unsigned long)time(NULL);
    for (int i = 0; i < n; i++) { out[i] = hex[v & 0xf]; v = v * 6364136223846793005UL + 1442695040888963407UL; v >>= 3; }
    out[n] = 0;
}

// Minimal JSON scrapers — enough for our flat request bodies. Not a parser.
static bool json_str(const char *body, const char *key, char *out, int cap) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = body ? strstr(body, pat) : NULL;
    if (!p) return false;
    p = strchr(p + strlen(pat), ':'); if (!p) return false;
    p++; while (*p == ' ' || *p == '"') p++;
    int i = 0; while (*p && *p != '"' && *p != ',' && *p != '}' && i < cap - 1) out[i++] = *p++;
    out[i] = 0; return i > 0;
}
static User *user_by_token(const char *token) {
    if (!token || !*token) return NULL;
    for (int i = 0; i < MAX_USERS; i++)
        if (g_users[i].used && strcmp(g_users[i].token, token) == 0) return &g_users[i];
    return NULL;
}
static GameSlot *game_by_id(const char *id) {
    for (int i = 0; i < MAX_GAMES; i++)
        if (g_games[i].used && strcmp(g_games[i].id, id) == 0) return &g_games[i];
    return NULL;
}
// Whether a seat is a bot: the kernel's own per-seat fact now (strategy_key),
// not a server-side is_ai array. A human seat is STRATEGY_KEY_HUMAN; a bot holds
// its roster index.
static bool seat_is_bot(const Game *g, int i) { return g->players[i].strategy_key != STRATEGY_KEY_HUMAN; }

static int seat_of(GameSlot *s, const char *user_id) {
    for (int i = 0; i < s->game.num_players; i++)
        if (!seat_is_bot(&s->game, i) && strcmp(s->seat_user[i], user_id) == 0) return i;
    return -1;
}

// The bot game-loop, one thread per game — a TRAMPOLINE, not a blocking hook.
// Each pass drives exactly ONE kernel cycle and RETURNS from the kernel; the
// server then decides how to wait. Same split as supabase (which `await`s a
// setTimeout) and the phone (Task.sleep): the KERNEL owns the cycle and the
// delay value (bot_drive + bot_pacing_ms); the host owns how it waits. The
// `Game` struct IS the continuation, so "resume" is just the next bot_drive.
//
// The lock is held while touching the game and RELEASED during the pacing sleep,
// so bots think + throw in over time while /action and /state keep serving. When
// no bot can act (a human is owed) the thread waits on `cond` until /action
// signals it.
static void *bot_thread(void *arg) {
    GameSlot *s = arg;
    pthread_mutex_lock(&g_lock);
    while (s->used && s->game.status == GAME_STATUS_PLAYING) {
        uint32_t hmask = game_human_mask(&s->game);   // the kernel's own human-seat mask
        BotDriveOut drv;
        bot_drive(&s->game, hmask, BOT_DRIVE_MAX_ACTIONS, 0, 0, &drv);   // ONE cycle, then returns

        if (drv.ended >= 0) break;   // the kernel already flipped g->status to GAME_OVER
        if (drv.stop == BOT_STOP_NO_ELIGIBLE) {              // a human's move is owed
            pthread_cond_wait(&s->cond, &g_lock);            // sleep until /action wakes us
            continue;
        }

        // A visible cycle landed — the kernel prices the wait; the host owns the
        // loop and the sleep (the trampoline). Lock released while we wait.
        int delay = bot_cycle_delay_ms(&s->game, hmask, &drv);
        if (delay > 0) {
            pthread_mutex_unlock(&g_lock);
            usleep((useconds_t)delay * 1000);
            pthread_mutex_lock(&g_lock);
        }
    }
    s->bot_running = false;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

// Spawn the game-loop for a freshly dealt game (idempotent).
static void start_bot_loop(GameSlot *s) {
    if (s->bot_running) return;
    s->bot_running = true;
    pthread_t t;
    if (pthread_create(&t, NULL, bot_thread, s) == 0) pthread_detach(t);
    else s->bot_running = false;
}

// --------------------------------------------------------------------------
// HTTP layer (hand-rolled; swap for mongoose in a real deployment)
// --------------------------------------------------------------------------

typedef struct { char method[8]; char path[256]; char query[256]; char token[64]; char *body; int body_len; } Req;

static void respond(int fd, int code, const char *json) {
    const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request"
                    : code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : "Error";
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: *\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        code, msg, strlen(json));
    write(fd, hdr, n);
    write(fd, json, strlen(json));
}

// Raw bytes (the packed kernel wire) — no JSON. The client decodes with its own
// kernel-wire reader (MaskedView etc.).
static void respond_bin(int fd, int code, const unsigned char *data, int len) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d OK\r\nContent-Type: application/octet-stream\r\n"
        "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        code, len);
    write(fd, hdr, n);
    if (len > 0) write(fd, data, (size_t)len);
}

// --------------------------------------------------------------------------
// Route handlers  (each locks g_lock around store access)
// --------------------------------------------------------------------------

static void h_signup(Req *r, int fd) {
    char uname[24] = {0};
    if (!json_str(r->body, "username", uname, sizeof uname)) { respond(fd, 400, "{\"error\":\"username\"}"); return; }
    pthread_mutex_lock(&g_lock);
    User *u = NULL;
    for (int i = 0; i < MAX_USERS; i++) if (g_users[i].used && !strcmp(g_users[i].username, uname)) { u = &g_users[i]; break; }
    if (!u) for (int i = 0; i < MAX_USERS; i++) if (!g_users[i].used) {
        u = &g_users[i]; u->used = true; snprintf(u->username, sizeof u->username, "%s", uname);
        gen_id(u->user_id, ID_LEN); break;
    }
    if (u) gen_id(u->token, 32);   // fresh session token
    char out[160];
    if (u) snprintf(out, sizeof out, "{\"token\":\"%s\",\"user_id\":\"%s\",\"username\":\"%s\"}", u->token, u->user_id, u->username);
    pthread_mutex_unlock(&g_lock);
    if (u) respond(fd, 200, out); else respond(fd, 400, "{\"error\":\"full\"}");
}

static void h_create(Req *r, int fd) {
    pthread_mutex_lock(&g_lock);
    User *u = user_by_token(r->token);
    if (!u) { pthread_mutex_unlock(&g_lock); respond(fd, 401, "{\"error\":\"auth\"}"); return; }
    GameSlot *s = NULL;
    for (int i = 0; i < MAX_GAMES; i++) if (!g_games[i].used) { s = &g_games[i]; break; }
    if (!s) { pthread_mutex_unlock(&g_lock); respond(fd, 400, "{\"error\":\"full\"}"); return; }
    memset(s, 0, sizeof *s);
    pthread_cond_init(&s->cond, NULL);
    s->used = true;
    gen_id(s->id, ID_LEN); snprintf(s->owner, sizeof s->owner, "%s", u->user_id);
    // Seat 0 = creator. Identity lives here; the kernel state is dealt at start.
    Game *g = &s->game; g->num_players = 1; g->status = GAME_STATUS_WAITING;
    snprintf(s->seat_user[0], ID_LEN + 1, "%s", u->user_id);
    snprintf(s->seat_name[0], 24, "%s", u->username);
    snprintf(g->players[0].name, 24, "%s", u->username);
    snprintf(g->players[0].player_id, 24, "%s", u->user_id);
    g->players[0].status = PLAYER_STATUS_IDLE;
    g->players[0].strategy_key = STRATEGY_KEY_HUMAN;   // the creator is a human seat
    char out[80]; snprintf(out, sizeof out, "{\"game_id\":\"%s\"}", s->id);
    pthread_mutex_unlock(&g_lock);
    respond(fd, 200, out);
}

static void h_meta(Req *r, int fd) {
    char type[16] = {0}, gid[ID_LEN + 1] = {0};
    json_str(r->body, "type", type, sizeof type);
    json_str(r->body, "game_id", gid, sizeof gid);
    pthread_mutex_lock(&g_lock);
    User *u = user_by_token(r->token);
    GameSlot *s = game_by_id(gid);
    if (!u) { pthread_mutex_unlock(&g_lock); respond(fd, 401, "{\"error\":\"auth\"}"); return; }
    if (!s) { pthread_mutex_unlock(&g_lock); respond(fd, 404, "{\"error\":\"no game\"}"); return; }
    Game *g = &s->game;

    if (!strcmp(type, "join")) {
        if (seat_of(s, u->user_id) < 0 && g->num_players < MAX_PLAYERS && g->status == GAME_STATUS_WAITING) {
            int i = g->num_players++;
            snprintf(s->seat_user[i], ID_LEN + 1, "%s", u->user_id);
            snprintf(s->seat_name[i], 24, "%s", u->username);
            snprintf(g->players[i].name, 24, "%s", u->username);
            snprintf(g->players[i].player_id, 24, "%s", u->user_id);
            g->players[i].status = PLAYER_STATUS_IDLE;
            g->players[i].strategy_key = STRATEGY_KEY_HUMAN;   // a human seat
        }
    } else if (!strcmp(type, "add-bot")) {
        char skey[24] = {0}; if (!json_str(r->body, "strategy", skey, sizeof skey)) snprintf(skey, sizeof skey, "random");
        if (g->num_players < MAX_PLAYERS && g->status == GAME_STATUS_WAITING) {
            int i = g->num_players++;
            s->seat_ready[i] = true;
            int strat = bot_roster_find(skey);
            if (strat < 0) strat = bot_roster_find("random");
            snprintf(s->seat_name[i], 24, "%%%s %d", skey, i);
            snprintf(g->players[i].name, 24, "%s", s->seat_name[i]);
            snprintf(g->players[i].player_id, 24, "bot%d", i);
            g->players[i].status = PLAYER_STATUS_READY;
            g->players[i].strategy_key = (int8_t)strat;   // the kernel's own seat kind
        }
    } else if (!strcmp(type, "start")) {
        int me = seat_of(s, u->user_id);
        if (me >= 0) { s->seat_ready[me] = true; g->players[me].status = PLAYER_STATUS_READY; }
        // Deal once every seated human is ready (bots are always ready) and 2+ seated.
        bool all = g->num_players >= 2;
        for (int i = 0; i < g->num_players; i++) if (!seat_is_bot(g, i) && !s->seat_ready[i]) all = false;
        if (all && g->status == GAME_STATUS_WAITING) {
            unsigned char seed[32]; for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(rand() ^ (i * 131 + (int)g_seq));
            game_set_deal_seed_bytes(seed, 32);
            for (int i = 0; i < g->num_players; i++) g->players[i].status = PLAYER_STATUS_READY;
            start_game(g);                 // THE deal — kernel (sets g->status = PLAYING)
            start_bot_loop(s);             // the game-loop paces bot play from here
        }
    } else if (!strcmp(type, "continue")) {
        // Reset the kernel to the lobby for a rematch (identities kept, re-dealt
        // on start). The host owns only this lobby transition; game-over was the
        // kernel's to declare.
        g->status = GAME_STATUS_WAITING;
        for (int i = 0; i < g->num_players; i++) {
            bool ai = seat_is_bot(g, i);
            s->seat_ready[i] = ai;
            g->players[i].status = ai ? PLAYER_STATUS_READY : PLAYER_STATUS_IDLE;
        }
    }
    char out[80]; snprintf(out, sizeof out, "{\"game_id\":\"%s\",\"status\":%d}", s->id, g->status);
    pthread_mutex_unlock(&g_lock);
    respond(fd, 200, out);
}

static void h_action(Req *r, int fd) {
    // game_id rides the query string (like /state); the body IS the packed awire
    // frame, so it can be binary. /action?game_id=..
    char gid[ID_LEN + 1] = {0};
    const char *gp = strstr(r->query, "game_id=");
    if (gp) { gp += 8; int i = 0; while (gp[i] && gp[i] != '&' && i < ID_LEN) { gid[i] = gp[i]; i++; } gid[i] = 0; }
    pthread_mutex_lock(&g_lock);
    User *u = user_by_token(r->token);
    GameSlot *s = game_by_id(gid);
    if (!u) { pthread_mutex_unlock(&g_lock); respond(fd, 401, "{\"error\":\"auth\"}"); return; }
    if (!s || s->game.status != GAME_STATUS_PLAYING) { pthread_mutex_unlock(&g_lock); respond(fd, 400, "{\"error\":\"not playing\"}"); return; }
    int seat = seat_of(s, u->user_id);
    if (seat < 0) { pthread_mutex_unlock(&g_lock); respond(fd, 400, "{\"error\":\"not seated\"}"); return; }
    // The body is the same packed move the browser validates and the phone
    // sends: [kind, n, cards, (attacks)]. The kernel decodes and dispatches —
    // the server enumerates no move types (awire_apply owns the switch).
    AwireAction a;
    bool ok = r->body && r->body_len > 0
              && awire_decode((const unsigned char *)r->body, r->body_len, &a)
              && awire_apply(&s->game, seat, &a);
    // Human changed the board — wake the game-loop so bots respond (or notice
    // the game ended; awire_apply already settled g->status). If it's mid-sleep
    // it re-reads the state on its own.
    if (ok) pthread_cond_signal(&s->cond);
    char out[96]; snprintf(out, sizeof out, "{\"ok\":%s,\"status\":%d}", ok ? "true" : "false", s->game.status);
    pthread_mutex_unlock(&g_lock);
    respond(fd, ok ? 200 : 400, out);
}

static void h_state(Req *r, int fd) {
    char gid[ID_LEN + 1] = {0}; int seat = 0;
    // query: game_id=..&seat=..
    const char *gp = strstr(r->query, "game_id=");
    if (gp) { gp += 8; int i = 0; while (gp[i] && gp[i] != '&' && i < ID_LEN) { gid[i] = gp[i]; i++; } gid[i] = 0; }
    const char *sp = strstr(r->query, "seat="); if (sp) seat = (int)strtol(sp + 5, NULL, 10);
    pthread_mutex_lock(&g_lock);
    GameSlot *s = game_by_id(gid);
    if (!s) { pthread_mutex_unlock(&g_lock); respond(fd, 404, "{\"error\":\"no game\"}"); return; }
    // The kernel renders the masked, per-seat view as the PACKED wire (view.c
    // state_put) — no JSON. The client decodes it with its own kernel-wire
    // reader (Swift MaskedView / the web's TS reader). Kernel-to-kernel.
    static unsigned char buf[65536];
    int n = state_put(&s->game, seat, buf);
    pthread_mutex_unlock(&g_lock);
    respond_bin(fd, 200, buf, n);
}

// A plain status int (0 waiting / 1 playing / 2 over), for smoke tests that used
// to read it off the JSON view. Not json_out — just an integer.
static void h_status(Req *r, int fd) {
    char gid[ID_LEN + 1] = {0};
    const char *gp = strstr(r->query, "game_id=");
    if (gp) { gp += 8; int i = 0; while (gp[i] && gp[i] != '&' && i < ID_LEN) { gid[i] = gp[i]; i++; } gid[i] = 0; }
    pthread_mutex_lock(&g_lock);
    GameSlot *s = game_by_id(gid);
    int st = s ? s->game.status : -1;
    pthread_mutex_unlock(&g_lock);
    char out[16]; snprintf(out, sizeof out, "%d", st);
    respond(fd, 200, out);
}

// --------------------------------------------------------------------------
// Connection handling
// --------------------------------------------------------------------------

static void route(Req *r, int fd) {
    if (!strcmp(r->method, "OPTIONS")) { respond(fd, 200, "{}"); return; }
    if (!strcmp(r->path, "/health")) { respond(fd, 200, "{\"ok\":true}"); return; }
    if (!strcmp(r->path, "/auth/signup") || !strcmp(r->path, "/auth/signin")) { h_signup(r, fd); return; }
    if (!strcmp(r->path, "/create")) { h_create(r, fd); return; }
    if (!strcmp(r->path, "/meta"))   { h_meta(r, fd); return; }
    if (!strcmp(r->path, "/action")) { h_action(r, fd); return; }
    if (!strcmp(r->path, "/state"))  { h_state(r, fd); return; }
    if (!strcmp(r->path, "/status")) { h_status(r, fd); return; }
    respond(fd, 404, "{\"error\":\"route\"}");
}

static void *conn_thread(void *arg) {
    int fd = (int)(long)arg;
    static __thread char buf[1 << 16];
    int total = 0, n;
    // Read headers (+ body when Content-Length fits one read window).
    while ((n = read(fd, buf + total, sizeof buf - 1 - total)) > 0) {
        total += n; buf[total] = 0;
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (!hdr_end) continue;
        int want = 0; char *cl = strcasestr(buf, "content-length:");
        if (cl) want = (int)strtol(cl + 15, NULL, 10);
        int have = total - (int)(hdr_end + 4 - buf);
        if (have >= want) break;
    }
    Req r; memset(&r, 0, sizeof r);
    sscanf(buf, "%7s %255s", r.method, r.path);
    char *q = strchr(r.path, '?'); if (q) { *q = 0; snprintf(r.query, sizeof r.query, "%s", q + 1); }
    char *auth = strcasestr(buf, "authorization:");
    if (auth) sscanf(auth + 14, " Bearer %63s", r.token);
    char *body = strstr(buf, "\r\n\r\n"); r.body = body ? body + 4 : NULL;
    r.body_len = r.body ? total - (int)(r.body - buf) : 0;   // real byte count (body may be binary awire)
    route(&r, fd);
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8099;
    srand((unsigned)(time(NULL) ^ getpid()));

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    listen(srv, 64);
    fprintf(stderr, "foolish native server (kernel-driven, in-memory) on :%d\n", port);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, (void *)(long)fd) == 0) pthread_detach(t);
        else { close(fd); }
    }
}
