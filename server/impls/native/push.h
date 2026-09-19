// push.h - how everyone watching a game hears that it changed.
//
// A game's state changes in four places (a human move over /action or /ws, a
// bot_drive cycle, a lobby transition in /meta) and every one of them bumps
// s->version under s->lock. This is the other half: turning that bump into
// bytes on every live /ws connection watching that game, without the client
// having to poll for it (PROFILE_HOTPATH.md "T1f", the push-only protocol).
//
// Two mechanisms, one per direction of the thread graph:
//   - epoll_notify_game_changed: a poke ACROSS threads (bot_thread, or a /meta
//     handler on a worker pool thread) at the epoll worker that owns the game.
//   - econn_push_view / worker_push_stale: the actual encode + fan-out, always
//     on the owning worker's own thread.
#ifndef FOOLISH_PUSH_H
#define FOOLISH_PUSH_H

#include <stdbool.h>
#include <stdint.h>

#include "registry.h"

// Named, not included: the connection and worker types are the transport's
// (shard.h), and the two callers that only need the cross-thread poke
// (bot_thread, h_meta) should not have to drag the whole epoll front-end in to
// get it.
struct EConn;
struct Worker;

// The one cross-thread seam (SERVER_SCALING.md "Stage 6"): a thread that is
// NOT the owning epoll worker - bot_thread (its own per-game trampoline
// thread), or a /meta handler on a work-queue thread - just mutated `s` and
// bumped s->version under s->lock, same as every other mutation site in this
// server. Under the OLD thread-per-connection /ws design that was enough by
// itself: every live connection was blocked in its OWN thread's
// ws_recv_message, so the NEXT client poll would simply see the fresh version.
// Under epoll, a connection sitting idle is NOT blocked in a read - it is just
// an fd registered in its worker's epoll set, and nothing else touches it until
// either the peer sends a frame or something pokes the worker. This function is
// that poke: a single eventfd write (no payload - it doesn't say WHICH game
// changed, since the scan this triggers, worker_push_stale, is cheap and only
// runs once per actual bot decision, not per idle poll) wakes the owning
// worker's epoll_wait, which then re-scans every /ws connection it owns for a
// stale cached view and pushes fresh state to each one that needs it. See
// worker_push_stale's doc for the two call sites (this one, and the fast
// per-request path used when a worker's own handling of a client's frame
// changed a game its OTHER connections are watching).
//
// A no-op whenever the server is running in --tls mode (which keeps the
// pre-Stage-6 thread-per-connection /ws design - see wsconn.h for why) - see
// the definition's `g_epoll_active` guard.
void epoll_notify_game_changed(GameSlot *s);

// Encode one binary WS frame [ok][state_put_cached(view)] for `ec` STRAIGHT
// into its output buffer under s->lock, copying the view bytes exactly once -
// from the per-seat cache into wbuf - instead of the old two-step (cache ->
// worker scratch in state_put_cached, then scratch -> wbuf in ws_send_frame).
// wbuf is owned by this single-threaded worker (game_worker_index's
// invariant), so appending to it while holding the GAME lock races nothing and
// needs no extra lock; the actual socket flush stays OUTSIDE the lock, in the
// caller. This is the two-copies-to-one push change (PROFILE_HOTPATH.md:
// memcpy ~18% on the epoll build).
//
//   apply_in != NULL  -> apply that client frame first (mover-reply path); the
//                        reply's ok byte is 1 iff the kernel accepted it.
//   only_if_stale     -> skip entirely unless ec->last_pushed_version differs
//                        from the (post-apply) version (fan-out path).
// Returns 1 if a frame was encoded+committed into wbuf (caller should flush),
// 0 if nothing was queued (only_if_stale and not stale), or -1 if the out
// buffer had no room (caller leaves ec untouched). *out_version (may be NULL)
// gets s->version as observed under the lock; *out_applied (may be NULL) gets
// whether apply_in was accepted.
int econn_push_view(struct EConn *ec, const unsigned char *apply_in, int apply_len,
                    bool only_if_stale, uint32_t *out_version, bool *out_applied);

// Pushes fresh state to every active /ws connection THIS worker owns whose
// cached view is stale (ec->last_pushed_version != its game's current
// s->version), skipping `skip` and, when `game` is non-NULL, every OTHER
// game too. TWO call sites:
//   1. The eventfd-triggered path, woken by epoll_notify_game_changed from
//      bot_thread (or h_meta - a lobby transition) on a DIFFERENT thread -
//      since the eventfd carries no payload (which game changed), this is
//      called with game=NULL, skip=NULL and checks every /ws connection
//      this worker owns.
//   2. PROFILE_HOTPATH.md "T1f" (push-only protocol): handle_ws_readable,
//      right after a human move applies, calls this INLINE with
//      game=ec->slot, skip=ec - fanning the new state out to that game's
//      OTHER live connections (the mover already got its own direct reply).
//      No cross-thread wakeup needed here: this runs on the SAME worker
//      thread that already owns every /ws connection for this game (see
//      game_worker_index's doc), so it's just a normal function call, not
//      an eventfd round trip.
// Bounded by how many connections one shard holds, and - thanks to the
// version-stale check above - naturally coalesces: several moves landing in
// quick succession before a given connection is next scanned still cost it
// only ONE push (the latest cached view), not one per move.
//
// History: an earlier revision of this server scoped call site 2 OUT
// (bot-thread pushes only) because the load client of the time POLLED -
// every idle seat sent an empty frame every ~1ms regardless - so fanning
// out on every human move on TOP of that made every OTHER seat's client
// immediately re-check eligibility and often re-submit, racing the others
// for the same now-stale window and measurably hurting the applied ratio
// (SERVER_SCALING.md "Stage 6"). That was a load-tool artifact, not a
// reason to withhold the push: a real client updates its UI and waits for
// its human, it doesn't auto-resubmit. T1f made the reference client
// (`foolish_hammer --mode=ws`) genuinely push-driven - it submits at most
// one legal move per pushed state and otherwise just waits - which removes
// the herd motive; see PROFILE_HOTPATH.md "T1f" for the measured ratio.
// WithOUT this call site, a push-only (non-polling) client would have no
// way to learn about another seat's move at all: it would just sit blocked
// in ws_recv_message forever, stalling the whole game.
void worker_push_stale(struct Worker *w, GameSlot *game, struct EConn *skip);

#endif
