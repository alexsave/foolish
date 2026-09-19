// The action path - see play.h.
#define _GNU_SOURCE
#include "play.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "awire.h"
#include "ctl_wire.h"
#include "game.h"
#include "session.h"   // user_by_token: every seat check starts with "whose token is this"
#include "view.h"

atomic_ulong g_moves_applied = 0;

void query_game_id(const char *query, char *gid) {
    gid[0] = 0;
    const char *gp = strstr(query, "game_id=");
    if (!gp) return;
    gp += 8;
    int i = 0;
    while (gp[i] && gp[i] != '&' && i < ID_LEN) { gid[i] = gp[i]; i++; }
    gid[i] = 0;
}

// --------------------------------------------------------------------------
// The masked view, and its per-seat cache
// --------------------------------------------------------------------------

int state_put_cached_ptr(GameSlot *s, int cache_idx, int viewer,
                         const unsigned char **pp, unsigned char *fallback) {
    if (s->view_cache_version[cache_idx] != s->version) {
        // Serialize straight into the scratch buffer (wider than
        // VIEW_CACHE_CAP) first: state_put's real worst case fits well
        // inside VIEW_CACHE_CAP today (see that constant's comment), but if
        // a future kernel change ever grew a cap enough to overflow it,
        // this falls back to "always recompute, never cache" for that slot
        // instead of truncating a state update - correctness over the
        // optimization.
        int n = state_put(&s->game, viewer, fallback);
        if (n < 0) n = 0;
        if (n <= VIEW_CACHE_CAP) {
            memcpy(s->view_cache[cache_idx], fallback, (size_t)n);
            s->view_cache_len[cache_idx] = n;
            s->view_cache_version[cache_idx] = s->version;
        } else {
            *pp = fallback;
            return n;
        }
    }
    *pp = s->view_cache[cache_idx];
    return s->view_cache_len[cache_idx];
}

int state_put_cached(GameSlot *s, int cache_idx, int viewer, unsigned char *out) {
    unsigned char scratch[1 + 65536];
    const unsigned char *p = scratch;
    int n = state_put_cached_ptr(s, cache_idx, viewer, &p, scratch);
    memcpy(out, p, (size_t)n);
    return n;
}

int seat_view_for(const char *gid, int seat, const char *token, unsigned char *out) {
    if (seat < VIEW_SPECTATOR) return SEAT_VIEW_BAD_SEAT;
    pthread_mutex_lock(&g_registry_lock);
    GameSlot *s = game_by_id(gid);
    if (!s) { pthread_mutex_unlock(&g_registry_lock); return SEAT_VIEW_NO_GAME; }
    char user_id[ID_LEN + 1] = {0};
    if (seat >= 0) {
        User *u = user_by_token(token);
        if (!u) { pthread_mutex_unlock(&g_registry_lock); return SEAT_VIEW_UNAUTHORIZED; }
        snprintf(user_id, sizeof user_id, "%s", u->user_id);
    }
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);
    if (seat >= 0 && (seat >= s->game.num_players || seat_of(s, user_id) != seat)) {
        pthread_mutex_unlock(&s->lock);
        return SEAT_VIEW_NOT_YOURS;
    }
    const int n = state_put(&s->game, seat, out);
    pthread_mutex_unlock(&s->lock);
    return n;
}

// --------------------------------------------------------------------------
// Applying a move, whatever transport carried it
// --------------------------------------------------------------------------

bool ws_apply_move_locked(GameSlot *s, int seat, bool spectator,
                          const unsigned char *in, int mlen) {
    // Spectators MAY NOT submit moves (Stage 4): `!spectator` keeps ANY frame
    // a spectator sends - empty or a well-formed move alike - from ever
    // reaching awire_decode/awire_apply.
    if (!s->used || spectator || mlen <= 0 || s->game.status != GAME_STATUS_PLAYING) return false;
    AwireAction a;
    if (!awire_decode(in, mlen, &a)) return false;
    // No g_kernel_lock (Stage 5, see h_action's identical pattern and the
    // "Locking" doc in registry.h) - s->lock, held for this whole call, is enough.
    if (!awire_apply(&s->game, seat, &a)) return false;
    s->version++;   // this seat's move can change every seat's view
    atomic_fetch_add_explicit(&g_moves_applied, 1, memory_order_relaxed);   // /stats throughput gauge
    game_mark_dirty(s);
    pthread_cond_signal(&s->cond);   // same wakeup /action gives the bot game-loop
    return true;
}

int ws_service_message(GameSlot *s, int seat, bool spectator, int cache_idx, int viewer,
                       const unsigned char *in, int mlen, unsigned char *msg, uint32_t *out_version) {
    int slen;
    pthread_mutex_lock(&s->lock);
    bool applied = ws_apply_move_locked(s, seat, spectator, in, mlen);
    // PROFILE_HOTPATH.md "T1c": on a pure poll (mlen==0 or an illegal/rejected
    // move, or ANY frame from a spectator) this view did NOT change, so
    // state_put_cached memcpy's the bytes computed last time instead of
    // re-running the kernel's full masked serialization.
    slen = s->used ? state_put_cached(s, cache_idx, viewer, msg + 1) : 0;
    if (out_version) *out_version = s->version;
    pthread_mutex_unlock(&s->lock);
    msg[0] = applied ? 1 : 0;
    return slen + 1;
}

// --------------------------------------------------------------------------
// Route handlers
// --------------------------------------------------------------------------

void h_action(Req *r, Conn *conn) {
    // game_id rides the query string (like /state); the body IS the packed awire
    // frame, so it can be binary. /action?game_id=..
    char gid[ID_LEN + 1] = {0};
    query_game_id(r->query, gid);

    pthread_mutex_lock(&g_registry_lock);
    User *u = user_by_token(r->token);
    GameSlot *s = game_by_id(gid);
    if (!u) { pthread_mutex_unlock(&g_registry_lock); respond_ctl_error(conn, 401, CTL_ERR_AUTH); return; }
    if (!s) { pthread_mutex_unlock(&g_registry_lock); respond_ctl_error(conn, 400, CTL_ERR_NOT_PLAYING); return; }
    char user_id[ID_LEN + 1]; snprintf(user_id, sizeof user_id, "%s", u->user_id);
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);

    if (s->game.status != GAME_STATUS_PLAYING) { pthread_mutex_unlock(&s->lock); respond_ctl_error(conn, 400, CTL_ERR_NOT_PLAYING); return; }
    int seat = seat_of(s, user_id);
    if (seat < 0) { pthread_mutex_unlock(&s->lock); respond_ctl_error(conn, 400, CTL_ERR_NOT_SEATED); return; }

    // The body is the same packed move the browser validates and the phone
    // sends: [kind, n, cards, (attacks)]. The kernel decodes and dispatches -
    // the server enumerates no move types (awire_apply owns the switch).
    // awire_decode is stateless (no kernel globals - confirmed by inspection
    // of awire.c). No g_kernel_lock around awire_apply either (Stage 5, see
    // the "Locking" doc in registry.h): engine_last_reject and every other
    // kernel static handle_* touches are now _Thread_local, so s->lock (held
    // for this whole handler) is the only serialization awire_apply needs.
    AwireAction a;
    bool decoded = r->body && r->body_len > 0
                   && awire_decode((const unsigned char *)r->body, r->body_len, &a);
    bool ok = false;
    if (decoded) {
        ok = awire_apply(&s->game, seat, &a);
    }
    // Human changed the board - wake the game-loop so bots respond (or notice
    // the game ended; awire_apply already settled g->status). If it's mid-sleep
    // it re-reads the state on its own.
    if (ok) { s->version++; game_mark_dirty(s); pthread_cond_signal(&s->cond); }   // real move -> the cached per-seat views are stale
    unsigned char out[CTL_FRAME_MAX];
    int n = ctl_enc_applied(ok, s->game.status, out, (int)sizeof out);
    pthread_mutex_unlock(&s->lock);
    respond_ctl(conn, ok ? 200 : 400, out, n);
}

void h_state(Req *r, Conn *conn) {
    char gid[ID_LEN + 1] = {0}; int seat = VIEW_SPECTATOR;
    // query: game_id=..&seat=..
    query_game_id(r->query, gid);
    const char *sp = strstr(r->query, "seat="); if (sp) seat = (int)strtol(sp + 5, NULL, 10);
    // Never let an unauthenticated /state request reach state_put's trusted
    // VIEW_UNMASKED (-2) serialization, which emits every hand and the deck.
    // The only public views are VIEW_SPECTATOR (-1, all hands masked) and a
    // concrete seat (0..num_players-1); reject anything below spectator so the
    // seat= sentinel can't be spoofed into a full-state disclosure.
    // The kernel renders the masked, per-seat view as the PACKED wire (view.c
    // state_put). The client decodes it with its own kernel-wire reader (Swift
    // MaskedView / the web's TS reader). Kernel-to-kernel.
    // Stack-local, not `static`: this handler runs on many worker threads
    // concurrently, and a shared buffer would let two /state requests
    // corrupt each other's bytes.
    unsigned char buf[65536];
    const int n = seat_view_for(gid, seat, r->token, buf);
    if (n >= 0) { respond_bin(conn, 200, buf, n); return; }
    switch (n) {
        case SEAT_VIEW_BAD_SEAT:     respond_ctl_error(conn, 400, CTL_ERR_BAD_SEAT); return;
        case SEAT_VIEW_NO_GAME:      respond_ctl_error(conn, 404, CTL_ERR_NO_GAME); return;
        case SEAT_VIEW_UNAUTHORIZED: respond_ctl_error(conn, 401, CTL_ERR_UNAUTHORIZED); return;
        default:                     respond_ctl_error(conn, 403, CTL_ERR_NOT_YOUR_SEAT); return;
    }
}

// A plain status int (0 waiting / 1 playing / 2 over, or -1 for no such game),
// for smoke tests that used to read it off a JSON view. Just an integer, in a
// CTL_STATUS frame.
void h_status(Req *r, Conn *conn) {
    char gid[ID_LEN + 1] = {0};
    query_game_id(r->query, gid);

    pthread_mutex_lock(&g_registry_lock);
    GameSlot *s = game_by_id(gid);
    int st = -1;
    if (s) {
        pthread_mutex_lock(&s->lock);
        pthread_mutex_unlock(&g_registry_lock);
        st = s->game.status;
        pthread_mutex_unlock(&s->lock);
    } else {
        pthread_mutex_unlock(&g_registry_lock);
    }
    unsigned char out[CTL_FRAME_MAX];
    int n = ctl_enc_status(st, out, (int)sizeof out);
    respond_ctl(conn, 200, out, n);
}
