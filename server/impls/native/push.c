// Turning a version bump into bytes on every watching connection - see push.h.
#define _GNU_SOURCE
#include "push.h"

#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>

#include "play.h"     // state_put_cached_ptr, ws_apply_move_locked
#include "shard.h"    // EConn / Worker, and the output-buffer + flush machinery
#include "ws.h"

void epoll_notify_game_changed(GameSlot *s) {
    if (!g_epoll_active) return;
    Worker *w = &g_workers[game_worker_index(s->id)];
    uint64_t one = 1;
    ssize_t wr = write(w->wake_evfd, &one, sizeof one);
    (void)wr;   // best-effort wake; eventfd only fails to accept a write at counter saturation (~2^63) - unreachable here
}

int econn_push_view(struct EConn *ecp, const unsigned char *apply_in, int apply_len,
                    bool only_if_stale, uint32_t *out_version, bool *out_applied) {
    static _Thread_local unsigned char fallback[1 + 65536];   // this worker's own thread only - never shared; used only on the never-hit overflow path
    EConn *ec = ecp;
    GameSlot *s = ec->slot;
    unsigned char ok = 0;
    int rc = 0;
    pthread_mutex_lock(&s->lock);
    if (apply_in) ok = ws_apply_move_locked(s, ec->seat, ec->spectator, apply_in, apply_len) ? 1 : 0;
    uint32_t v = s->version;
    bool stale = ec->last_pushed_version != v;
    if (!only_if_stale || stale) {
        const unsigned char *state = NULL;
        int slen = s->used ? state_put_cached_ptr(s, ec->cache_idx, ec->viewer, &state, fallback) : 0;
        if (econn_reserve_out(ec, slen + 1 + 14) &&
            ws_send_frame2(&ec->wc_out, WS_OP_BIN, &ok, 1, state, slen) >= 0) {
            econn_commit_out(ec);
            rc = 1;
        } else {
            rc = -1;
        }
    }
    if (out_version) *out_version = v;
    pthread_mutex_unlock(&s->lock);
    if (out_applied) *out_applied = (ok != 0);
    return rc;
}

void worker_push_stale(struct Worker *wp, GameSlot *game, struct EConn *skip) {
    Worker *w = wp;
    // `next` MUST be captured before econn_try_flush below: on a real write
    // error that call does econn_close(ec), which frees ec AND unlinks it from
    // ws_head. Advancing the loop with `ec = ec->next` after that is a
    // use-after-free (it reads the freed node's link) - and under load enough
    // connections error out at once that it corrupts the list and cascades into
    // a double-free / abort ("free(): invalid pointer"). econn_close only frees
    // ec itself and re-links its neighbors, so the `next` node captured up front
    // stays valid. (A plain `continue` does NOT avoid this - the for-loop's own
    // `ec = ec->next` increment still runs on the freed pointer.)
    EConn *next;
    for (EConn *ec = w->ws_head; ec; ec = next) {
        next = ec->next;
        if (ec == skip) continue;
        if (game && ec->slot != game) continue;
        uint32_t v;
        if (econn_push_view(ec, NULL, 0, /*only_if_stale=*/true, &v, NULL) == 1) {
            ec->last_pushed_version = v;
            if (!econn_try_flush(w, ec)) continue;   // ec is now freed; `next` was saved above
        }
    }
}
