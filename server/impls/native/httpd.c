// The HTTP front door - see httpd.h.
#define _GNU_SOURCE
#include "httpd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strncasecmp - the hand-rolled header scan below
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "ctl_wire.h"
#include "lobby.h"
#include "metrics.h"
#include "play.h"
#include "registry.h"
#include "session.h"
#include "shard.h"
#include "srvthread.h"
#include "wsconn.h"

atomic_int   g_live_conns = 0;
atomic_ulong g_rate_limited = 0;
int          g_max_conns = 0;
int          g_io_timeout_s = 30;
SSL_CTX     *g_tls_ctx = NULL;

int g_n_meta_workers = N_META_WORKERS_DEFAULT;
int g_n_create_workers = N_CREATE_WORKERS_DEFAULT;
int g_n_accept_threads = N_ACCEPT_THREADS_DEFAULT;

// --------------------------------------------------------------------------
// STAGE 3: OpenSSL TLS (WSS/HTTPS) - DONE. Every plain-HTTP socket byte in
// this server funnels through io_read/io_write below, thin wrappers over
// conn_read/conn_write (conn.c/conn.h), which dispatch to read()/write() or
// SSL_read()/SSL_write() depending on this connection's Conn (a plain `int
// fd` or a per-connection `SSL*` - see conn.h). ws.c's ws_read_full/
// ws_write_full/ws_fill are the equivalent, already-updated seam for the
// WebSocket path (see ws.c's header comment), and every handler in this
// server takes a `Conn *` instead of a bare `int fd` (respond_ctl/
// respond_bin, h_signup/h_create/h_meta/h_action/h_state/h_status, route,
// worker_thread's WorkItem, ws_conn_thread's WsSpawnArg) - nothing above
// this layer inspects a raw fd directly anymore. See TLS.md for the design
// writeup, how to run with certs, and the measured overhead. The one
// non-uniform spot the seam comment (and SERVER_SCALING.md/DURABILITY.md's
// "Seams left" sections) called out ahead of time: ws_send_frame's unmasked
// server path used writev() to send a header+payload in one syscall;
// OpenSSL has no vector write, so ws.c's TLS branch concatenates into one
// buffer instead (see its comment) - everything else was a drop-in swap,
// exactly as predicted.
// --------------------------------------------------------------------------
static ssize_t io_read(Conn *c, void *buf, size_t n)  { return conn_read(c, buf, n); }
static ssize_t io_write(Conn *c, const void *buf, size_t n) { return conn_write(c, buf, n); }

// --------------------------------------------------------------------------
// Rate limiting (see httpd.h for the design note)
// --------------------------------------------------------------------------
#define RL_SIZE 65536   // power of two
typedef struct { uint32_t ip; float tokens; uint64_t last_us; } RlBucket;
static RlBucket g_rl[RL_SIZE];
static pthread_mutex_t g_rl_lock = PTHREAD_MUTEX_INITIALIZER;
float g_rl_rate  = 0.0f;
float g_rl_burst = 10.0f;

bool ratelimit_allow(unsigned int ip) {
    if (g_rl_rate <= 0.0f) return true;   // disabled
    uint64_t now = now_us();
    unsigned idx = ((unsigned)(ip * 2654435761u) >> 15) & (RL_SIZE - 1);   // Knuth multiplicative hash
    bool allow;
    pthread_mutex_lock(&g_rl_lock);
    RlBucket *b = &g_rl[idx];
    if (b->ip != ip || b->last_us == 0) {
        b->ip = ip; b->tokens = g_rl_burst; b->last_us = now;   // new/evicted IP starts with a full burst
    } else {
        b->tokens += (float)((double)(now - b->last_us) / 1e6) * g_rl_rate;
        if (b->tokens > g_rl_burst) b->tokens = g_rl_burst;
        b->last_us = now;
    }
    allow = b->tokens >= 1.0f;
    if (allow) b->tokens -= 1.0f;
    pthread_mutex_unlock(&g_rl_lock);
    if (!allow) atomic_fetch_add_explicit(&g_rate_limited, 1, memory_order_relaxed);
    return allow;
}

// The client's IPv4 as a host-order uint32 (0 if unavailable - a buffered Conn,
// a non-IPv4 peer, or a getpeername failure all map to one shared bucket, which
// only makes the limiter stricter, never leaks past it).
static uint32_t conn_peer_ip(const Conn *c) {
    struct sockaddr_in sa; socklen_t sl = sizeof sa;
    if (c->fd < 0 || getpeername(c->fd, (struct sockaddr *)&sa, &sl) != 0 || sa.sin_family != AF_INET) return 0;
    return ntohl(sa.sin_addr.s_addr);
}

unsigned int client_ip_key(const Req *r, const Conn *conn) {
    if (r->client_ip[0]) return (unsigned int)hash_str(r->client_ip);
    return conn_peer_ip(conn);
}

// --------------------------------------------------------------------------
// Request parsing (hand-rolled; swap for mongoose in a real deployment)
// --------------------------------------------------------------------------

// Finds the blank line ending the header block ("\r\n\r\n") by a plain byte
// scan instead of strstr (PROFILE_HOTPATH.md T1 report item 3: strstr's
// two-way-search internals showed up as real cost even for this 4-byte
// literal). Returns a pointer just PAST the blank line, or NULL if not seen
// yet in the bytes read so far.
static char *find_headers_end(char *buf, int n) {
    for (int i = 0; i + 3 < n; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i + 4;
    return NULL;
}

// Hand-rolled request-line + header parser - replaces both
// `sscanf(buf, "%7s %255s", method, path)` (2.1% of all instructions under
// load, __vfscanf_internal) and the `strcasestr` header scan (content-length:
// + authorization:, another ~3% combined) with one single pass over the
// header block using strchr-style manual scanning and fixed-width
// `strncasecmp` on each line's already-located header name. Same semantics:
// method/path/query split on the request line, Content-Length, a Bearer
// token, and whatever the WebSocket upgrade needs.
static void parse_request_line_and_headers(char *buf, char *hdr_end, Req *r) {
    memset(r, 0, sizeof *r);
    char *p = buf;

    char *s = p;
    while (p < hdr_end && *p != ' ' && *p != '\r' && *p != '\n') p++;
    int len = (int)(p - s); if (len > (int)sizeof r->method - 1) len = (int)sizeof r->method - 1;
    memcpy(r->method, s, (size_t)len); r->method[len] = 0;
    if (p < hdr_end && *p == ' ') p++;

    s = p;
    while (p < hdr_end && *p != ' ' && *p != '?' && *p != '\r' && *p != '\n') p++;
    len = (int)(p - s); if (len > (int)sizeof r->path - 1) len = (int)sizeof r->path - 1;
    memcpy(r->path, s, (size_t)len); r->path[len] = 0;
    if (p < hdr_end && *p == '?') {
        p++; s = p;
        while (p < hdr_end && *p != ' ' && *p != '\r' && *p != '\n') p++;
        len = (int)(p - s); if (len > (int)sizeof r->query - 1) len = (int)sizeof r->query - 1;
        memcpy(r->query, s, (size_t)len); r->query[len] = 0;
    }
    // Skip the rest of the request line (HTTP version) up to its CRLF.
    while (p < hdr_end && *p != '\n') p++;
    if (p < hdr_end) p++;

    bool saw_upgrade_websocket = false, saw_connection_upgrade = false;
    while (p < hdr_end) {
        char *line_start = p;
        char *colon = NULL;
        while (p < hdr_end && *p != '\r' && *p != '\n') { if (!colon && *p == ':') colon = p; p++; }
        char *line_end = p;
        if (p < hdr_end && *p == '\r') p++;
        if (p < hdr_end && *p == '\n') p++;
        if (!colon || line_start == colon) continue;   // blank/malformed line - skip

        int name_len = (int)(colon - line_start);
        const char *val = colon + 1;
        while (val < line_end && (*val == ' ' || *val == '\t')) val++;
        int val_len = (int)(line_end - val);

        if (name_len == 14 && strncasecmp(line_start, "content-length", 14) == 0) {
            int cl = 0;
            for (const char *q = val; q < line_end && *q >= '0' && *q <= '9'; q++) cl = cl * 10 + (*q - '0');
            r->content_length = cl;
        } else if (name_len == 13 && strncasecmp(line_start, "authorization", 13) == 0) {
            if (val_len > 7 && strncasecmp(val, "Bearer ", 7) == 0) {
                const char *tok = val + 7;
                int tlen = val_len - 7; if (tlen > (int)sizeof r->token - 1) tlen = (int)sizeof r->token - 1;
                memcpy(r->token, tok, (size_t)tlen); r->token[tlen] = 0;
            }
        } else if (name_len == 7 && strncasecmp(line_start, "upgrade", 7) == 0) {
            for (int i = 0; i + 9 <= val_len; i++)
                if (strncasecmp(val + i, "websocket", 9) == 0) { saw_upgrade_websocket = true; break; }
        } else if (name_len == 10 && strncasecmp(line_start, "connection", 10) == 0) {
            for (int i = 0; i + 7 <= val_len; i++)
                if (strncasecmp(val + i, "upgrade", 7) == 0) { saw_connection_upgrade = true; break; }
        } else if (name_len == 17 && strncasecmp(line_start, "sec-websocket-key", 17) == 0) {
            int klen = val_len; if (klen > (int)sizeof r->ws_key - 1) klen = (int)sizeof r->ws_key - 1;
            memcpy(r->ws_key, val, (size_t)klen); r->ws_key[klen] = 0;
        } else if (name_len == 13 && strncasecmp(line_start, "fly-client-ip", 13) == 0) {
            // Fly's real client IP (authoritative behind the edge proxy).
            int n = val_len; if (n > (int)sizeof r->client_ip - 1) n = (int)sizeof r->client_ip - 1;
            memcpy(r->client_ip, val, (size_t)n); r->client_ip[n] = 0;
        } else if (name_len == 15 && strncasecmp(line_start, "x-forwarded-for", 15) == 0) {
            // First hop = original client; only if Fly-Client-IP didn't already set it.
            if (!r->client_ip[0]) {
                const char *e = val; while (e < line_end && *e != ',' && *e != ' ') e++;
                int n = (int)(e - val); if (n > (int)sizeof r->client_ip - 1) n = (int)sizeof r->client_ip - 1;
                memcpy(r->client_ip, val, (size_t)n); r->client_ip[n] = 0;
            }
        }
    }
    r->is_ws_upgrade = saw_upgrade_websocket && saw_connection_upgrade && r->ws_key[0];
}

bool read_and_parse_request(Conn *conn, char *buf, int cap, Req *r) {
    int total = 0, n;
    char *hdr_end = NULL;
    while ((n = (int)io_read(conn, buf + total, (size_t)(cap - 1 - total))) > 0) {
        total += n; buf[total] = 0;
        hdr_end = find_headers_end(buf, total);
        if (!hdr_end) continue;
        break;
    }
    if (!hdr_end) return false;   // malformed / empty request

    parse_request_line_and_headers(buf, hdr_end, r);
    // Keep reading if the body (by Content-Length) hasn't fully arrived yet.
    int have = total - (int)(hdr_end - buf);
    while (have < r->content_length && (n = (int)io_read(conn, buf + total, (size_t)(cap - 1 - total))) > 0) {
        total += n; buf[total] = 0;
        have = total - (int)(hdr_end - buf);
    }
    r->body = hdr_end;
    r->body_len = total - (int)(hdr_end - buf);   // real byte count (the body is always binary: a packed control frame or an awire move)
    return true;
}

// --------------------------------------------------------------------------
// Responses
// --------------------------------------------------------------------------

static const char *reason_phrase(int code) {
    return code == 200 ? "OK" : code == 400 ? "Bad Request"
         : code == 401 ? "Unauthorized" : code == 404 ? "Not Found"
         : code == 429 ? "Too Many Requests" : "Error";
}

void respond_ctl(Conn *conn, int code, const unsigned char *frame, int len) {
    if (len < 0) len = 0;
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: application/octet-stream\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: *\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n",
        code, reason_phrase(code), len);
    io_write(conn, hdr, n);
    if (len > 0) io_write(conn, frame, (size_t)len);
}

void respond_ctl_error(Conn *conn, int code, int reason) {
    unsigned char frame[CTL_FRAME_MAX];
    int n = ctl_enc_error(reason, frame, (int)sizeof frame);
    respond_ctl(conn, code, frame, n);
}

void respond_bin(Conn *conn, int code, const unsigned char *data, int len) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d OK\r\nContent-Type: application/octet-stream\r\n"
        "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        code, len);
    io_write(conn, hdr, n);
    if (len > 0) io_write(conn, data, (size_t)len);
}

void respond_text(Conn *conn, int code, const char *body, int len) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d OK\r\nContent-Type: text/plain; version=0.0.4\r\n"
        "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        code, len);
    io_write(conn, hdr, n);
    if (len > 0) io_write(conn, body, (size_t)len);
}

// A bodyless "yes, I'm here" - OPTIONS preflight and GET /health. One empty
// CTL_HEALTH frame, so even the liveness probe speaks the same wire as
// everything else.
static void respond_health(Conn *conn) {
    unsigned char frame[CTL_FRAME_MAX];
    int n = ctl_enc_health(frame, (int)sizeof frame);
    respond_ctl(conn, 200, frame, n);
}

// --------------------------------------------------------------------------
// The route table
// --------------------------------------------------------------------------

void route(Req *r, Conn *conn) {
    if (!strcmp(r->method, "OPTIONS")) { respond_health(conn); return; }
    if (!strcmp(r->path, "/health")) { respond_health(conn); return; }
    if (!strcmp(r->path, "/auth/signup") || !strcmp(r->path, "/auth/signin")) { h_signup(r, conn); return; }
    if (!strcmp(r->path, "/create")) { h_create(r, conn); return; }
    if (!strcmp(r->path, "/meta"))   { h_meta(r, conn); return; }
    if (!strcmp(r->path, "/action")) { h_action(r, conn); return; }
    if (!strcmp(r->path, "/state"))  { h_state(r, conn); return; }
    if (!strcmp(r->path, "/status")) { h_status(r, conn); return; }
    if (!strcmp(r->path, "/stats"))  { h_stats(r, conn); return; }
    if (!strcmp(r->path, "/metrics")) { h_metrics(r, conn); return; }
    respond_ctl_error(conn, 404, CTL_ERR_ROUTE);
}

// --------------------------------------------------------------------------
// Work-queue thread routing (T2a Deliverable 2). Replaces thread-per-
// connection for every one-shot HTTP endpoint with a dispatcher (the accept
// loop, below) + a small number of typed worker pools, each pulling off its
// own bounded queue:
//   - /auth/*, /create  -> g_auth_create_q (g_n_create_workers threads)
//   - /meta             -> g_meta_q        (g_n_meta_workers threads - see
//                           below for the g_n_meta_workers==0 "fold into the
//                           game pool" mode)
//   - /action,/state,/status -> g_game_q[hash(game_id) % g_n_game_workers]
// Sharding the game queues by game_id means every request for a given game
// lands on the SAME worker thread, so requests for one game are serialized
// by construction; the per-game lock (GameSlot.lock) then only has to
// arbitrate against that game's bot_thread and its /ws dedicated thread(s),
// not against a pile of other HTTP workers for the same game. Multiple
// worker threads may safely share ONE WorkQueue (wq_pop is mutex-guarded,
// any number of consumers), which is how g_n_create_workers/g_n_meta_workers
// > 1 works below - auth/create and meta requests carry no game_id to shard
// by, so widening those pools just adds more consumers of the same queue.
//
// All three counts (game/meta/create) are runtime-configurable
// (--game-workers=N --meta-workers=N --create-workers=N) and were tuned
// EMPIRICALLY on this 4-core box with the WS+legal hammer, not guessed -
// see SERVER_SCALING.md "T2a Deliverable 2 - worker-pool sweep" for the
// sweep table across connection counts and the defaults it settled on.
// g_n_meta_workers may be 0: /meta is low-frequency (lobby join/start/
// continue, not the hot per-move loop) and every branch it runs already
// goes through the same registry->game lock handoff as /action, so folding
// it onto the game pool (sharded by game_id, same as /action) instead of
// paying for a dedicated idle thread is a real point on the sweep, not just
// a degenerate case - the table says whether it actually wins.
//
// Stage 6 (plaintext) replaces the meta/game pools with the epoll shards
// (shard.h); they stay fully intact here, unused in that mode, purely for the
// --tls fallback.
// --------------------------------------------------------------------------

#define WQ_CAP 512

typedef struct {
    Conn conn;
    Req req;
    char *raw_buf;   // owns the bytes req.body/req.query/etc point into until freed
} WorkItem;

typedef struct {
    WorkItem *buf;    // ring buffer, WQ_CAP entries
    int head, tail, count;
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} WorkQueue;

static void wq_init(WorkQueue *q) {
    q->buf = calloc(WQ_CAP, sizeof(WorkItem));
    q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

// Blocking push (backpressure): if a pool's queue is full, the dispatcher
// waits rather than dropping the connection or growing unboundedly. Fine
// for this POC's bounded load; a production version would size WQ_CAP for
// the target burst or shed load with a 503 instead of blocking the accept
// loop.
static void wq_push(WorkQueue *q, const WorkItem *item) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == WQ_CAP) pthread_cond_wait(&q->not_full, &q->mtx);
    q->buf[q->tail] = *item;
    q->tail = (q->tail + 1) % WQ_CAP;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

static void wq_pop(WorkQueue *q, WorkItem *out) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0) pthread_cond_wait(&q->not_empty, &q->mtx);
    *out = q->buf[q->head];
    q->head = (q->head + 1) % WQ_CAP;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
}

static WorkQueue g_auth_create_q;
static WorkQueue g_meta_q;
static WorkQueue g_game_q[MAX_GAME_WORKERS];

// Picks which pool's queue a one-shot request belongs on. Called by the
// dispatcher only for requests it hasn't already answered inline (OPTIONS,
// /health) or handed to a dedicated thread (/ws) - see acceptor_main.
static WorkQueue *classify_queue(Req *r) {
    if (!strcmp(r->path, "/auth/signup") || !strcmp(r->path, "/auth/signin") || !strcmp(r->path, "/create"))
        return &g_auth_create_q;
    bool is_meta = !strcmp(r->path, "/meta");
    bool is_game = !strcmp(r->path, "/action") || !strcmp(r->path, "/state") || !strcmp(r->path, "/status");
    if (is_meta && g_n_meta_workers > 0) return &g_meta_q;
    if (is_game || is_meta) {   // is_meta here implies g_n_meta_workers==0 - fold onto the game pool
        char gid[ID_LEN + 1] = {0};
        query_game_id(r->query, gid);
        unsigned long h = gid[0] ? hash_str(gid) : 0;
        return &g_game_q[h % (unsigned long)g_n_game_workers];
    }
    // unrecognized route -> route() 404s it; any pool can carry it.
    return g_n_meta_workers > 0 ? &g_meta_q : &g_game_q[0];
}

static void *worker_thread(void *arg) {
    thread_disable_cancellation();
    WorkQueue *q = arg;
    for (;;) {
        WorkItem item;
        wq_pop(q, &item);
        route(&item.req, &item.conn);
        conn_close(&item.conn);
        free(item.raw_buf);
    }
    return NULL;
}

void httpd_queues_init(void) {
    wq_init(&g_auth_create_q);
    wq_init(&g_meta_q);
    for (int i = 0; i < g_n_game_workers; i++) wq_init(&g_game_q[i]);
}

void httpd_start_create_workers(void) {
    // /auth/* and /create stay on their own threaded worker pool in BOTH
    // modes (task-granted: "the dispatcher and create/auth worker can stay
    // threads") - no game_id to shard by, never the hot path (see
    // SERVER_SCALING.md's Deliverable 2 sweep - neither pool was ever close
    // to contended).
    pthread_t wt;
    for (int i = 0; i < g_n_create_workers; i++)
        if (pthread_create(&wt, NULL, worker_thread, &g_auth_create_q) == 0) pthread_detach(wt);
}

void httpd_start_tls_fallback_workers(void) {
    // Stage 6 TLS fallback: non-blocking OpenSSL's WANT_READ/WANT_WRITE
    // state machine is out of that stage's budget (see SERVER_SCALING.md
    // "Stage 6" for the honest writeup) - a --tls server keeps the ENTIRE
    // pre-Stage-6 design, byte-for-byte: thread-per-/ws-connection + these
    // typed HTTP work-queue pools.
    pthread_t wt;
    for (int i = 0; i < g_n_meta_workers; i++)
        if (pthread_create(&wt, NULL, worker_thread, &g_meta_q) == 0) pthread_detach(wt);
    for (int i = 0; i < g_n_game_workers; i++)
        if (pthread_create(&wt, NULL, worker_thread, &g_game_q[i]) == 0) pthread_detach(wt);
}

// --------------------------------------------------------------------------
// Connection handling - the acceptors (T2a Deliverable 2)
//
// SO_REUSEPORT multi-acceptor (see SERVER_SCALING.md). Instead of one
// dispatcher thread owning the single listening socket, run N acceptor
// threads, each with its OWN listener bound to the same port with
// SO_REUSEPORT. The kernel spreads inbound connections across the listeners
// by 4-tuple hash, so accept() + the TLS handshake + request parsing all
// parallelize and no single thread is the connection-arrival bottleneck (the
// old single dispatcher serialized every SSL_accept). Game-affinity is
// unchanged: each acceptor still hands the parsed connection to the epoll
// worker that owns its game (worker_handoff_push), exactly as before.
// --accept-threads=N; default 2 (a safe win over 1 without oversubscribing a
// small box, since idle acceptors just block in accept()).
// --------------------------------------------------------------------------

int make_listener(int port, bool reuseport) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    if (reuseport && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof opt) < 0)
        perror("setsockopt(SO_REUSEPORT)");   // non-fatal: a single listener still works
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); close(fd); return -1; }
    if (listen(fd, 1024) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

void *acceptor_main(void *arg) {
    int srv = (int)(intptr_t)arg;
    thread_disable_cancellation();   // per-thread cancel bracket; each acceptor pays it once
    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        // Admission control: at the connection ceiling, shed the new one
        // immediately (a closed fd, not an OOM). The OS listen backlog already
        // absorbs bursts; this is the app-level bound that keeps a connection
        // flood from exhausting memory faster than the fd limit alone would.
        if (g_max_conns > 0 && atomic_load_explicit(&g_live_conns, memory_order_relaxed) >= g_max_conns) {
            close(fd);
            continue;
        }
        // A one-shot HTTP request/response barely notices Nagle's algorithm,
        // but a persistent /ws connection does many small back-and-forth
        // writes+reads - without TCP_NODELAY, Nagle batching interacting
        // with the peer's delayed ACKs turns each round trip into tens of
        // milliseconds instead of tens of microseconds.
        int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        // Slowloris guard: a read/write deadline on the connection while it is
        // still blocking here (the TLS handshake + the request read). A client
        // that opens a socket and dribbles or never finishes its request would
        // otherwise pin this acceptor thread forever - and with only a handful
        // of acceptors, a handful of such connections is a full DoS. Generous
        // enough (default 30s, --io-timeout-s) that a real request never trips
        // it. Moot once the fd is flipped non-blocking for the epoll worker
        // (SO_RCVTIMEO doesn't apply to non-blocking reads), so an established,
        // idle /ws connection is unaffected.
        if (g_io_timeout_s > 0) {
            struct timeval tv = { .tv_sec = g_io_timeout_s, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        }

        // Stage 3: SSL_accept happens HERE, on this connection's own
        // servicing path, before any HTTP parsing - a fresh SSL* per
        // connection, off the one shared g_tls_ctx (see its doc in httpd.h). A
        // failed/abandoned handshake (a port scanner, a plaintext probe
        // against a TLS listener, a client that hangs up mid-handshake) is
        // just a dropped connection, same as a malformed plaintext request
        // below - never fatal to the process.
        Conn conn;
        if (g_tls_ctx) {
            if (!conn_tls_accept(&conn, g_tls_ctx, fd)) { close(fd); continue; }
        } else {
            conn_init_plain(&conn, fd);
        }

        char *buf = malloc(REQ_BUF_CAP);
        if (!buf) { conn_close(&conn); continue; }
        Req r;
        if (!read_and_parse_request(&conn, buf, REQ_BUF_CAP, &r)) { conn_close(&conn); free(buf); continue; }

        if (r.is_ws_upgrade && !strcmp(r.method, "GET") && !strcmp(r.path, "/ws")) {
            if (g_tls_ctx) {
                // Stage 6 TLS fallback: dedicated per-connection thread
                // (design B, see wsconn.h) - this thread lives for the
                // client's whole session, so the pthread_create cost is paid
                // once per client instead of once per action. See
                // PROFILE_HOTPATH.md T1b.
                WsSpawnArg *sa = malloc(sizeof *sa);
                if (!sa) { conn_close(&conn); free(buf); continue; }
                sa->conn = conn; sa->req = r; sa->raw_buf = buf;
                pthread_t t;
                if (pthread_create(&t, NULL, ws_conn_thread, sa) == 0) pthread_detach(t);
                else { conn_close(&conn); free(buf); free(sa); }
            } else {
                // Stage 6 plaintext: no thread - hands the now-upgraded
                // connection to its shard's epoll worker. See
                // epoll_dispatch_ws's doc.
                epoll_dispatch_ws(&conn, &r, buf);
            }
            continue;
        }

        // Cheap, store-free routes: answer inline instead of paying a queue
        // round trip for them, in EITHER mode (no game_id, never worth
        // sharding to an epoll worker just to flush a few bytes).
        if (!strcmp(r.method, "OPTIONS")) { respond_health(&conn); conn_close(&conn); free(buf); continue; }
        if (!strcmp(r.path, "/health"))  { respond_health(&conn); conn_close(&conn); free(buf); continue; }
        if (!strcmp(r.path, "/stats"))   { h_stats(&r, &conn); conn_close(&conn); free(buf); continue; }
        if (!strcmp(r.path, "/metrics")) { h_metrics(&r, &conn); conn_close(&conn); free(buf); continue; }

        if (g_tls_ctx) {
            // Stage 6 TLS fallback: unchanged typed HTTP work-queue pools.
            WorkItem item; item.conn = conn; item.req = r; item.raw_buf = buf;
            wq_push(classify_queue(&r), &item);
            continue;
        }
        // Stage 6 plaintext: /auth/signup, /auth/signin, /create have no
        // game_id to shard by - stay on the threaded create/auth pool
        // (started above, both modes). Everything else here
        // (/action,/state,/status,/meta) is game_id-sharded - hand it to
        // its shard's epoll worker instead of the (now TLS-only) typed
        // game/meta queues.
        if (!strcmp(r.path, "/auth/signup") || !strcmp(r.path, "/auth/signin") || !strcmp(r.path, "/create")) {
            WorkItem item; item.conn = conn; item.req = r; item.raw_buf = buf;
            wq_push(&g_auth_create_q, &item);
        } else {
            epoll_dispatch_oneshot(&conn, &r, buf);
        }
    }
    return NULL;   // unreachable - the accept loop above never terminates
}
