// shard.h - how connections are multiplexed: one epoll loop per game shard.
//
// ==========================================================================
// STAGE 6 - epoll-per-shard connection I/O (SERVER_SCALING.md "Stage 6").
//
// T2a (SERVER_SCALING.md "Deliverable 2 - WS design: (B), not (A)") deferred
// this on purpose: a thread per live /ws connection was the correct,
// Helgrind-provable-clean choice to ship first, with the tradeoff stated
// plainly - ~0.9MB/connection (thread stack) and, at real scale, scheduler
// oversubscription (Stage 5 measured ~300+ runnable OS threads on 4 cores at
// 160 games, capping bot-compute CPU engagement at ~80% even with the
// kernel's own serialization removed). This is design (A): each of the
// `g_n_game_workers` game-worker threads runs its OWN epoll loop,
// single-threaded, over exactly the connections whose game_id hashes to it
// (the SAME hash classify_queue uses to shard the typed HTTP queues) - no
// dedicated OS thread per connection, plaintext or `/ws`, one-shot or
// persistent.
//
// SCOPE (stated up front, the same "correct partial beats broken unified"
// posture the task itself allows): this covers **plaintext** connections
// only. Non-blocking OpenSSL (SSL_read/SSL_write's WANT_READ/WANT_WRITE state
// machine re-armed against epoll's read/write readiness) is real, fiddly,
// per-direction state that this stage did not implement - see
// SERVER_SCALING.md's "Stage 6" section for the honest writeup. A `--tls`
// server instead keeps the ENTIRE pre-Stage-6 design: thread-per-`/ws`-
// connection (wsconn.h ws_conn_thread) + the typed HTTP work-queue pools
// (httpd.c, both still fully intact, unchanged) - see main()'s branch on
// `g_tls_ctx` for exactly where the two designs split.
//
// DESIGN
//   - Each epoll worker (`Worker`, below) owns one `epoll_fd`, one `eventfd`
//     (`wake_evfd` - the cross-thread wakeup primitive, used for TWO
//     things: the acceptor handing off a freshly-accepted fd, and
//     bot_thread signaling a state change - see epoll_notify_game_changed's
//     doc in push.h), a bounded mutex-guarded handoff queue (acceptors
//     produce, this worker alone consumes), and a doubly-linked list of its
//     live `/ws` connections (`ws_head` - this worker's own thread is the
//     ONLY reader/writer of that list, so it needs no lock of its own; see
//     "Threading" below).
//   - The ACCEPTOR (httpd.c's accept loop) still fully reads+parses each
//     request BLOCKING, exactly as every earlier stage did
//     (read_and_parse_request) - see SERVER_SCALING.md's "Stage 6" section
//     for why this is a deliberate, documented scope decision rather than a
//     deviation: it keeps 100% of the existing request-parsing code (and the
//     /ws handshake's auth+upgrade logic) completely unchanged, and it does
//     not reintroduce thread-per-connection - no thread is spawned either
//     way, the fd is just handed to a shard's epoll loop once the acceptor
//     is done with it. What's NEW is what happens AFTER the read: instead of
//     spawning a `ws_conn_thread` or pushing a `WorkItem` onto a typed queue,
//     the acceptor builds the response (or the 101 handshake + initial state
//     push) into a fresh `EConn`'s buffered output (`conn_init_buffered` -
//     see conn.h) by calling the SAME handler code (route()/h_action/
//     h_state/h_meta/ws_handshake_validate/ws_send_handshake_and_push) this
//     server has always used, flips the fd non-blocking, and hands it to
//     `game_worker_index(game_id)`'s `Worker` over the handoff queue. The
//     worker's job from then on is purely non-blocking flush (one-shot) or
//     the ongoing async WS frame loop (`/ws`) - it never re-does any of the
//     auth/business logic the acceptor already ran.
//   - Non-blocking WS framing: `wsasync_feed` (shard.c) is a byte-for-byte
//     reimplementation of ws_recv_message's (ws.c) RFC 6455 decode -
//     2-byte header, optional 2/8-byte extended length, optional 4-byte
//     mask, payload, control frames answered inline, CONT-frame reassembly
//     - restructured as an explicit, resumable phase machine that consumes
//     bytes fed in from a non-blocking read() instead of blocking on
//     conn_read/ws_fill. See its own doc for the exact contract.
//   - The one cross-thread seam: bot_thread mutates a `GameSlot` a worker
//     also owns connections for. See push.h - that is the piece Helgrind's
//     job is to prove race-clean (SERVER_SCALING.md "Stage 6").
//
// THREADING
//   Each `Worker`'s epoll loop is fully SINGLE-THREADED over its own shard:
//   only that worker's own thread ever touches its `EConn`s, its `ws_head`
//   list, or calls epoll_ctl on its `epoll_fd` - so none of that needs a
//   lock. The two things that ARE touched cross-thread are (a) the handoff
//   queue (acceptors produce, the worker consumes - guarded by
//   `handoff_mtx`, the same bounded-queue discipline `WorkQueue` in httpd.c
//   uses) and (b) `GameSlot.lock` itself (already the existing, proven
//   per-game lock every path in this server goes through - bot_thread and a
//   worker's WS-message handling both take it exactly the way bot_thread
//   and ws_conn_thread always have).
// ==========================================================================
#ifndef FOOLISH_SHARD_H
#define FOOLISH_SHARD_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "httpd.h"
#include "registry.h"
#include "ws.h"

#define N_GAME_WORKERS_DEFAULT 4
#define MAX_GAME_WORKERS 64

// `--game-workers=N` sizes the epoll shard count in plaintext mode and the
// typed game-queue pool under --tls - the same knob either way, because it is
// the same sharding decision (SERVER_SCALING.md).
extern int g_n_game_workers;

#define EPOLL_MAX_EVENTS   256
#define WS_IN_CAP          4096                  // incoming client frame cap - matches ws_conn_thread's `in[4096]`; client->server messages are always small (a move or a poll)
#define WS_OUT_CAP         4096                  // one reply frame's cap - real worst case is 690B (VIEW_CACHE_CAP's own doc); generous margin, same sizing discipline
#define WS_WBUF_CAP        (2 * WS_OUT_CAP)      // room for one in-flight (unflushed) frame + one freshly queued push, per live /ws connection
#define ONESHOT_WBUF_CAP   (1 + 65536 + 512)     // one-shot HTTP response cap - h_state's raw (uncached) state_put can be up to 65536; +512 header margin

typedef enum { ECONN_ONE_SHOT, ECONN_WS } EConnKind;

// wsasync_feed's return contract (see its own doc in shard.c).
#define WSF_ERROR     (-1)
#define WSF_NEED_MORE   0
#define WSF_MESSAGE     1

// Incremental, resumable WS frame-parse phase - see wsasync_feed.
typedef enum { WSP_HDR2, WSP_EXTLEN, WSP_MASK, WSP_PAYLOAD } WsParsePhase;

typedef struct EConn {
    int fd;
    EConnKind kind;

    // Non-blocking WRITE side: wbuf[woff..wlen) are the bytes still owed to
    // the peer. Filled by whichever handler produced a reply - a one-shot
    // route() call, ws_send_handshake_and_push, or ws_send_frame appending
    // through `wc_out` (a buffered Conn - conn.h - wrapping THIS SAME
    // wbuf), never a real write() until the epoll loop flushes it. Touched
    // only by this EConn's owning worker thread.
    unsigned char *wbuf;
    int wbuf_cap, wlen, woff;
    bool want_epollout;      // whether EPOLLOUT is currently armed in epoll_ctl for this fd
    bool close_after_flush;  // ECONN_ONE_SHOT: close once wbuf fully drains (every one-shot response ends the connection, same as the old `Connection: close`)
    bool closed;             // econn_close ran: fd shut + unlinked, but free() DEFERRED to end of this epoll batch (see econn_close / econn_drain_dead). Guards against a stale same-batch event re-closing it.

    // ECONN_WS fields (unused for ECONN_ONE_SHOT):
    GameSlot *slot;
    int seat;
    bool spectator;
    int cache_idx, viewer;             // see state_put_cached's doc (play.h) for why these differ for a spectator
    uint32_t last_pushed_version;      // s->version as of the last reply/push THIS connection's peer actually received
    WsConn wc_out;                     // output-only WsConn wrapping wbuf via a buffered Conn (conn.h) - see econn_reserve_out
    // Incremental parser state (wsasync_feed):
    WsParsePhase phase;
    unsigned char hdrbuf[8];
    int hdr_have, hdr_need;
    int fin, op, masked;
    int64_t frame_len;
    unsigned char mkey[4];
    int64_t payload_got;
    bool frame_validated;               // has THIS frame's header (new-vs-continuation, oversized) already been validated? See wsasync_feed's WSP_PAYLOAD case.
    int msg_opcode;                    // -1 iff not currently assembling a fragmented data message
    int msg_total;                     // bytes assembled so far into msg_buf
    int last_msg_len;                  // set alongside a WSF_MESSAGE return - see wsasync_feed's doc
    unsigned char msg_buf[WS_IN_CAP];
    unsigned char ctrl_buf[125];       // control-frame (PING/PONG/CLOSE) payload scratch - RFC 6455 5.5: control frames are never > 125 bytes

    struct EConn *prev, *next;   // intrusive list - EITHER this worker's handoff queue (via `next` only, before the handoff completes) OR its active ws_head list (both, after) - the two lifetimes never overlap
} EConn;

typedef struct Worker {
    int idx;
    int epfd;
    int wake_evfd;
    pthread_mutex_t handoff_mtx;
    EConn *handoff_head, *handoff_tail;   // acceptor-produced, this worker alone consumes (drain_handoff_queue)
    EConn *ws_head;                       // this worker's live /ws connections - single-threaded, no lock (see "Threading" above)
    EConn *dead_head;                     // econn_close'd this batch, freed by econn_drain_dead at the end of the epoll loop iteration - see econn_close
} Worker;

extern Worker g_workers[MAX_GAME_WORKERS];
// Set true only when main() actually starts the epoll worker pool
// (plaintext mode). epoll_notify_game_changed no-ops while false, so a
// --tls server (which never populates g_workers) never touches it.
extern bool g_epoll_active;

// Same shard hash classify_queue's game-worker routing has always used
// (hash_str(game_id) % g_n_game_workers) - reused here so a game's HTTP
// requests, its /ws connections, AND its bot_thread's push notifications all
// agree on exactly one owning worker.
int game_worker_index(const char *game_id);

// Prepares to append `need` more bytes to ec->wbuf: compacts away any
// already-flushed prefix first (slides the unflushed [woff,wlen) tail down
// to offset 0), so backpressure that has only PARTIALLY drained a previous
// write never blocks a new append from reusing that reclaimed space. Fills
// ec->wc_out with a buffered Conn (conn.h) over the (now-compacted) tail -
// callers append through `ec->wc_out` (ws_send_frame et al.), then MUST call
// econn_commit_out to publish the new length. Returns false (appends
// nothing) iff there truly isn't `need` bytes of room even after compacting
// - a real backpressure case (this connection's peer has stopped reading
// entirely) - callers drop that specific send rather than risk writing a
// partial/corrupt frame into the buffer.
bool econn_reserve_out(EConn *ec, int need);
void econn_commit_out(EConn *ec);

// Non-blocking flush of ec->wbuf[woff..wlen). Returns false iff `ec` was
// closed (a real I/O error, or - ECONN_ONE_SHOT with close_after_flush - a
// completed flush): the caller must not touch `ec` again after a false
// return. On EAGAIN, arms EPOLLOUT and returns true (still open, not yet
// fully drained) - the next EPOLLOUT event resumes the flush.
bool econn_try_flush(Worker *w, EConn *ec);

// Shuts a connection's fd and unlinks it, but DEFERS free() to the end of the
// current epoll batch (econn_drain_dead). Reason: one connection's handler can
// close another (worker_push_stale fanning a move out to a peer that then hits
// a write error), and that peer may already have its OWN event queued later in
// the SAME epoll_wait() batch - freeing it immediately turns that stale event
// into a use-after-free / double-free (memcheck-confirmed; the crash that
// aborted the server at a few hundred connections). Instead we mark it closed,
// unlink it from ws_head, and park it on w->dead_head; the event loop skips any
// event whose ec is already `closed`, and frees the parked set once the whole
// batch is drained. Idempotent: a second close (e.g. a stale HUP event) no-ops.
void econn_close(Worker *w, EConn *ec);

// --------------------------------------------------------------------------
// Acceptor-side handlers (Stage 6). Called from httpd.c's accept loop, on an
// acceptor thread, AFTER read_and_parse_request has already fully read this
// request (headers + body) - exactly the same blocking read every earlier
// stage did there. `conn`'s fd is still in its default BLOCKING mode at that
// point (flipped to non-blocking right before the handoff - safe because the
// acceptor is the fd's sole owner until that instant). Both take ownership of
// `raw_buf` and free it.
// --------------------------------------------------------------------------

// /action, /state, /status, /meta - game_id-sharded one-shot HTTP. Builds
// the response with the UNCHANGED route()/h_action/h_state/h_status/h_meta
// handlers (a buffered Conn - conn.h - captures their output into memory
// instead of a real write()), then hands the connection to its shard's
// worker purely to flush that response non-blockingly and close.
void epoll_dispatch_oneshot(Conn *conn, Req *r, char *raw_buf);

// GET /ws (seated or ?spectator=1) - validates + completes the handshake
// HERE (ws_handshake_validate/ws_send_handshake_and_push - the exact checks
// and wire bytes ws_conn_thread has always produced), then hands the
// now-upgraded connection to its shard's worker. No thread is spawned.
void epoll_dispatch_ws(Conn *conn, Req *r, char *raw_buf);

// Brings up the g_n_game_workers epoll shards and sets g_epoll_active.
// Returns false after printing the reason if any shard could not be set up -
// main() treats that as fatal, the same posture a requested-but-failed --tls
// or --db takes.
bool shard_start_workers(void);

#endif
