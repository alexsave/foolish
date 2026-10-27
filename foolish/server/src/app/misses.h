// misses.h - THE GUESSED-ID RULE, its one owner: how often one address may ask after a game
// it does not sit at and find nothing.
//
// A game id is six hex characters (store/game_id.h), 16.7 million of them: few enough for a
// script to walk, looking for open lobbies to join. Looking at a game whose id you were given
// is intended (anyone may spectate), so a lookup that finds a game costs nothing. What costs is
// a MISS, an answer that tells a game that is there from one that is not:
//   - an id of the wrong shape, or one that names no game, at GET /v1/games/<id>,
//     GET /v1/wait/<id>, GET /v1/replay/<id>, POST /v1/meta and POST /v1/action;
//   - a move or an ask at a game the caller does not sit at (the kernel's TABLE_E_NOT_SEATED),
//     which tells the two apart as a 404 does;
//   - a join the kernel refused (a game dealt already, or full).
// Each takes one from the address's CB_LIMIT_ID_MISS_IP bucket (rate_limits.h: 30, refilled
// evenly over 5 minutes; CBASE_LIMIT_ID_MISS_IP sets the burst). The address is the viewer's
// (rate_limits.h cb_viewer_key); a socket's REQ carries its socket's.
//
// While an address's bucket is empty it is answered only about games its caller sits at: a
// seated read, long poll, move or ask runs as ever, and everything else (a spectator's read, a
// join, a miss) is 429 BUSY with Retry-After, the same answer whether the game is there or not,
// so a limited address learns nothing. A person never meets this: polling and play find their
// game every time, and only a mistyped code or a stale link misses.
#ifndef CB_MISSES_H
#define CB_MISSES_H

#include "ctx.h"
#include "net/http.h"

#include <stdbool.h>

// Before a lookup the caller may not be entitled to (a spectator's read, a join): true when the
// address may make it; false with the 429 answered. Nothing is taken: only a miss costs.
bool cb_miss_may_look(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);

// A miss: takes one from the address. True when that was allowed, and the route answers its
// own refusal (the 404, the 400); false with the 429 answered instead (the address had none
// left).
bool cb_miss_take(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);

#endif
