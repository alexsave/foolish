// client.h - ONE CLIENT OF THE GAME SERVER, as birdseye's fake humans and the league's watcher are:
// an account signed up (or in) over HTTP, the platform's socket (GET /ws) with its HELLO, and every
// request after that sent on the socket as an SF_REQ and answered by an SF_REPLY that this file
// matches back to what was asked. Every byte is the kernel's codecs (netwire.h) and the platform's
// stream frames (shared/c/stream_frame.h). What a client does with a push or a reply is its owner's
// (sim.c for a player, league.c for the watcher): the owner hands a callback to be_client_read.
//
// One client is driven by one thread; the only shared state is the open-socket count.
#ifndef BE_CLIENT_H
#define BE_CLIENT_H

#include "netwire.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BE_PEND_MAX  8
#define BE_PING_MS   30000

// A request on its way: what it was (the owner's kind) and when it left.
typedef struct {
    uint32_t id;
    uint8_t  kind;      // the owner's request kind
    uint8_t  hostile;   // the owner's mark (sim.c: BE_HOSTILE_* for a hostile action)
    uint32_t intent;    // an action's version, or the owner's tag (league.c: the table's slot)
    int64_t  t0_us;     // cb_mono_us at send
} be_pending;

typedef struct {
    char       name[32], password[40];
    char       uid[NW_ID_MAX + 1];
    char       token[NW_ACCESS_MAX + 1];
    int        ws;                   // -1 closed
    uint8_t   *in;
    size_t     in_len, in_cap;
    int64_t    next_ping_ms;         // cb_mono_ms
    uint32_t   req_seq;
    be_pending pend[BE_PEND_MAX];
    int        n_pend;
} be_client;

// The client of `name`, closed; false when out of memory. be_client_free gives the buffer back.
bool be_client_init(be_client *c, const char *name, const char *password);
void be_client_free(be_client *c);

// POST /v1/auth/signup, or signin when the name is taken (a rerun on the same store). Blocking.
bool be_client_sign_in(be_client *c, int port);
// Opens /ws and says HELLO (the READY comes back like any frame). false when the socket did not open.
bool be_client_open(be_client *c, int port);
void be_client_close(be_client *c);

// One request on the socket: the HTTP route's method (SF_METH_*), path and body. false when the
// socket is closed or the frame would not fit; the reply is matched with be_client_take.
bool be_client_req(be_client *c, int kind, int meth, const char *path, const uint8_t *body, size_t n, uint8_t hostile,
                   uint32_t intent);
// Hear (or stop hearing) a game's room (netwire.h NW_SUB_GAME).
void be_client_sub(be_client *c, const char *gid, int gid_len, bool unsub);
// The request a reply answers, removed from the pending list: false for one not asked here (a
// subscription's reply, or one forgotten when more than BE_PEND_MAX were in flight).
bool be_client_take(be_client *c, uint32_t id, be_pending *out);
// A ping when one is due (the platform drops a silent socket).
void be_client_ping(be_client *c, int64_t now_ms);

// Reads what the socket holds and hands each binary message to fn; answers the server's pings.
// The socket may be closed on return (an error, a CLOSE, a message past CB_WS_MAX_MESSAGE).
typedef void (*be_client_msg_fn)(void *ud, const uint8_t *msg, size_t n);
void be_client_read(be_client *c, be_client_msg_fn fn, void *ud);

// Sockets open across every client, for the gauges.
unsigned be_client_sockets(void);

#endif
