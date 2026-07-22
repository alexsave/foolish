// game_bridge.h — thin, thread-safe wrappers the QUIC/WebTransport transport
// (quic_wt.c) uses to reach the shared in-memory game WITHOUT seeing
// foolish_server.c's statics. Implemented in foolish_server.c, compiled only
// in the QUIC build (-DFOOLISH_QUIC). Every call is safe to make from the QUIC
// thread concurrently with the TCP acceptors and epoll workers: they take the
// exact same registry / per-game locks the HTTP and /ws paths do, so QUIC is
// just another front-end onto the one authoritative game state.
#ifndef FOOLISH_GAME_BRIDGE_H
#define FOOLISH_GAME_BRIDGE_H

// Serialize the masked view of `game_id` for `seat` into out[0..cap).
// Unauthenticated, exactly like HTTP GET /state: the view is already masked
// per seat. `seat` follows /state semantics — VIEW_SPECTATOR (-1) or a concrete
// seat (0..num_players-1); the trusted VIEW_UNMASKED (-2) sentinel is rejected.
// Returns the number of bytes written (>= 0), or -1 if the game doesn't exist,
// the seat is invalid, or `cap` is too small.
int gb_state_for(const char *game_id, int seat, unsigned char *out, int cap);

// Apply a move from a SEATED client and return that seat's fresh masked view.
// `token` must own `seat` in `game_id` — the identical Bearer-token + seat
// ownership check GET /ws performs (see ws_handshake_validate). Applies the
// move bytes in[0..len) to the shared game exactly as a /ws move would, then
// serializes the seat's resulting view into out[0..cap). An illegal or
// rejected move is NOT an error (same posture as /ws): the current, unchanged
// view is returned. Returns the view length written (>= 0), or -1 if auth or
// the game/seat lookup fails, or `cap` is too small. Pass in=NULL/len=0 to
// validate the session and fetch the current view without submitting a move.
int gb_apply_move(const char *game_id, const char *token, int seat,
                  const unsigned char *in, int len, unsigned char *out, int cap);

#endif
