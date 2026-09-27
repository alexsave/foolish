// httpd.h - the front door: what arrives on a socket, and what goes back.
//
// A hand-rolled HTTP/1.1 request parser (swap for mongoose in a real
// deployment), the response encoders every handler answers through, the
// per-IP rate limiter that guards the abuse-prone endpoints, the route table,
// the typed work-queue pools, and the SO_REUSEPORT acceptor loop that drives
// all of it.
//
// Nothing here knows a game rule or a wire format. A handler is handed a
// parsed Req and a Conn and answers with one of the respond_* calls; what the
// bytes MEAN is ctl_wire.h's (the control plane) or view.c's (a masked state).
#ifndef FOOLISH_HTTPD_H
#define FOOLISH_HTTPD_H

#include <stdatomic.h>
#include <stdbool.h>

#include "conn.h"   // Stage 3: TLS - see conn.h and the "STAGE 3: OpenSSL TLS" block in httpd.c

typedef struct {
    char method[8];
    char path[256];
    char query[256];
    char token[96];         // Bearer token - a signed binary token is ~70 base64url chars
    char *body; int body_len;
    int  content_length;    // parsed off Content-Length: while walking the headers
    bool is_ws_upgrade;     // GET with Upgrade: websocket + Connection: Upgrade + a key
    char ws_key[64];        // Sec-WebSocket-Key, verbatim (base64, ~24 chars)
    char client_ip[46];     // forwarded client IP (Fly-Client-IP / X-Forwarded-For 1st hop); "" if none - rate limiting
} Req;

#define REQ_BUF_CAP (1 << 16)

// Reads a full HTTP request (headers, then body up to Content-Length) off
// `conn` into caller-owned `buf` (>= cap bytes), and parses it into `r`. `r->
// body`/`r->body_len` end up pointing INTO `buf` - the caller must keep
// `buf` alive for as long as `r` is used (see WorkItem/WsSpawnArg, which
// both carry the buffer alongside the parsed Req for exactly this
// reason). Returns false on a malformed/empty request (caller should just
// close the conn and free buf).
bool read_and_parse_request(Conn *conn, char *buf, int cap, Req *r);

// A control-plane answer: one packed ctl_wire.h frame, as
// application/octet-stream. This is what every endpoint that used to emit a
// flat JSON object answers with now.
void respond_ctl(Conn *conn, int code, const unsigned char *frame, int len);

// The refusal shorthand: encodes CTL_ERROR with `reason` (a CTL_ERR_* from
// ctl_wire.h) and sends it with `code`. Replaces the {"error":"..."} bodies -
// the string was never prose for a human, it was a tag, and a byte carries a
// tag exactly as well.
void respond_ctl_error(Conn *conn, int code, int reason);

// Raw bytes (the packed KERNEL wire - a masked state_put view). The client
// decodes with its own kernel-wire reader (MaskedView etc.).
void respond_bin(Conn *conn, int code, const unsigned char *data, int len);

// text/plain body - used by /metrics (Prometheus text exposition format is
// line-based text, NOT a packed frame and NOT a binary buffer: Prometheus/
// Grafana/Fly can only scrape text, so this one endpoint speaks text on
// purpose).
void respond_text(Conn *conn, int code, const char *body, int len);

// --------------------------------------------------------------------------
// Per-IP rate limiting for the abuse-prone endpoints (/auth/signup, /create):
// a token bucket per client IP. This is a speed bump, not a security boundary -
// it stops a script minting accounts/games faster than any human, which the
// process-wide --max-conns ceiling can't (one IP can churn short connections).
// Fly's edge handles L3/L4 floods; this is the L7 per-endpoint logic only the
// app can do. Direct-mapped table: a hash collision just means two IPs share a
// bucket (slightly stricter for them), never a bypass. --ratelimit-rpm sets the
// per-IP steady rate; 0 disables.
// --------------------------------------------------------------------------
extern float g_rl_rate;    // tokens/sec refilled per IP; 0 = OFF (opt-in via --ratelimit-rpm, so local load tools aren't throttled)
extern float g_rl_burst;   // bucket capacity (burst allowance)
extern atomic_ulong g_rate_limited;   // /metrics counter: requests this limiter turned away
bool ratelimit_allow(unsigned int ip);

// The rate-limit bucket key for this request: the forwarded client IP hashed
// (Fly-Client-IP / X-Forwarded-For - required to be meaningful behind Fly's
// edge proxy, where getpeername sees only the proxy), else the direct peer's
// IPv4. Both collapse to a uint32 the token-bucket table keys on.
unsigned int client_ip_key(const Req *r, const Conn *conn);

// Production hygiene: a live-connection gauge (admission control +
// observability) and the connection ceiling. The acceptor loop reads the gauge
// to shed at the ceiling; the epoll front-end (shard.c) is what moves it, once
// per connection admitted and once per connection closed.
extern atomic_int g_live_conns;   // currently-open epoll connections
extern int g_max_conns;      // 0 = unlimited; --max-conns=N sheds NEW connections past N (OOM guard)
extern int g_io_timeout_s;   // read/write deadline on a still-blocking accepted socket (slowloris guard); 0 = off, --io-timeout-s=N

// --------------------------------------------------------------------------
// STAGE 3: TLS listener state. `g_tls_ctx` is built ONCE in main() - before
// the accept loop, before any worker/connection thread exists - off
// tls_server_ctx_create (conn.c), then only ever READ afterward (every
// accepted connection calls conn_tls_accept, which allocates its OWN fresh
// SSL* off this ctx; the ctx itself is never mutated post-setup), so
// sharing it read-only across every worker/connection thread is safe under
// OpenSSL 3's default library context - see TLS.md's Helgrind section for
// the verification. NULL means plaintext (the default, and the ONLY mode
// when --tls isn't passed) - the accept loop branches on this exactly
// once per accepted connection.
// --------------------------------------------------------------------------
extern SSL_CTX *g_tls_ctx;

// Dispatch one already-parsed request to its handler. The typed work-queue
// workers and the epoll one-shot dispatcher both go through this, so there is
// exactly one route table.
void route(Req *r, Conn *conn);

// --------------------------------------------------------------------------
// Work-queue thread routing (T2a Deliverable 2), and the acceptors.
// See httpd.c for the pool design and the empirical sweep behind the defaults.
// --------------------------------------------------------------------------
#define N_META_WORKERS_DEFAULT 0
#define MAX_META_WORKERS 32
#define N_CREATE_WORKERS_DEFAULT 1
#define MAX_CREATE_WORKERS 32
#define N_ACCEPT_THREADS_DEFAULT 2
#define MAX_ACCEPT_THREADS 64

extern int g_n_meta_workers;
extern int g_n_create_workers;
extern int g_n_accept_threads;

// Bring-up, in the order main() runs it: the queues first, then the create/auth
// pool (BOTH modes), then - only under --tls - the meta/game pools the epoll
// front-end otherwise replaces.
void httpd_queues_init(void);
void httpd_start_create_workers(void);
void httpd_start_tls_fallback_workers(void);

// Create a listening socket bound to `port`. With reuseport, sets
// SO_REUSEPORT so multiple sockets can share the port and the kernel
// load-balances accepts across them (the SO_REUSEPORT multi-acceptor model);
// it also lets a redeploy bind the port before the old process has fully
// exited. Backlog is deliberately large - a server aiming at tens of
// thousands of connections must not drop SYNs during an arrival burst.
// Returns the fd, or -1 on failure (the caller decides fatal-ness).
int make_listener(int port, bool reuseport);

// One acceptor thread: owns `srv` (its own SO_REUSEPORT listener, passed as
// an int in arg) and runs the accept -> (TLS handshake) -> read+parse ->
// dispatch loop. N of these run concurrently, one per listener. Never returns.
void *acceptor_main(void *arg);

#endif
