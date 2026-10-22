// lws.h - the gateway's state and the entries the three halves share: the routes (routes.c,
// request-pool threads: the upgrade and its MongoDB reads), the hub (hub.c, the loop thread:
// every socket after its upgrade, every Redis line, every timer) and the app (lws_app.c).
//
// THREADING. A connection is made on a pool thread with everything the upgrade resolved (the
// user, the game's player, the troll mark), handed to the loop through cb_ws_upgrade, and from
// on_open on it belongs to the loop thread alone. The hub holds no lock. The routes' caches
// (routes.c) are the only shared state, behind their own mutex.
#ifndef LWS_H
#define LWS_H

#include "ctx.h"
#include "net/ws.h"
#include "smap.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { K_SITE = 0, K_API, K_LOBBY, K_ROUND } lws_kind;

// An intrusive doubly-linked list node; a list is a sentinel node linked to itself.
typedef struct lnode { struct lnode *prev, *next; } lnode;
static inline void lnode_init(lnode *l) { l->prev = l->next = l; }
static inline bool lnode_linked(const lnode *n) { return n->next != NULL; }
static inline void lnode_push(lnode *l, lnode *n) { n->prev = l->prev; n->next = l; l->prev->next = n; l->prev = n; }
static inline void lnode_unlink(lnode *n) { if (!n->next) return; n->prev->next = n->next; n->next->prev = n->prev; n->next = n->prev = NULL; }
#define LNODE_OWNER(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
// Walks a list safely against the removal of the current node.
#define LNODE_EACH(l, it, nx) for (lnode *it = (l)->next, *nx = it->next; it != (l); it = nx, nx = it->next)

#define LWS_SRI_MAX 64
#define LWS_USER_MAX 32
#define LWS_ROOM_MAX 32
#define LWS_HIST 20
#define LWS_WATCH_MAX 16

typedef struct lws_room lws_room;

typedef struct lws_conn {
    cb_ws    *ws;
    lws_kind  kind;
    char      sri[LWS_SRI_MAX];
    char      user[LWS_USER_MAX];      // "" when anonymous
    char      name[LWS_USER_MAX + 8];  // the user's title and name as shown ("GM Name"), for crowds
    char      ip[64];
    char      flag[12];                // ?flag=: "", api, embed, simul, tournament
    bool      troll;
    // round
    char      room_id[LWS_ROOM_MAX];   // the game, set by the route; the room is made on the loop at on_open
    lws_room *room;
    bool      is_player;
    char      player_id[8];
    int       color;                   // 0 white, 1 black (players)
    char      ext_kind;                // 0, 't' tournament, 'w' swiss, 'm' simul (the game's tid, iid, sid)
    char      ext_id[16];
    bool      has_from;                // ?v= given
    int       from_version;
    // lobby
    bool      idle;
    // site state (ClientActor.State)
    int       last_ping_s;
    bool      mlat;
    lws_room *watched[LWS_WATCH_MAX];  // Fens: games this socket watches, oldest first
    int       n_watched;
    // the per-socket rate limit
    int       credits, max_credits, interval_ms;
    int64_t   clear_at_ms;
    bool      rl_logged;
    // the last crowd sent (round): the "d" object's bytes, to send only changes
    char     *last_crowd;
    // list memberships
    lnode     n_all, n_user, n_sri, n_room, n_kind, n_mlat, n_flag;
} lws_conn;

// One versioned event kept for resync (History): the full frame, and the mover's form (without
// dests) made when first needed.
typedef struct {
    int      v;
    char     type[16];
    uint8_t  f_watcher, f_owner, f_troll;
    int8_t   f_player;    // -1, 0 white, 1 black: for that color only
    int8_t   f_move_by;   // -1, 0, 1: the color that moved (gets the form without dests)
    char    *full;        // {"t":..,"v":..,"d":..} (round) or {"v":..,...} (room)
    size_t   full_len;
    char    *nodests;
    size_t   nodests_len;
} lws_event;

struct lws_room {
    char      id[LWS_ROOM_MAX];
    bool      round;             // a game: events are r/ver, crowds count players
    lnode     members;           // the sockets in it
    int       n_members;
    smap      users;             // user -> (uintptr_t) count, members with a user, players excluded
    int       anons;             // members without a user, players excluded
    int       players[2];        // players (and bots) online per color
    lws_event hist[LWS_HIST];    // a ring: hist_n events, the newest at (hist_head - 1)
    int       hist_n, hist_head;
    bool      crowd_dirty;       // in the hub's dirty list
    // Fens: who watches this game's position, and the last position seen
    lws_conn **fen_watchers;
    int       n_fen, cap_fen;
    bool      has_pos, pos_clock;
    char      pos_uci[8], pos_fen[96], pos_turn;
    int       pos_wc, pos_bc;
};

// The WebSocket handlers every socket gets: on_open registers the socket in the hub.
extern const cb_ws_handlers lws_ws_handlers;

// A socket's record, made by the route (pool thread) with what the upgrade resolved; the hub
// owns it from on_open. NULL on allocation failure.
lws_conn *lws_conn_new(lws_kind kind, const char *sri, const char *user, const char *ip, const char *flag, int credits, int interval_s);
void      lws_conn_free(lws_conn *c);

// The hub: made at serve_open on the loop, torn down at serve_close.
int  lws_hub_open(cb_ctx *ctx, const char *redis_host, int redis_port, char *err, size_t cap);
void lws_hub_close(void);

// Counters the smoke and the bench read through /lws/stats (lws_app.c).
typedef struct {
    long conns, users, rooms, rounds_crowded, events, redis_published, redis_received, dropped_offline;
    bool lila_online, redis_up;
} lws_stats;
void lws_hub_stats(lws_stats *out);   // loop thread

#endif
