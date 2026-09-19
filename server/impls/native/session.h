// session.h - who a request is, and how it proved it.
//
// Accounts (sign up / sign in are the same act here: a username claims or
// re-claims a User) and the stateless signed session token that every
// authenticated endpoint checks. There is no token store and no session table:
// the token IS the session, and any instance holding the same secret can
// verify it.
#ifndef FOOLISH_SESSION_H
#define FOOLISH_SESSION_H

#include <stdbool.h>

#include "httpd.h"
#include "registry.h"

// --------------------------------------------------------------------------
// Stateless signed session tokens (Bucket A). A token is a signed BINARY blob
// (NO JWT, NO JSON): base64url( user_id[ID_LEN] || exp_be[8] |
// HMAC-SHA256(secret, user_id||exp)[32] ). Any instance holding the same secret
// verifies it - no shared token store needed, which is the auth half of
// horizontal scale-out - and it carries an absolute expiry. The secret comes
// from $FOOLISH_TOKEN_SECRET (64 hex chars) so tokens survive restart and span
// instances; absent that, a random per-process secret is used (tokens then
// don't survive a restart - fine for dev, flagged at startup).
// --------------------------------------------------------------------------

// One-time secret init (main()). $FOOLISH_TOKEN_SECRET = 64 hex chars -> 32-byte
// key (share it across instances / set it to survive restart), else random.
void token_secret_init(void);

// Mint a fresh signed token for `user_id` into out[0..cap).
void make_token(const char *user_id, char *out, int cap);

// Verify signature + expiry; on success fills user_id_out[ID_LEN+1]. The MAC
// compare is constant-time so a forger can't byte-search the signature.
bool verify_token(const char *token, char *user_id_out);

// token -> the User it names, or NULL (bad signature, expired, or no such
// user). Caller MUST hold g_registry_lock.
User *user_by_token(const char *token);

// POST /auth/signup and POST /auth/signin - one handler, because in this store
// they are one act: a username that exists is re-claimed, one that doesn't is
// created, and either way the answer is a fresh signed token. Answers a
// CTL_SESSION frame (ctl_wire.h).
void h_signup(Req *r, Conn *conn);

#endif
