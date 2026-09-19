// A /ws session - see wsconn.h.
#define _GNU_SOURCE
#include "wsconn.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "ctl_wire.h"
#include "play.h"
#include "session.h"
#include "srvthread.h"
#include "view.h"

GameSlot *ws_handshake_validate(Req *r, int *out_seat, bool *out_spectator,
                                int *out_cache_idx, int *out_viewer) {
    char gid[ID_LEN + 1] = {0}; int seat = -1;
    query_game_id(r->query, gid);
    const char *sp = strstr(r->query, "seat=");
    if (sp) seat = (int)strtol(sp + 5, NULL, 10);
    bool spectator = false;
    const char *spq = strstr(r->query, "spectator=");
    if (spq) { spq += 10; spectator = (*spq == '1'); }

    pthread_mutex_lock(&g_registry_lock);
    User *u = user_by_token(r->token);
    GameSlot *s = game_by_id(gid);
    if (!u || !s) { pthread_mutex_unlock(&g_registry_lock); return NULL; }
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);
    // Spectators: a real Bearer token is still required (same user_by_token
    // check every /ws client passes - this is "no SEAT membership", not "no
    // auth"), but ownership of a specific seat is not - any authenticated
    // user may watch any EXISTING game. Seated clients are exactly as
    // before: must own the seat they asked for.
    bool ok;
    if (spectator) {
        ok = true;
    } else {
        char user_id[ID_LEN + 1]; snprintf(user_id, sizeof user_id, "%s", u->user_id);
        ok = seat >= 0 && seat < s->game.num_players && seat_of(s, user_id) == seat;
    }
    // A connection attempt is activity: refresh last_active under s->lock so the
    // reaper can't reclaim this game in the tiny window between here and the
    // caller taking its own conn ref (see epoll_dispatch_ws / ws_conn_thread).
    s->last_active_us = now_us();
    pthread_mutex_unlock(&s->lock);
    if (!ok) return NULL;

    *out_seat = seat; *out_spectator = spectator;
    *out_cache_idx = spectator ? SPECTATOR_CACHE_IDX : seat;
    *out_viewer    = spectator ? VIEW_SPECTATOR : seat;
    return s;
}

bool ws_send_handshake_and_push(WsConn *out_wc, Conn conn, const char *accept,
                                GameSlot *s, int cache_idx, int viewer) {
    char resp[256];
    int n = snprintf(resp, sizeof resp,
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (n <= 0 || conn_write(&conn, resp, (size_t)n) != n) return false;

    ws_conn_init(out_wc, conn, /*mask_outgoing=*/0);
    // [ok:u8][state_put bytes] - sized for state_put's documented worst case
    // (h_state uses the same 65536 cap).
    unsigned char msg[1 + 65536];
    pthread_mutex_lock(&s->lock);
    int slen = s->used ? state_put_cached(s, cache_idx, viewer, msg + 1) : 0;
    pthread_mutex_unlock(&s->lock);
    msg[0] = 0;
    return ws_send_frame(out_wc, WS_OP_BIN, msg, slen + 1) >= 0;
}

void *ws_conn_thread(void *argp) {
    thread_disable_cancellation();
    WsSpawnArg *sa = argp;
    Conn *conn = &sa->conn;
    Req *r = &sa->req;

    int seat; bool spectator; int cache_idx, viewer;
    GameSlot *s = ws_handshake_validate(r, &seat, &spectator, &cache_idx, &viewer);
    if (!s) {
        respond_ctl_error(conn, 401, CTL_ERR_WS_AUTH);
        conn_close(conn); free(sa->raw_buf); free(sa); return NULL;
    }

    char accept[64];
    if (!ws_accept_from_key(r->ws_key, accept, sizeof accept)) {
        respond_ctl_error(conn, 400, CTL_ERR_WS_KEY);
        conn_close(conn); free(sa->raw_buf); free(sa); return NULL;
    }
    WsConn wc;
    if (!ws_send_handshake_and_push(&wc, *conn, accept, s, cache_idx, viewer)) {
        conn_close(conn); free(sa->raw_buf); free(sa); return NULL;
    }
    if (r->body_len > 0) ws_conn_prime(&wc, (const unsigned char *)r->body, r->body_len);
    free(sa->raw_buf); sa->raw_buf = NULL;   // primed into wc.pending - the raw request buffer is no longer referenced

    game_conn_ref(s);   // this thread references the slot for its whole session - reclamation-safe until unref below

    // Clear the accept-time slowloris deadline for this now-established session:
    // the loop below is a BLOCKING SSL_read, and a live WS may sit idle between
    // a player's moves far longer than g_io_timeout_s. (The plaintext /ws path
    // runs non-blocking under epoll, so SO_RCVTIMEO never applied there.)
    { struct timeval z = {0, 0};
      setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof z);
      setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, &z, sizeof z); }

    unsigned char in[4096];
    unsigned char msg[1 + 65536];
    int opcode;
    int mlen;
    while ((mlen = ws_recv_message(&wc, in, sizeof in, &opcode)) >= 0) {
        if (opcode != WS_OP_BIN && opcode != WS_OP_TEXT) continue;
        int mtotal = ws_service_message(s, seat, spectator, cache_idx, viewer, in, mlen, msg, NULL);
        if (ws_send_frame(&wc, WS_OP_BIN, msg, mtotal) < 0) break;
    }
    game_conn_unref(s);
    conn_close(&wc.conn);
    free(sa);
    return NULL;
}
