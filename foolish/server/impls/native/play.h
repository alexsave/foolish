// play.h - the action path: applying a move to a dealt game, and serving the
// masked view that results.
//
// Everything a SEATED client (or a spectator) does to a PLAYING game funnels
// through here, whatever transport it arrived on - HTTP /action, a /ws binary
// frame, or a WebTransport stream via game_bridge.c. That is deliberate: the
// ownership check, the apply, the version bump, the bot wakeup and the per-seat
// view cache are one decision each, made once, not once per front-end.
//
// No rule is decided here. awire_decode/awire_apply own the move, state_put
// owns the masked view; this file owns who may ask and what gets cached.
#ifndef FOOLISH_PLAY_H
#define FOOLISH_PLAY_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "httpd.h"
#include "registry.h"

extern atomic_ulong g_moves_applied;   // total client moves the kernel accepted (/stats + /metrics throughput gauge)

// Serialize a masked view, reusing the cached bytes from GameSlot when
// nothing has changed since they were computed (PROFILE_HOTPATH.md "T1c" -
// see GameSlot's `version`/`view_cache*` fields in registry.h for the
// invariant). `cache_idx` selects WHICH cached slot to use/fill: 0 <=
// cache_idx < num_players <= MAX_PLAYERS for a seated client's own cache, or
// SPECTATOR_CACHE_IDX (Stage 4) for the one shared spectator slot every
// spectator of this game reads. `viewer` is state_put's own viewer argument
// (the seat number, or VIEW_SPECTATOR) - kept separate from `cache_idx`
// because the spectator cache slot's INDEX (MAX_PLAYERS) is not a valid
// state_put viewer value (that's VIEW_SPECTATOR, -1). MUST be called with
// s->lock held (same contract as a bare state_put call here) and
// `cache_idx`/`viewer` MUST already be known valid - ws_handshake_validate
// validates both at handshake time before ever calling in (a seated
// client's own seat, or the fixed spectator pair); this function does not
// re-check, so it is not safe to point at an unvalidated/attacker-
// controlled index.
//
// Returns a POINTER to the serialized view bytes instead of copying them into
// an out buffer - on a cache hit that is zero copies (the pointer is the cache
// slot itself), letting the push path land the one unavoidable copy directly
// in its output buffer via ws_send_frame2 (PROFILE_HOTPATH.md: memcpy was ~18%
// of the epoll build, the redundant cache->scratch->wbuf double copy).
// `fallback` is caller-owned scratch (>= 1 + 65536) used ONLY on the rare path
// where a view is too big to cache (n > VIEW_CACHE_CAP - never at this build's
// caps, see VIEW_CACHE_CAP): there the bytes are serialized into `fallback` and
// *pp points at it. The returned pointer is valid only while s->lock is still
// held - a later version bump may recompute the slot.
int state_put_cached_ptr(GameSlot *s, int cache_idx, int viewer,
                         const unsigned char **pp, unsigned char *fallback);

// Copy-out wrapper kept for the thread-per-connection (--tls) path, whose
// ws_send_frame concatenates a single contiguous payload. The epoll push path
// uses state_put_cached_ptr + ws_send_frame2 to avoid this copy.
int state_put_cached(GameSlot *s, int cache_idx, int viewer, unsigned char *out);

// One seat's masked view (or VIEW_SPECTATOR's) of a game, for every transport
// that serves it (HTTP GET /state, HTTP/3 /state through game_bridge.h). A seat's
// view holds that seat's hand, so only the account seated there may read it: the
// same rule /ws's handshake and gb_apply_move apply. It used to be served to anyone
// who named the seat (and seat 0 to a request naming none), over TCP and QUIC
// alike, so any client could read every hand of any game. The spectator view is
// public. The trusted VIEW_UNMASKED (-2) is never served. `out` must hold 65536
// bytes. Returns the bytes written, or a SEAT_VIEW_* refusal.
#define SEAT_VIEW_BAD_SEAT     (-1)
#define SEAT_VIEW_NO_GAME      (-2)
#define SEAT_VIEW_UNAUTHORIZED (-3)
#define SEAT_VIEW_NOT_YOURS    (-4)
int seat_view_for(const char *gid, int seat, const char *token, unsigned char *out);

// Decode+apply one client frame with s->lock ALREADY HELD. Returns true iff a
// real move was decoded AND the kernel accepted it (awire_apply) - on
// acceptance it bumps s->version, marks the game dirty, and signals the bot
// game-loop. A spectator frame, an empty poll, a not-playing game, or a
// rejected/illegal move returns false and mutates nothing. Factored out so the
// epoll push path (econn_push_view) and the --tls thread-per-conn path
// (ws_service_message) share ONE copy of the apply logic and can never drift.
bool ws_apply_move_locked(GameSlot *s, int seat, bool spectator,
                          const unsigned char *in, int mlen);

// Applies one received WS application message (a real awire move, or an
// empty/rejected/spectator-sent poll) and produces the reply payload - the
// exact per-message body ws_conn_thread's loop always ran. `in`/`mlen` is
// the just-received message (mlen==0 is a plain poll); `msg` is
// caller-owned, >= 1+65536 bytes. Returns the total reply length (>= 1),
// [ok:u8][state_put_cached bytes]. MUST be called with NO lock held - takes
// and releases s->lock itself, exactly once. `out_version` (may be NULL) is
// set to `s->version` as observed under that SAME lock acquisition, right
// before it releases - Stage 6's epoll worker uses this to record exactly
// which version this connection's peer was just brought up to date with,
// without a second, unlocked (racy) read of s->version afterward.
int ws_service_message(GameSlot *s, int seat, bool spectator, int cache_idx, int viewer,
                       const unsigned char *in, int mlen, unsigned char *msg, uint32_t *out_version);

// Route handlers. Each follows the SAME registry->game lock handoff (see
// "Locking" in registry.h): take g_registry_lock, find the User*/GameSlot*
// (copying out any User field it still needs as a local - the User itself is
// only safe to dereference while g_registry_lock is held), take the GameSlot's
// own `lock`, release g_registry_lock, do the game work, release the GameSlot
// lock, THEN respond (I/O never happens with either lock held).
void h_action(Req *r, Conn *conn);
void h_state(Req *r, Conn *conn);
void h_status(Req *r, Conn *conn);

// game_id off a query string, into a caller-owned gid[ID_LEN+1]. Every path
// that shards or looks up by game (h_action, h_state, h_status, the /ws
// handshake, the one-shot epoll dispatcher, classify_queue) reads it the same
// way, so it is written once. `gid` is left "" when the query carries none -
// which a lookup then turns into the same "no such game" any bogus id gets.
void query_game_id(const char *query, char *gid);

#endif
