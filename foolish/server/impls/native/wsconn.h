// wsconn.h - what a /ws session IS: who may open one, the bytes that open it,
// and the blocking loop that services one when epoll cannot.
//
// /ws - the WebSocket hot loop (item 1/2 of PROFILE_HOTPATH.md's "where to
// speed up" list). One handshake, then ONE persistent connection carries
// every move + state push for that seat's whole session - no pthread_create
// per action.
//
// Handshake: GET /ws?game_id=..&seat=.. (Bearer token), validated exactly
// like /action (must be a real user, a real game, and THIS user's own seat -
// no seat spoofing), then the RFC 6455 upgrade (ws_accept_from_key). After
// the 101 response, the server immediately PUSHES the current masked state
// once (ok=0 - nothing was applied yet, this is just "here's where things
// stand"), then loops:
//   client sends a binary frame -> [awire bytes] apply it, or [] (empty) to
//     just poll (a seat with no eligible move yet still needs to notice
//     when OTHER seats' actions make it eligible - see foolish_hammer.c's ws
//     worker) -> server always answers with one binary frame,
//     [ok:u8][state_put(seat) bytes], ok=1 iff a real move decoded AND the
//     kernel accepted it (awire_apply's own validation - same as /action).
// A human's turn wakes the bot game-loop exactly like /action did.
//
// Stage 4 (SERVER_SCALING.md "Stage 4 - spectators"): GET
// /ws?game_id=..&spectator=1 (Bearer, no `seat=`) upgrades the SAME way but
// owns no seat - `seat_of` ownership is never checked, only that the token
// is a real user and the game exists (the seat-membership check is simply
// skipped, everything else about the handshake is identical). The
// masked view it receives is VIEW_SPECTATOR (view.h - every hand AND the
// deck hidden, not just the other seats' hands), cached in the ONE shared
// SPECTATOR_CACHE_IDX slot (every spectator of a game sees the same bytes at
// a given version, so they share one cache entry instead of paying MAX_
// PLAYERS+1 distinct recomputes). A spectator MAY NOT submit moves: any
// frame it sends - empty or not - is treated purely as "send me the current
// state" (the `!spectator` guard in ws_apply_move_locked keeps a spectator's
// bytes from ever reaching awire_decode/awire_apply at all), so `ok` is always
// 0 in a spectator's replies. Same per-game lock, same design as a seated
// client - a spectator is just another long-lived /ws connection that happens
// to skip the seat check and the apply branch.
//
// THE TWO SERVICING DESIGNS. The functions below are the parts BOTH have to
// agree on, factored out (Stage 6) so the epoll worker path (shard.c) and the
// thread-per-connection path (ws_conn_thread, below) can never drift on auth,
// the handshake wire bytes, or move-apply semantics. Splitting them out was a
// pure refactor: a single-shard, single-connection run through ws_conn_thread
// produces IDENTICAL bytes before and after.
#ifndef FOOLISH_WSCONN_H
#define FOOLISH_WSCONN_H

#include <stdbool.h>

#include "httpd.h"
#include "registry.h"
#include "ws.h"

// Validates a /ws (or /ws?spectator=1) upgrade against the registry + this
// game's roster. Returns NULL on any auth/seat failure (caller responds 401
// and closes); on success returns the owning GameSlot* with *out_seat/
// *out_spectator/*out_cache_idx/*out_viewer resolved for the rest of the
// connection's life (see state_put_cached's doc in play.h for why cache_idx
// and viewer are two different values for a spectator).
GameSlot *ws_handshake_validate(Req *r, int *out_seat, bool *out_spectator,
                                int *out_cache_idx, int *out_viewer);

// Encodes the 101 Switching Protocols response, THEN the immediate
// post-handshake state push (current masked view, ok=0 - "here's where
// things stand", not a move confirmation) into `conn` (a real fd, blocking -
// ws_conn_thread; or a Stage 6 buffered Conn - the epoll dispatcher, see
// conn.h). `accept` is the already-computed Sec-WebSocket-Accept value
// (callers check ws_accept_from_key's own failure mode separately, since the
// two paths respond to a bad key differently). On success fills *out_wc
// (mask_outgoing=0: server frames are never masked) and returns true; false
// means "close the connection, nothing more to send."
bool ws_send_handshake_and_push(WsConn *out_wc, Conn conn, const char *accept,
                                GameSlot *s, int cache_idx, int viewer);

// T2a design note (see SERVER_SCALING.md "Deliverable 2 - WS design: (B), not
// (A)"): a persistent /ws connection is the one endpoint that does NOT go
// through the typed work-queue pools - a queue worker that blocked for a
// connection's whole lifetime would defeat the point of a small, fixed worker
// pool. It keeps its own dedicated thread, spawned straight off the acceptor
// loop, and takes THIS game's `lock` - not a process-wide one - for every
// access to shared state, releasing it around all socket I/O (handshake,
// ws_recv_message, ws_send_frame). This was design (B) from the task brief:
// simpler and easier to keep Helgrind-clean than moving WS service onto a
// shared epoll-driven worker (design A); the tradeoff is a thread per live WS
// connection (PROFILE_HOTPATH.md T1c's ~0.9MB/conn memory tax), which the
// epoll design removes. Stage 6 built design (A) for plaintext (shard.c) and
// kept this one as the --tls fallback, because non-blocking OpenSSL's
// WANT_READ/WANT_WRITE state machine was out of that stage's budget - see
// SERVER_SCALING.md "Stage 6" for the honest writeup, and main()'s branch on
// `g_tls_ctx` for exactly where the two designs split.
typedef struct {
    Conn conn;
    Req req;
    char *raw_buf;   // owns the bytes r.body/r.query/etc point into until freed
} WsSpawnArg;

// The dedicated-thread entry. Takes ownership of `argp` (a malloc'd
// WsSpawnArg) and frees it, and its raw_buf, on every exit path.
void *ws_conn_thread(void *argp);

#endif
