// v1_http.h - the binary surface's HTTP routes (DESIGN.md "Binary surface";
// the bytes are the kernel's netwire.h).
//
//   GET  /v1/games           (bearer) -> NW_GAMES, the caller's views, newest first
//   GET  /v1/games/<id>      (bearer) -> NW_VIEW, the caller's view, else the spectators'
//   GET  /v1/chat/<id>       (bearer) -> NW_CHATS, the last 100, oldest first (none for a non-member)
//   POST /v1/chat/<id>       (bearer, member) NW_CHAT_SEND -> 204
//   GET  /v1/bots            -> NW_BOTS, ladder order (public: cached at the edge, a day)
//   GET  /v1/leaderboard     -> NW_BOARD: the top 100 humans and the top 100 bots who have played
//                               (public: cached at the edge, an hour; invalidated by a rated game)
//   GET  /v1/history         (bearer) -> NW_HISTORY: the caller's rating and last 50 replays
//   GET  /v1/replay/<id>     (bearer) -> NW_REPLAY: the newest replay of a game the caller played
//
// The sessions (/v1/auth/*) are the platform's (c_base core/src/auth/auth_http.h), in the same
// bytes (netwire.h's kinds 1-5 and 20 are cbase/auth_wire.h's, asserted in v1_http.c).
// The writes (create, action, meta, account/delete) are functions_http.c's,
// mounted under /v1/ too. Every refusal is the kernel's AskRefusal; an account
// refusal is ASK_WHY_ACCOUNT with an NW_ACCOUNT_E_* code. No route takes the
// anon key.
#ifndef CB_V1_HTTP_H
#define CB_V1_HTTP_H

#include "cbase/app.h"
#include "ctx.h"

#include <stddef.h>

// The same routes as a list, for the app (app/foolish_app.c).
extern const cb_route_list cb_v1_route_list;
int cb_v1_routes(cb_ctx *ctx);

#endif
