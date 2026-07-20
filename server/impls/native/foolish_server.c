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
//   GET  /ws?game_id=..&seat=..  (bearer, Upgrade: websocket)
//                                         -> RFC 6455 WebSocket. ONE persistent
//     connection per (authenticated, seated) client replaces the HTTP
//     action+state round trip for the hot loop: every binary frame the
//     client sends is applied as an awire move (or, if empty, treated as a
//     "give me current state" poll) and answered with one binary frame,
//     [ok:u8][state_put masked view bytes]. See ws.h/ws.c and the "T1b"
//     section of PROFILE_HOTPATH.md for why: thread-per-CONNECTION instead
//     of thread-per-REQUEST amortizes pthread_create's cost (a fresh
//     thread's zeroed stack/TLS was 85.8% of instructions under load,
//     T1) over a client's entire session instead of paying it per move.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strncasecmp — the hand-rolled header scan below
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "game.h"
#include "legal.h"
#include "view.h"
#include "awire.h"
#include "bot_drive.h"
#include "bot_roster.h"
#include "ws.h"

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

// --------------------------------------------------------------------------
// token -> User* / game_id -> GameSlot* : fixed-size open-addressing hash
// maps (PROFILE_HOTPATH.md T1 report item 2 — these two were O(MAX_USERS)/
// O(MAX_GAMES) linear strcmp scans on EVERY authenticated request, 1.08M
// strcmp calls in one 20s hammer run). Both g_users/g_games are fixed
// static arrays whose elements never move or get freed for the life of the
// process (a POC store — no delete), so raw pointers into them are safe to
// cache here forever; the table only ever grows (insert-only, sized well
// below 1.0 load factor for MAX_USERS/MAX_GAMES, so no growth/tombstone
// logic is needed). A stale slot (e.g. after h_signup mints a fresh token
// for an existing username, orphaning the old token's slot) is harmless: the
// final `strcmp` against the LIVE field still rejects it.
#define TOKEN_HT_SIZE 1024   // power of two, > 2x MAX_USERS
#define GAME_HT_SIZE   512   // power of two, > 2x MAX_GAMES

static User     *g_token_ht[TOKEN_HT_SIZE];
static GameSlot *g_game_ht[GAME_HT_SIZE];

static unsigned long hash_str(const char *s) {
    unsigned long h = 1469598103934665603UL;   // FNV-1a, 64-bit offset basis
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211UL; }
    return h;
}

static void token_ht_insert(User *u) {
    unsigned long h = hash_str(u->token) & (TOKEN_HT_SIZE - 1);
    while (g_token_ht[h]) h = (h + 1) & (TOKEN_HT_SIZE - 1);
    g_token_ht[h] = u;
}
static void game_ht_insert(GameSlot *s) {
    unsigned long h = hash_str(s->id) & (GAME_HT_SIZE - 1);
    while (g_game_ht[h]) h = (h + 1) & (GAME_HT_SIZE - 1);
    g_game_ht[h] = s;
}

static User *user_by_token(const char *token) {
    if (!token || !*token) return NULL;
    unsigned long h = hash_str(token) & (TOKEN_HT_SIZE - 1);
    for (int probes = 0; probes < TOKEN_HT_SIZE; probes++) {
        User *u = g_token_ht[h];
        if (!u) return NULL;   // insert-only table: an empty slot ends the probe chain
        if (u->used && strcmp(u->token, token) == 0) return u;
        h = (h + 1) & (TOKEN_HT_SIZE - 1);
    }
    return NULL;
}
static GameSlot *game_by_id(const char *id) {
    if (!id || !*id) return NULL;
    unsigned long h = hash_str(id) & (GAME_HT_SIZE - 1);
    for (int probes = 0; probes < GAME_HT_SIZE; probes++) {
        GameSlot *s = g_game_ht[h];
        if (!s) return NULL;
        if (s->used && strcmp(s->id, id) == 0) return s;
        h = (h + 1) & (GAME_HT_SIZE - 1);
    }
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

typedef struct {
    char method[8];
    char path[256];
    char query[256];
    char token[64];
    char *body; int body_len;
    int  content_length;    // parsed off Content-Length: while walking the headers
    bool is_ws_upgrade;     // GET with Upgrade: websocket + Connection: Upgrade + a key
    char ws_key[64];        // Sec-WebSocket-Key, verbatim (base64, ~24 chars)
} Req;

// Finds the blank line ending the header block ("\r\n\r\n") by a plain byte
// scan instead of strstr (PROFILE_HOTPATH.md T1 report item 3: strstr's
// two-way-search internals showed up as real cost even for this 4-byte
// literal). Returns a pointer just PAST the blank line, or NULL if not seen
// yet in the bytes read so far.
static char *find_headers_end(char *buf, int n) {
    for (int i = 0; i + 3 < n; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i + 4;
    return NULL;
}

// Hand-rolled request-line + header parser — replaces both
// `sscanf(buf, "%7s %255s", method, path)` (2.1% of all instructions under
// load, __vfscanf_internal) and the `strcasestr` header scan (content-length:
// + authorization:, another ~3% combined) with one single pass over the
// header block using strchr-style manual scanning and fixed-width
// `strncasecmp` on each line's already-located header name. Same semantics:
// method/path/query split on the request line, Content-Length, a Bearer
// token, and (new) whatever the WebSocket upgrade needs.
static void parse_request_line_and_headers(char *buf, char *hdr_end, Req *r) {
    memset(r, 0, sizeof *r);
    char *p = buf;

    char *s = p;
    while (p < hdr_end && *p != ' ' && *p != '\r' && *p != '\n') p++;
    int len = (int)(p - s); if (len > (int)sizeof r->method - 1) len = (int)sizeof r->method - 1;
    memcpy(r->method, s, (size_t)len); r->method[len] = 0;
    if (p < hdr_end && *p == ' ') p++;

    s = p;
    while (p < hdr_end && *p != ' ' && *p != '?' && *p != '\r' && *p != '\n') p++;
    len = (int)(p - s); if (len > (int)sizeof r->path - 1) len = (int)sizeof r->path - 1;
    memcpy(r->path, s, (size_t)len); r->path[len] = 0;
    if (p < hdr_end && *p == '?') {
        p++; s = p;
        while (p < hdr_end && *p != ' ' && *p != '\r' && *p != '\n') p++;
        len = (int)(p - s); if (len > (int)sizeof r->query - 1) len = (int)sizeof r->query - 1;
        memcpy(r->query, s, (size_t)len); r->query[len] = 0;
    }
    // Skip the rest of the request line (HTTP version) up to its CRLF.
    while (p < hdr_end && *p != '\n') p++;
    if (p < hdr_end) p++;

    bool saw_upgrade_websocket = false, saw_connection_upgrade = false;
    while (p < hdr_end) {
        char *line_start = p;
        char *colon = NULL;
        while (p < hdr_end && *p != '\r' && *p != '\n') { if (!colon && *p == ':') colon = p; p++; }
        char *line_end = p;
        if (p < hdr_end && *p == '\r') p++;
        if (p < hdr_end && *p == '\n') p++;
        if (!colon || line_start == colon) continue;   // blank/malformed line — skip

        int name_len = (int)(colon - line_start);
        const char *val = colon + 1;
        while (val < line_end && (*val == ' ' || *val == '\t')) val++;
        int val_len = (int)(line_end - val);

        if (name_len == 14 && strncasecmp(line_start, "content-length", 14) == 0) {
            int cl = 0;
            for (const char *q = val; q < line_end && *q >= '0' && *q <= '9'; q++) cl = cl * 10 + (*q - '0');
            r->content_length = cl;
        } else if (name_len == 13 && strncasecmp(line_start, "authorization", 13) == 0) {
            if (val_len > 7 && strncasecmp(val, "Bearer ", 7) == 0) {
                const char *tok = val + 7;
                int tlen = val_len - 7; if (tlen > (int)sizeof r->token - 1) tlen = (int)sizeof r->token - 1;
                memcpy(r->token, tok, (size_t)tlen); r->token[tlen] = 0;
            }
        } else if (name_len == 7 && strncasecmp(line_start, "upgrade", 7) == 0) {
            for (int i = 0; i + 9 <= val_len; i++)
                if (strncasecmp(val + i, "websocket", 9) == 0) { saw_upgrade_websocket = true; break; }
        } else if (name_len == 10 && strncasecmp(line_start, "connection", 10) == 0) {
            for (int i = 0; i + 7 <= val_len; i++)
                if (strncasecmp(val + i, "upgrade", 7) == 0) { saw_connection_upgrade = true; break; }
        } else if (name_len == 17 && strncasecmp(line_start, "sec-websocket-key", 17) == 0) {
            int klen = val_len; if (klen > (int)sizeof r->ws_key - 1) klen = (int)sizeof r->ws_key - 1;
            memcpy(r->ws_key, val, (size_t)klen); r->ws_key[klen] = 0;
        }
    }
    r->is_ws_upgrade = saw_upgrade_websocket && saw_connection_upgrade && r->ws_key[0];
}

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
    if (u) { gen_id(u->token, 32); token_ht_insert(u); }   // fresh session token, indexed
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
    gen_id(s->id, ID_LEN); game_ht_insert(s); snprintf(s->owner, sizeof s->owner, "%s", u->user_id);
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
            // Seats were wired in the lobby (strategy_key per seat), so pass NULL:
            // the kernel owns marking them + the deal (+ g->status = PLAYING).
            game_seat_and_deal(g, NULL, g->num_players);
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
    // Stack-local, not `static`: this handler runs on many connection
    // threads concurrently, and a shared buffer would let two /state
    // requests corrupt each other's bytes.
    unsigned char buf[65536];
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
// /ws — the WebSocket hot loop (item 1/2 of PROFILE_HOTPATH.md's "where to
// speed up" list). One handshake, then ONE persistent connection carries
// every move + state push for that seat's whole session — no pthread_create
// per action.
//
// Handshake: GET /ws?game_id=..&seat=.. (Bearer token), validated exactly
// like /action (must be a real user, a real game, and THIS user's own seat —
// no seat spoofing), then the RFC 6455 upgrade (ws_accept_from_key). After
// the 101 response, the server immediately PUSHES the current masked state
// once (ok=0 — nothing was applied yet, this is just "here's where things
// stand"), then loops:
//   client sends a binary frame -> [awire bytes] apply it, or [] (empty) to
//     just poll (a seat with no eligible move yet still needs to notice
//     when OTHER seats' actions make it eligible — see foolish_hammer.c's ws
//     worker) -> server always answers with one binary frame,
//     [ok:u8][state_put(seat) bytes], ok=1 iff a real move decoded AND the
//     kernel accepted it (awire_apply's own validation — same as /action).
// A human's turn wakes the bot game-loop exactly like /action did.
static void h_ws(Req *r, int fd) {
    char gid[ID_LEN + 1] = {0}; int seat = -1;
    const char *gp = strstr(r->query, "game_id=");
    if (gp) { gp += 8; int i = 0; while (gp[i] && gp[i] != '&' && i < ID_LEN) { gid[i] = gp[i]; i++; } gid[i] = 0; }
    const char *sp = strstr(r->query, "seat=");
    if (sp) seat = (int)strtol(sp + 5, NULL, 10);

    pthread_mutex_lock(&g_lock);
    User *u = user_by_token(r->token);
    GameSlot *s = game_by_id(gid);
    bool ok = u && s && seat >= 0 && seat < s->game.num_players && seat_of(s, u->user_id) == seat;
    pthread_mutex_unlock(&g_lock);
    if (!ok) { respond(fd, 401, "{\"error\":\"ws auth\"}"); return; }

    char accept[64];
    if (!ws_accept_from_key(r->ws_key, accept, sizeof accept)) { respond(fd, 400, "{\"error\":\"ws key\"}"); return; }
    char resp[256];
    int n = snprintf(resp, sizeof resp,
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (n <= 0 || ws_write_full(fd, resp, n) != n) return;

    WsConn wc; ws_conn_init(&wc, fd, /*mask_outgoing=*/0);   // server frames: never masked
    if (r->body_len > 0) ws_conn_prime(&wc, (const unsigned char *)r->body, r->body_len);

    // [ok:u8][state_put bytes] — sized for state_put's documented worst case
    // (h_state uses the same 65536 cap).
    unsigned char msg[1 + 65536];
    pthread_mutex_lock(&g_lock);
    int slen = s->used ? state_put(&s->game, seat, msg + 1) : 0;
    pthread_mutex_unlock(&g_lock);
    msg[0] = 0;
    if (ws_send_frame(&wc, WS_OP_BIN, msg, slen + 1) < 0) return;

    unsigned char in[4096];
    int opcode;
    int mlen;
    while ((mlen = ws_recv_message(&wc, in, sizeof in, &opcode)) >= 0) {
        if (opcode != WS_OP_BIN && opcode != WS_OP_TEXT) continue;
        bool applied = false;
        pthread_mutex_lock(&g_lock);
        if (s->used) {
            if (mlen > 0 && s->game.status == GAME_STATUS_PLAYING) {
                AwireAction a;
                if (awire_decode(in, mlen, &a) && awire_apply(&s->game, seat, &a)) {
                    applied = true;
                    pthread_cond_signal(&s->cond);   // same wakeup /action gives the bot game-loop
                }
            }
            slen = state_put(&s->game, seat, msg + 1);
        } else {
            slen = 0;
        }
        pthread_mutex_unlock(&g_lock);
        msg[0] = applied ? 1 : 0;
        if (ws_send_frame(&wc, WS_OP_BIN, msg, slen + 1) < 0) break;
    }
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
    char *hdr_end = NULL;
    // Read headers (+ body when Content-Length fits one read window). One
    // parse of the header block once it's fully in hand (item 3/4 of
    // PROFILE_HOTPATH.md: no per-iteration sscanf/strcasestr rescans).
    while ((n = read(fd, buf + total, sizeof buf - 1 - total)) > 0) {
        total += n; buf[total] = 0;
        hdr_end = find_headers_end(buf, total);
        if (!hdr_end) continue;
        break;
    }
    if (!hdr_end) { close(fd); return NULL; }   // malformed / empty request

    Req r;
    parse_request_line_and_headers(buf, hdr_end, &r);
    // Keep reading if the body (by Content-Length) hasn't fully arrived yet.
    int have = total - (int)(hdr_end - buf);
    while (have < r.content_length && (n = read(fd, buf + total, sizeof buf - 1 - total)) > 0) {
        total += n; buf[total] = 0;
        have = total - (int)(hdr_end - buf);
    }
    r.body = hdr_end;
    r.body_len = total - (int)(hdr_end - buf);   // real byte count (body may be binary awire)

    if (r.is_ws_upgrade && !strcmp(r.method, "GET") && !strcmp(r.path, "/ws")) {
        h_ws(&r, fd);   // owns the connection for its whole (long) life
        close(fd);
        return NULL;
    }
    route(&r, fd);
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8099;
    srand((unsigned)(time(NULL) ^ getpid()));
    // Long-lived /ws connections mean a peer can vanish (crash, network
    // reset, the load client's own `timeout` cutting it off) between our
    // read and our next write; the default SIGPIPE action is to kill the
    // WHOLE PROCESS on that write. Ignore it — write() already reports the
    // same failure as -1/EPIPE, which every write path here already checks.
    signal(SIGPIPE, SIG_IGN);

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
        // A one-shot HTTP request/response barely notices Nagle's algorithm,
        // but a persistent /ws connection does many small back-and-forth
        // writes+reads — without TCP_NODELAY, Nagle batching interacting
        // with the peer's delayed ACKs turns each round trip into tens of
        // milliseconds instead of tens of microseconds. The load client's
        // own connect_to() already sets this; the accept side never did.
        int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        pthread_t t;
        // Thread-per-CONNECTION (not per-request): for a /ws upgrade this
        // one thread now lives for the client's whole session, so the
        // pthread_create cost — 85.8% of instructions under load in the old
        // thread-per-HTTP-request T1 profile — is paid once per client
        // instead of once per action. See PROFILE_HOTPATH.md T1b.
        if (pthread_create(&t, NULL, conn_thread, (void *)(long)fd) == 0) pthread_detach(t);
        else { close(fd); }
    }
}
