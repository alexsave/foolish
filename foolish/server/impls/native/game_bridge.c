// --------------------------------------------------------------------------
// game_bridge.h implementation - the QUIC/WebTransport transport (quic_wt.c)
// reaches the shared game through these, taking the exact same registry and
// per-game locks the HTTP/WS paths do. So QUIC is just another front-end onto
// the one authoritative in-memory game; no game logic is duplicated here.
//
// Compiled only into the QUIC build (-DFOOLISH_QUIC, see the Makefile's
// foolish_server_quic target).
// --------------------------------------------------------------------------
#define _GNU_SOURCE
#include "game_bridge.h"

#include <pthread.h>
#include <stdio.h>

#include "play.h"
#include "quic_wt.h"
#include "registry.h"
#include "session.h"
#include "srvthread.h"

int gb_state_for(const char *game_id, int seat, const char *token, unsigned char *out, int cap) {
    if (cap < 65536) return -1;   // require state_put's documented worst-case room (same 65536 buffer h_state uses)
    const int n = seat_view_for(game_id, seat, token, out);   // HTTP /state's own rule, owner check included
    return n < 0 ? -1 : n;
}

int gb_apply_move(const char *game_id, const char *token, int seat,
                  const unsigned char *in, int len, unsigned char *out, int cap) {
    if (seat < 0) return -1;                                       // seated players only (spectators use gb_state_for)
    if (cap < 65536) return -1;                                    // room for state_put_cached's worst case (same as h_state)
    pthread_mutex_lock(&g_registry_lock);
    User *u = user_by_token(token);
    GameSlot *s = game_by_id(game_id);
    if (!u || !s) { pthread_mutex_unlock(&g_registry_lock); return -1; }
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);
    // Same ownership check GET /ws enforces: this token must actually hold this
    // seat in this game (see ws_handshake_validate).
    char user_id[ID_LEN + 1]; snprintf(user_id, sizeof user_id, "%s", u->user_id);
    if (!(seat < s->game.num_players && seat_of(s, user_id) == seat)) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    // An empty payload (len==0) is a pure "fetch my current view" - no move.
    if (in && len > 0) ws_apply_move_locked(s, seat, /*spectator=*/false, in, len);
    int n = state_put_cached(s, /*cache_idx=*/seat, /*viewer=*/seat, out);
    pthread_mutex_unlock(&s->lock);
    return n;
}

bool gb_game_ref(const char *game_id) {
    pthread_mutex_lock(&g_registry_lock);
    GameSlot *s = game_by_id(game_id);
    if (!s) { pthread_mutex_unlock(&g_registry_lock); return false; }
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);
    s->conn_refs++;                 // pin against reclamation for the WT session's lifetime (mirrors /ws bind)
    s->last_active_us = now_us();
    pthread_mutex_unlock(&s->lock);
    return true;
}

void gb_game_unref(const char *game_id) {
    pthread_mutex_lock(&g_registry_lock);
    GameSlot *s = game_by_id(game_id);
    if (!s) { pthread_mutex_unlock(&g_registry_lock); return; }
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);
    if (s->conn_refs > 0) s->conn_refs--;
    s->last_active_us = now_us();
    pthread_mutex_unlock(&s->lock);
}

// Thread wrapper: run the QUIC/HTTP3/WebTransport listener alongside the TCP
// acceptors, sharing this process's game state via the bridge above.
struct QuicArgs { int port; int workers; const char *cert; const char *key; };
static void *quic_thread_main(void *a) {
    thread_disable_cancellation();
    struct QuicArgs *qa = a;
    quic_wt_run(qa->port, qa->workers, qa->cert, qa->key);
    return NULL;
}

bool quic_bridge_start(int port, int workers, const char *cert, const char *key) {
    // static: the thread reads these for the whole process lifetime, long
    // after this call has returned.
    static struct QuicArgs qa;
    qa.port = port;
    qa.workers = workers;
    qa.cert = cert;
    qa.key  = key;
    pthread_t qt;
    if (pthread_create(&qt, NULL, quic_thread_main, &qa) != 0) return false;
    pthread_detach(qt);
    return true;
}
