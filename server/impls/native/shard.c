// epoll-per-shard connection I/O - see shard.h for the whole Stage 6 design,
// its scope, and the threading contract every function here relies on.
#define _GNU_SOURCE
#include "shard.h"

#include <errno.h>
#include <fcntl.h>       // Stage 6: O_NONBLOCK
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>   // Stage 6: epoll-per-shard
#include <sys/eventfd.h> // Stage 6: cross-thread worker wakeup (acceptor handoff + bot_thread push)
#include <unistd.h>

#include "ctl_wire.h"
#include "play.h"
#include "push.h"
#include "srvthread.h"
#include "wsconn.h"

int g_n_game_workers = N_GAME_WORKERS_DEFAULT;
Worker g_workers[MAX_GAME_WORKERS];
bool g_epoll_active = false;

int game_worker_index(const char *game_id) {
    unsigned long h = (game_id && game_id[0]) ? hash_str(game_id) : 0;
    return (int)(h % (unsigned long)g_n_game_workers);
}

// --------------------------------------------------------------------------
// wsasync_feed - incremental, resumable RFC 6455 frame decode.
//
// Mirrors ws_recv_message's (ws.c) exact per-field logic and control-frame/
// fragmentation semantics - see that function's own comment for the wire
// format this replicates - but consumes bytes fed in from a non-blocking
// read() instead of blocking on conn_read/ws_fill, resuming exactly where
// the previous call left off (all resumable state lives in `ec`). PING is
// answered with PONG inline (queued into ec->wbuf via ec->wc_out, same as
// ws_recv_message's own `ws_send_frame(c, WS_OP_PONG, ...); continue;`);
// PONG is swallowed; CLOSE gets a best-effort echo (matching
// ws_send_close(c, 1000)) and ends the connection - none of the three ever
// surface to the caller as a "message".
//
// Returns:
//   WSF_MESSAGE   - one complete data message (TEXT or BIN, fully
//                   reassembled across any CONT fragments) is ready:
//                   ec->msg_buf[0..ec->last_msg_len) holds it. *consumed
//                   says how many of `in`'s `n` bytes this call used - there
//                   may be bytes for the START of the NEXT frame left over
//                   in `in[*consumed..n)`; the caller must re-feed those
//                   (after handling this message) before reading the socket
//                   again.
//   WSF_NEED_MORE - no complete message yet; *consumed == n (every input
//                   byte was used). Caller should wait for more EPOLLIN.
//   WSF_ERROR     - protocol violation, an oversized message (> WS_IN_CAP),
//                   or a CLOSE frame. Caller must tear the connection down;
//                   *consumed is NOT reliably set on every WSF_ERROR return
//                   path (every caller ignores it in this case - the
//                   connection is going away regardless).
static int wsasync_feed(EConn *ec, const unsigned char *in, int n, int *consumed) {
    int off = 0;
    for (;;) {
        switch (ec->phase) {
        case WSP_HDR2: {
            int take = 2 - ec->hdr_have; if (take > n - off) take = n - off;
            if (take > 0) { memcpy(ec->hdrbuf + ec->hdr_have, in + off, (size_t)take); ec->hdr_have += take; off += take; }
            if (ec->hdr_have < 2) { *consumed = off; return WSF_NEED_MORE; }
            ec->fin    = (ec->hdrbuf[0] >> 7) & 1;
            ec->op     = ec->hdrbuf[0] & 0x0F;
            ec->masked = (ec->hdrbuf[1] >> 7) & 1;
            int64_t len7 = ec->hdrbuf[1] & 0x7F;
            ec->hdr_have = 0;
            ec->frame_validated = false;   // a brand-new frame starts here - see WSP_PAYLOAD's doc
            if (len7 == 126)      { ec->hdr_need = 2; ec->phase = WSP_EXTLEN; }
            else if (len7 == 127) { ec->hdr_need = 8; ec->phase = WSP_EXTLEN; }
            else { ec->frame_len = len7; ec->phase = ec->masked ? WSP_MASK : WSP_PAYLOAD; }
            continue;
        }
        case WSP_EXTLEN: {
            int take = ec->hdr_need - ec->hdr_have; if (take > n - off) take = n - off;
            if (take > 0) { memcpy(ec->hdrbuf + ec->hdr_have, in + off, (size_t)take); ec->hdr_have += take; off += take; }
            if (ec->hdr_have < ec->hdr_need) { *consumed = off; return WSF_NEED_MORE; }
            uint64_t len = 0;
            for (int i = 0; i < ec->hdr_need; i++) len = (len << 8) | ec->hdrbuf[i];   // unsigned: a hostile 8-byte length must not overflow (UB) mid-assembly
            // A single frame can never exceed this connection's fixed buffer;
            // rejecting here also covers the RFC 6455 5.2 "MSB must be 0" rule
            // and any absurd/negative value the 8-byte form could encode.
            if (len > (uint64_t)WS_IN_CAP) return WSF_ERROR;
            ec->frame_len = (int64_t)len;
            ec->hdr_have = 0;
            ec->phase = ec->masked ? WSP_MASK : WSP_PAYLOAD;
            continue;
        }
        case WSP_MASK: {
            int take = 4 - ec->hdr_have; if (take > n - off) take = n - off;
            if (take > 0) { memcpy(ec->mkey + ec->hdr_have, in + off, (size_t)take); ec->hdr_have += take; off += take; }
            if (ec->hdr_have < 4) { *consumed = off; return WSF_NEED_MORE; }
            ec->hdr_have = 0;
            ec->phase = WSP_PAYLOAD;
            continue;
        }
        case WSP_PAYLOAD: {
            bool ctrl = (ec->op == WS_OP_PING || ec->op == WS_OP_PONG || ec->op == WS_OP_CLOSE);
            // Frame-header validation (new-vs-continuation bookkeeping, the
            // oversized check) MUST run EXACTLY ONCE per frame. wsasync_feed
            // can be called many times while a single frame's payload
            // trickles in across several non-blocking reads (WSF_NEED_MORE
            // below, resumed on the next EPOLLIN) - including calls that
            // land here with ZERO payload bytes yet available (the header
            // and mask arrived, but the payload hasn't started at all) - so
            // neither "first call" nor "payload_got == 0" reliably means
            // "not yet validated" (a real, caught-live bug: a header+mask
            // that fully arrives with 0 payload bytes in the SAME read, then
            // the payload trickles in on a LATER read, hits this case again
            // with payload_got still 0 - see SERVER_SCALING.md "Stage 6").
            // `ec->frame_validated` is the actual per-FRAME signal: reset to
            // false exactly once, when WSP_HDR2 starts parsing this frame's
            // 2-byte header, and set true here the first time validation
            // actually runs for it - correct regardless of how the
            // header/mask/payload bytes happen to split across reads.
            if (!ec->frame_validated) {
                ec->frame_validated = true;
                if (ctrl && ec->frame_len > 125) return WSF_ERROR;   // control frames are never fragmented/oversized (RFC 6455 5.5)
                if (!ctrl) {
                    if (ec->op != WS_OP_CONT && ec->op != WS_OP_TEXT && ec->op != WS_OP_BIN) return WSF_ERROR;
                    if (ec->op != WS_OP_CONT) {
                        if (ec->msg_opcode != -1) return WSF_ERROR;   // a new message started before the last one finished
                        ec->msg_opcode = ec->op;
                    } else if (ec->msg_opcode == -1) {
                        return WSF_ERROR;   // continuation with nothing to continue
                    }
                    if (ec->msg_total + ec->frame_len > (int64_t)WS_IN_CAP) return WSF_ERROR;   // oversized for this buffer
                }
            }
            unsigned char *dst = ctrl ? ec->ctrl_buf : ec->msg_buf + ec->msg_total;
            int take = (int)(ec->frame_len - ec->payload_got); if (take > n - off) take = n - off;
            if (take > 0) { memcpy(dst + ec->payload_got, in + off, (size_t)take); ec->payload_got += take; off += take; }
            if (ec->payload_got < ec->frame_len) { *consumed = off; return WSF_NEED_MORE; }

            if (ec->masked) for (int64_t i = 0; i < ec->frame_len; i++) dst[i] ^= ec->mkey[i & 3];
            int op = ec->op; int64_t flen = ec->frame_len;
            ec->payload_got = 0; ec->hdr_have = 0; ec->phase = WSP_HDR2;   // ready for the next frame either way

            if (ctrl) {
                if (op == WS_OP_PING) {
                    if (econn_reserve_out(ec, (int)flen + 14)) {
                        ws_send_frame(&ec->wc_out, WS_OP_PONG, ec->ctrl_buf, flen);
                        econn_commit_out(ec);
                    }
                    continue;   // more frames may remain in this same chunk
                }
                if (op == WS_OP_PONG) continue;
                // CLOSE: best-effort echo (matches ws_send_close(c, 1000)), then tear down.
                if (econn_reserve_out(ec, 16)) {
                    unsigned char payload[2] = { 0x03, 0xE8 };   // 1000, network byte order
                    ws_send_frame(&ec->wc_out, WS_OP_CLOSE, payload, 2);
                    econn_commit_out(ec);
                }
                *consumed = off;
                return WSF_ERROR;
            }
            ec->msg_total += (int)flen;
            if (ec->fin) {
                *consumed = off;
                ec->last_msg_len = ec->msg_total;
                ec->msg_total = 0; ec->msg_opcode = -1;
                return WSF_MESSAGE;
            }
            continue;   // more fragments to come
        }
        }
    }
}

// --------------------------------------------------------------------------
// EConn output buffering + epoll bookkeeping - all single-threaded per
// worker (see "Threading" in shard.h), so none of this needs a lock.
// --------------------------------------------------------------------------

bool econn_reserve_out(EConn *ec, int need) {
    if (ec->woff > 0) {
        int rem = ec->wlen - ec->woff;
        if (rem > 0) memmove(ec->wbuf, ec->wbuf + ec->woff, (size_t)rem);
        ec->wlen = rem; ec->woff = 0;
    }
    if (ec->wlen + need > ec->wbuf_cap) return false;
    Conn buffered; conn_init_buffered(&buffered, ec->wbuf, ec->wbuf_cap);
    buffered.buf_len = ec->wlen;   // resume appending after whatever's already queued
    ws_conn_init(&ec->wc_out, buffered, /*mask_outgoing=*/0);   // server frames are never masked
    return true;
}
void econn_commit_out(EConn *ec) { ec->wlen = ec->wc_out.conn.buf_len; }

static void econn_set_epollout(Worker *w, EConn *ec, bool want) {
    if (want == ec->want_epollout) return;
    ec->want_epollout = want;
    uint32_t events = (uint32_t)((ec->kind == ECONN_WS ? EPOLLIN : 0) | (want ? EPOLLOUT : 0));
    struct epoll_event ev = { .events = events, .data.ptr = ec };
    epoll_ctl(w->epfd, EPOLL_CTL_MOD, ec->fd, &ev);
}

void econn_close(Worker *w, EConn *ec) {
    if (ec->closed) return;
    ec->closed = true;
    atomic_fetch_sub_explicit(&g_live_conns, 1, memory_order_relaxed);   // pairs with worker_handoff_push's admit
    epoll_ctl(w->epfd, EPOLL_CTL_DEL, ec->fd, NULL);
    close(ec->fd);
    if (ec->kind == ECONN_WS) {
        if (ec->prev) ec->prev->next = ec->next; else if (w->ws_head == ec) w->ws_head = ec->next;
        if (ec->next) ec->next->prev = ec->prev;
        if (ec->slot) game_conn_unref(ec->slot);   // release the reclamation ref taken at bind (epoll_dispatch_ws)
    }
    // Reuse `next` as the dead-list link - ec is now off ws_head, and any live
    // ws_head iterator (worker_push_stale) captured its next pointer before the
    // close, so overwriting it here is safe.
    ec->next = w->dead_head;
    w->dead_head = ec;
}

// Frees every connection econn_close parked this batch. Called once at the end
// of each epoll loop iteration, when no events[] entry can reference them.
static void econn_drain_dead(Worker *w) {
    EConn *d = w->dead_head;
    w->dead_head = NULL;
    while (d) {
        EConn *nx = d->next;
        free(d->wbuf);
        free(d);
        d = nx;
    }
}

bool econn_try_flush(Worker *w, EConn *ec) {
    while (ec->woff < ec->wlen) {
        ssize_t wr = write(ec->fd, ec->wbuf + ec->woff, (size_t)(ec->wlen - ec->woff));
        if (wr < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { econn_set_epollout(w, ec, true); return true; }
            if (errno == EINTR) continue;
            econn_close(w, ec); return false;
        }
        if (wr == 0) { econn_close(w, ec); return false; }
        ec->woff += (int)wr;
    }
    ec->woff = ec->wlen = 0;
    econn_set_epollout(w, ec, false);
    if (ec->kind == ECONN_ONE_SHOT && ec->close_after_flush) { econn_close(w, ec); return false; }
    return true;
}

// Drains fds handed off by the acceptors (epoll_dispatch_ws/
// epoll_dispatch_oneshot) into this worker's own epoll set. Each arrives
// with its FIRST reply/handshake already encoded into wbuf (the acceptor
// built it via the buffered-Conn trick before handing the fd over) - this
// just registers it for EPOLLOUT (to flush that) and, for a /ws connection,
// EPOLLIN too (to read whatever the client sends next) and links it into
// ws_head.
static void drain_handoff_queue(Worker *w) {
    pthread_mutex_lock(&w->handoff_mtx);
    EConn *head = w->handoff_head;
    w->handoff_head = w->handoff_tail = NULL;
    pthread_mutex_unlock(&w->handoff_mtx);
    while (head) {
        EConn *ec = head; head = head->next; ec->next = NULL;
        if (ec->wlen <= ec->woff) { econn_close(w, ec); continue; }   // nothing to send at all - shouldn't happen, defensive
        uint32_t events = (uint32_t)((ec->kind == ECONN_WS ? EPOLLIN : 0) | EPOLLOUT);
        ec->want_epollout = true;
        struct epoll_event ev = { .events = events, .data.ptr = ec };
        if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, ec->fd, &ev) < 0) { atomic_fetch_sub_explicit(&g_live_conns, 1, memory_order_relaxed); close(ec->fd); free(ec->wbuf); free(ec); continue; }
        if (ec->kind == ECONN_WS) {
            ec->prev = NULL; ec->next = w->ws_head;
            if (w->ws_head) w->ws_head->prev = ec;
            w->ws_head = ec;
        }
    }
}

// EPOLLIN on a live /ws connection: drain what's available non-blockingly,
// feed it through wsasync_feed, and for each complete message, apply it and
// reply.
static void handle_ws_readable(Worker *w, EConn *ec) {
    for (;;) {
        unsigned char tmp[8192];
        ssize_t r = read(ec->fd, tmp, sizeof tmp);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            econn_close(w, ec); return;
        }
        if (r == 0) { econn_close(w, ec); return; }   // peer closed
        int off = 0;
        while (off < (int)r) {
            int consumed = 0;
            int rc = wsasync_feed(ec, tmp + off, (int)r - off, &consumed);
            off += consumed;
            if (rc == WSF_MESSAGE) {
                // Apply the move (if any) and encode the [ok][state] reply
                // straight into wbuf under one lock acquisition - the same
                // service ws_conn_thread does via ws_service_message, but the
                // buffered (epoll) sink lets econn_push_view land the state
                // bytes in wbuf with a single copy (see its doc).
                uint32_t v; bool applied = false;
                if (econn_push_view(ec, ec->msg_buf, ec->last_msg_len, /*only_if_stale=*/false, &v, &applied) == 1) {
                    ec->last_pushed_version = v;
                }
                if (!econn_try_flush(w, ec)) return;   // closed (real write error) - ec is gone
                // PROFILE_HOTPATH.md "T1f" (push-only protocol): fan the new
                // state out to this game's OTHER live /ws connections - the
                // exact same cached-view push worker_push_stale already
                // gives the bot_thread/eventfd path (see its doc), just
                // called straight from THIS worker's own thread instead of
                // via an eventfd wakeup, since this worker already owns
                // every connection for `ec->slot` (game_worker_index's
                // invariant). Gated on `applied`: a rejected move, a
                // spectator's frame, or (pre-T1f-client) a plain poll never
                // changed s->version, so worker_push_stale's own stale-check
                // would no-op every OTHER connection anyway - skipping the
                // call entirely on those avoids even the per-connection lock
                // scan. `skip=ec`: the mover already has its direct reply
                // above; it must not also receive a redundant fan-out frame
                // for the very same version.
                if (applied) worker_push_stale(w, ec->slot, ec);
            } else if (rc == WSF_ERROR) {
                // Best-effort: send any queued CLOSE-echo bytes before
                // tearing down. econn_try_flush's OWN return tells us
                // whether it already closed `ec` (a real write error) -
                // NEVER re-derive that by re-reading `ec` afterward, which
                // would be a use-after-free the moment it already freed it.
                if (econn_try_flush(w, ec)) econn_close(w, ec);
                return;
            } else {
                break;   // WSF_NEED_MORE
            }
        }
        if ((size_t)r < sizeof tmp) return;   // short read: likely drained for now - next EPOLLIN will confirm
    }
}

static void *epoll_worker_main(void *arg) {
    thread_disable_cancellation();
    Worker *w = arg;
    struct epoll_event events[EPOLL_MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(w->epfd, events, EPOLL_MAX_EVENTS, -1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        bool woke = false;
        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {   // the wake_evfd itself - see epoll_notify_game_changed / worker_handoff_push
                uint64_t v; ssize_t rd = read(w->wake_evfd, &v, sizeof v); (void)rd;
                woke = true;
                continue;
            }
            EConn *ec = events[i].data.ptr;
            if (ec->closed) continue;   // a handler earlier in THIS batch already closed it (deferred-freed) - its event is stale, skip it
            uint32_t evb = events[i].events;
            if (evb & (EPOLLHUP | EPOLLERR)) { econn_close(w, ec); continue; }
            if (evb & EPOLLOUT) { if (!econn_try_flush(w, ec)) continue; }
            if (evb & EPOLLIN) { if (ec->kind == ECONN_WS) handle_ws_readable(w, ec); }
        }
        if (woke) {
            drain_handoff_queue(w);
            worker_push_stale(w, NULL, NULL);   // don't know which game(s) a bot_thread notification was for - check them all
        }
        econn_drain_dead(w);   // free everything closed during this batch, now that no events[] entry can still reference it
    }
    return NULL;
}

// Pushes a freshly-prepared EConn (its first reply/handshake already
// encoded into wbuf) onto `w`'s handoff queue and wakes it - the ONE way an
// fd crosses from an acceptor thread to its owning worker thread.
static void worker_handoff_push(Worker *w, EConn *ec) {
    atomic_fetch_add_explicit(&g_live_conns, 1, memory_order_relaxed);   // admitted: every epoll connection passes through here exactly once
    pthread_mutex_lock(&w->handoff_mtx);
    ec->next = NULL;
    if (w->handoff_tail) w->handoff_tail->next = ec; else w->handoff_head = ec;
    w->handoff_tail = ec;
    pthread_mutex_unlock(&w->handoff_mtx);
    uint64_t one = 1;
    ssize_t wr = write(w->wake_evfd, &one, sizeof one);
    (void)wr;
}

void epoll_dispatch_oneshot(Conn *conn, Req *r, char *raw_buf) {
    EConn *ec = calloc(1, sizeof *ec);
    if (!ec) { conn_close(conn); free(raw_buf); return; }
    ec->kind = ECONN_ONE_SHOT;
    ec->fd = conn->fd;
    ec->wbuf_cap = ONESHOT_WBUF_CAP;
    ec->wbuf = malloc((size_t)ec->wbuf_cap);
    if (!ec->wbuf) { free(ec); conn_close(conn); free(raw_buf); return; }

    Conn buffered; conn_init_buffered(&buffered, ec->wbuf, ec->wbuf_cap);
    route(r, &buffered);   // h_action/h_state/h_status/h_meta/h_stats - completely unchanged
    ec->wlen = buffered.buf_len;
    ec->close_after_flush = true;
    free(raw_buf);

    int fl = fcntl(ec->fd, F_GETFL, 0);
    if (fl >= 0) fcntl(ec->fd, F_SETFL, fl | O_NONBLOCK);

    char gid[ID_LEN + 1] = {0};
    query_game_id(r->query, gid);
    worker_handoff_push(&g_workers[game_worker_index(gid)], ec);
}

void epoll_dispatch_ws(Conn *conn, Req *r, char *raw_buf) {
    int seat; bool spectator; int cache_idx, viewer;
    GameSlot *s = ws_handshake_validate(r, &seat, &spectator, &cache_idx, &viewer);
    if (!s) { respond_ctl_error(conn, 401, CTL_ERR_WS_AUTH); conn_close(conn); free(raw_buf); return; }
    char accept[64];
    if (!ws_accept_from_key(r->ws_key, accept, sizeof accept)) {
        respond_ctl_error(conn, 400, CTL_ERR_WS_KEY); conn_close(conn); free(raw_buf); return;
    }

    EConn *ec = calloc(1, sizeof *ec);
    if (!ec) { conn_close(conn); free(raw_buf); return; }
    ec->kind = ECONN_WS;
    ec->fd = conn->fd;
    ec->slot = s; ec->seat = seat; ec->spectator = spectator; ec->cache_idx = cache_idx; ec->viewer = viewer;
    ec->phase = WSP_HDR2; ec->msg_opcode = -1;
    ec->wbuf_cap = WS_WBUF_CAP;
    ec->wbuf = malloc((size_t)ec->wbuf_cap);
    if (!ec->wbuf) { free(ec); conn_close(conn); free(raw_buf); return; }

    Conn buffered; conn_init_buffered(&buffered, ec->wbuf, ec->wbuf_cap);
    WsConn wc;
    if (!ws_send_handshake_and_push(&wc, buffered, accept, s, cache_idx, viewer)) {
        free(ec->wbuf); free(ec); conn_close(conn); free(raw_buf); return;
    }
    ec->wlen = wc.conn.buf_len; ec->woff = 0;
    pthread_mutex_lock(&s->lock);
    ec->last_pushed_version = s->version;
    s->conn_refs++;                     // this EConn now references the slot - reclamation-safe until econn_close
    s->last_active_us = now_us();
    pthread_mutex_unlock(&s->lock);

    // Any bytes the client pipelined past the upgrade request (the same
    // case ws_conn_prime primes into WsConn.pending for the thread-per-
    // connection path) - feed them straight into this connection's async
    // parser now, before the fd is even handed to its worker, so a
    // pipelined first move/poll is never lost.
    if (r->body_len > 0) {
        int off = 0, n = r->body_len; const unsigned char *body = (const unsigned char *)r->body;
        while (off < n) {
            int consumed = 0;
            int rc = wsasync_feed(ec, body + off, n - off, &consumed);
            off += consumed;
            if (rc == WSF_MESSAGE) {
                unsigned char reply[1 + 65536]; uint32_t v;
                int mtotal = ws_service_message(s, seat, spectator, cache_idx, viewer, ec->msg_buf, ec->last_msg_len, reply, &v);
                if (econn_reserve_out(ec, mtotal + 14) && ws_send_frame(&ec->wc_out, WS_OP_BIN, reply, mtotal) >= 0) {
                    econn_commit_out(ec);
                    ec->last_pushed_version = v;
                }
            } else if (rc != WSF_NEED_MORE) {
                break;   // a malformed pipelined frame - let the worker's own error handling take it from here once connected
            }
        }
    }
    free(raw_buf);

    int fl = fcntl(ec->fd, F_GETFL, 0);
    if (fl >= 0) fcntl(ec->fd, F_SETFL, fl | O_NONBLOCK);

    worker_handoff_push(&g_workers[game_worker_index(s->id)], ec);
}

bool shard_start_workers(void) {
    pthread_t wt;
    for (int i = 0; i < g_n_game_workers; i++) {
        Worker *w = &g_workers[i];
        w->idx = i;
        w->epfd = epoll_create1(0);
        w->wake_evfd = eventfd(0, EFD_NONBLOCK);
        if (w->epfd < 0 || w->wake_evfd < 0) {
            fprintf(stderr, "fatal: epoll worker %d setup failed (epoll_create1/eventfd)\n", i);
            return false;
        }
        pthread_mutex_init(&w->handoff_mtx, NULL);
        w->handoff_head = w->handoff_tail = NULL;
        w->ws_head = NULL;
        struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };   // NULL data.ptr marks the wake_evfd itself - see epoll_worker_main
        if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->wake_evfd, &ev) < 0) {
            fprintf(stderr, "fatal: epoll worker %d setup failed (epoll_ctl)\n", i);
            return false;
        }
        if (pthread_create(&wt, NULL, epoll_worker_main, w) != 0) {
            fprintf(stderr, "fatal: epoll worker %d thread create failed\n", i);
            return false;
        }
        pthread_detach(wt);
    }
    g_epoll_active = true;
    return true;
}
