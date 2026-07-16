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
#include "bot_roster.h"
#include "json_out.h"

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
    int  status;                    // GAME_STATUS_*
    Game game;                      // THE kernel state — the whole game
    // Lobby roster (identity lives beside the state blob, never in it — game.h).
    char owner[ID_LEN + 1];
    char seat_user[MAX_PLAYERS][ID_LEN + 1];  // "" for a bot
    char seat_name[MAX_PLAYERS][24];
    bool seat_is_ai[MAX_PLAYERS];
    int  seat_strategy[MAX_PLAYERS];
    bool seat_ready[MAX_PLAYERS];
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
static int seat_of(GameSlot *s, const char *user_id) {
    for (int i = 0; i < s->game.num_players; i++)
        if (!s->seat_is_ai[i] && strcmp(s->seat_user[i], user_id) == 0) return i;
    return -1;
}

// --------------------------------------------------------------------------
// Kernel-driven actions
// --------------------------------------------------------------------------

// Parse a move JSON ("type","cards":[{"s","v"}],"attackCards":[...]) and apply
// it through the kernel handler for `seat`. Returns true on a legal move.
static int parse_cards(const char *arr, Card *out, int max) {
    if (!arr) return 0;
    int n = 0; const char *p = arr;
    while (n < max && (p = strstr(p, "\"s\"")) != NULL) {
        const char *sc = strchr(p, ':'); if (!sc) break;
        int s = (int)strtol(sc + 1, NULL, 10);
        const char *vp = strstr(sc, "\"v\""); if (!vp) break;
        const char *vc = strchr(vp, ':'); if (!vc) break;
        int v = (int)strtol(vc + 1, NULL, 10);
        out[n].suit = (int8_t)s; out[n].value = (int8_t)v; n++;
        p = vc + 1;
    }
    return n;
}

static bool apply_move_json(Game *g, int seat, const char *move) {
    char type[16] = {0};
    if (!json_str(move, "type", type, sizeof type)) return false;
    // cards / attackCards arrays: locate their brackets, parse {s,v} entries.
    Card cards[MAX_MOVE_CARDS], atk[MAX_MOVE_CARDS];
    const char *ca = strstr(move, "\"cards\"");
    const char *aa = strstr(move, "\"attackCards\"");
    int nc = parse_cards(ca, cards, MAX_MOVE_CARDS);
    int na = parse_cards(aa, atk, MAX_MOVE_CARDS);
    if      (!strcmp(type, "attack")) return handle_attack(g, seat, cards, nc);
    else if (!strcmp(type, "cover"))  return (na == nc) && handle_cover(g, seat, cards, atk, nc);
    else if (!strcmp(type, "pass"))   return handle_pass(g, seat, cards, nc);
    else if (!strcmp(type, "pickup")) return handle_pickup(g, seat);
    else if (!strcmp(type, "good"))   return handle_good(g, seat);
    return false;
}

// Let every eligible bot act until none can (or the game ends). The kernel
// picks the move (bot_roster_choose); the loop is the only server-side part.
static void run_bots(GameSlot *s) {
    Game *g = &s->game;
    for (int guard = 0; guard < 512 && game_done(g) < 0; guard++) {
        bool acted = false;
        for (int seat = 0; seat < g->num_players; seat++) {
            if (!s->seat_is_ai[seat]) continue;
            LegalMoves moves;
            calculate_legal_moves(g, seat, &moves);
            if (moves.n == 0) continue;
            int idx = bot_roster_choose(s->seat_strategy[seat], g, seat, &moves);
            if (idx < 0 || idx >= moves.n) idx = 0;
            const LegalMove *m = &moves.moves[idx];
            switch (m->type) {
                case MOVE_ATTACK: handle_attack(g, seat, m->cards, m->n_cards); break;
                case MOVE_COVER:  handle_cover(g, seat, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   handle_pass(g, seat, m->cards, m->n_cards); break;
                case MOVE_PICKUP: handle_pickup(g, seat); break;
                case MOVE_GOOD:   handle_good(g, seat); break;
                default: break;
            }
            acted = true;
        }
        if (!acted) break;
    }
}

static void refresh_status(GameSlot *s) {
    if (s->status == GAME_STATUS_PLAYING && game_done(&s->game) >= 0)
        s->status = GAME_STATUS_GAME_OVER;
}

// --------------------------------------------------------------------------
// HTTP layer (hand-rolled; swap for mongoose in a real deployment)
// --------------------------------------------------------------------------

typedef struct { char method[8]; char path[256]; char query[256]; char token[64]; char *body; } Req;

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
    s->used = true; s->status = GAME_STATUS_WAITING;
    gen_id(s->id, ID_LEN); snprintf(s->owner, sizeof s->owner, "%s", u->user_id);
    // Seat 0 = creator. Identity lives here; the kernel state is dealt at start.
    Game *g = &s->game; g->num_players = 1; g->status = GAME_STATUS_WAITING;
    snprintf(s->seat_user[0], ID_LEN + 1, "%s", u->user_id);
    snprintf(s->seat_name[0], 24, "%s", u->username);
    snprintf(g->players[0].name, 24, "%s", u->username);
    snprintf(g->players[0].player_id, 24, "%s", u->user_id);
    g->players[0].status = PLAYER_STATUS_IDLE;
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
        if (seat_of(s, u->user_id) < 0 && g->num_players < MAX_PLAYERS && s->status == GAME_STATUS_WAITING) {
            int i = g->num_players++;
            snprintf(s->seat_user[i], ID_LEN + 1, "%s", u->user_id);
            snprintf(s->seat_name[i], 24, "%s", u->username);
            snprintf(g->players[i].name, 24, "%s", u->username);
            snprintf(g->players[i].player_id, 24, "%s", u->user_id);
            g->players[i].status = PLAYER_STATUS_IDLE;
        }
    } else if (!strcmp(type, "add-bot")) {
        char skey[24] = {0}; if (!json_str(r->body, "strategy", skey, sizeof skey)) snprintf(skey, sizeof skey, "random");
        if (g->num_players < MAX_PLAYERS && s->status == GAME_STATUS_WAITING) {
            int i = g->num_players++;
            s->seat_is_ai[i] = true; s->seat_ready[i] = true;
            s->seat_strategy[i] = bot_roster_find(skey);
            if (s->seat_strategy[i] < 0) s->seat_strategy[i] = bot_roster_find("random");
            snprintf(s->seat_name[i], 24, "%%%s %d", skey, i);
            snprintf(g->players[i].name, 24, "%s", s->seat_name[i]);
            snprintf(g->players[i].player_id, 24, "bot%d", i);
            g->players[i].status = PLAYER_STATUS_READY;
            g->players[i].strategy_key = (int8_t)s->seat_strategy[i];
        }
    } else if (!strcmp(type, "start")) {
        int me = seat_of(s, u->user_id);
        if (me >= 0) { s->seat_ready[me] = true; g->players[me].status = PLAYER_STATUS_READY; }
        // Deal once every seated human is ready (bots are always ready) and 2+ seated.
        bool all = g->num_players >= 2;
        for (int i = 0; i < g->num_players; i++) if (!s->seat_is_ai[i] && !s->seat_ready[i]) all = false;
        if (all && s->status == GAME_STATUS_WAITING) {
            unsigned char seed[32]; for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(rand() ^ (i * 131 + (int)g_seq));
            game_set_deal_seed_bytes(seed, 32);
            for (int i = 0; i < g->num_players; i++) g->players[i].status = PLAYER_STATUS_READY;
            start_game(g);                 // THE deal — kernel
            s->status = GAME_STATUS_PLAYING;
            run_bots(s); refresh_status(s); // bots may open the round
        }
    } else if (!strcmp(type, "continue")) {
        // Reset to the lobby for a rematch (identities kept, state re-dealt on start).
        s->status = GAME_STATUS_WAITING;
        for (int i = 0; i < g->num_players; i++) {
            s->seat_ready[i] = s->seat_is_ai[i];
            g->players[i].status = s->seat_is_ai[i] ? PLAYER_STATUS_READY : PLAYER_STATUS_IDLE;
        }
    }
    char out[80]; snprintf(out, sizeof out, "{\"game_id\":\"%s\",\"status\":%d}", s->id, s->status);
    pthread_mutex_unlock(&g_lock);
    respond(fd, 200, out);
}

static void h_action(Req *r, int fd) {
    char gid[ID_LEN + 1] = {0};
    json_str(r->body, "game_id", gid, sizeof gid);
    pthread_mutex_lock(&g_lock);
    User *u = user_by_token(r->token);
    GameSlot *s = game_by_id(gid);
    if (!u) { pthread_mutex_unlock(&g_lock); respond(fd, 401, "{\"error\":\"auth\"}"); return; }
    if (!s || s->status != GAME_STATUS_PLAYING) { pthread_mutex_unlock(&g_lock); respond(fd, 400, "{\"error\":\"not playing\"}"); return; }
    int seat = seat_of(s, u->user_id);
    if (seat < 0) { pthread_mutex_unlock(&g_lock); respond(fd, 400, "{\"error\":\"not seated\"}"); return; }
    const char *move = strstr(r->body ? r->body : "", "\"move\"");
    bool ok = apply_move_json(&s->game, seat, move ? move : "");
    if (ok) { run_bots(s); refresh_status(s); }
    char out[96]; snprintf(out, sizeof out, "{\"ok\":%s,\"status\":%d}", ok ? "true" : "false", s->status);
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
    static char buf[65536];
    // The kernel renders the masked, per-seat view — the same json_state_of the
    // iOS bridge uses. (Waiting lobbies decode too, since json_view fix.)
    int n = json_state_of(&s->game, seat, buf, sizeof buf);
    int status = s->status;
    pthread_mutex_unlock(&g_lock);
    if (n < 0) { respond(fd, 400, "{\"error\":\"view\"}"); return; }
    respond(fd, 200, buf); (void)status;
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
