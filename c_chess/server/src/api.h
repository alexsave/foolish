/* api.h - c_chess's routes (api.c). */
#ifndef C_CHESS_API_H
#define C_CHESS_API_H
#include "cbase/app.h"
#include "auth/auth.h"

#include <stdbool.h>
#include <stddef.h>
extern const cb_route_list cc_api_routes;
extern const cb_route_list cc_arena_routes;   /* api_arena.c */
extern const cb_route_list cc_chat_routes;    /* api_chat.c */
#define CC_N_ADMIN_CALLS 1
extern const cb_admin_call cc_admin_calls[CC_N_ADMIN_CALLS];   /* api_chat.c: the console's mute */

/* For the other route files: the answer as octets (no-store), a refusal (cw_err), and the verified
 * caller (false with the refusal answered). */
void cc_api_answer(cb_http_res *res, int status, const void *p, size_t n);
void cc_api_refuse(cb_http_res *res, int status, int code, int rc);
bool cc_api_caller(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, cb_user *u);
/* The number after the route's prefix: "/c/game/123" -> 123; 0 for none (it stops at '?'). */
uint32_t cc_api_tail_id(const char *path, const char *prefix);
#endif
