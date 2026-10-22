// resp.c - resp.h.
#include "resp.h"

#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MOD "redis"
#define RECONNECT_MS 1000
#define READ_CHUNK 65536

typedef struct {
    resp_client *c;
    int     fd;            // -1: down
    bool    connecting;    // connect(2) in progress
    bool    up;            // connected
    cb_buf  out, in;
    bool    want_write;
    int     pending_subs;  // subscribe confirmations awaited (the subscribing connection)
} rconn;

struct resp_client {
    cb_loop      *loop;
    char          host[128];
    int           port;
    resp_msg_fn   on_msg;
    resp_state_fn on_state;
    void         *ud;
    rconn         sub, pub;
    char        **chans;
    int           n_chans, cap_chans;
    uint64_t      timer;
    bool          started, was_up;
    long          published, received;
};

static void conn_io(cb_loop *l, int fd, int events, void *ud);
static void schedule_reconnect(resp_client *c);

static void conn_init(rconn *r, resp_client *c) {
    r->c = c;
    r->fd = -1;
    cb_buf_init(&r->out);
    cb_buf_init(&r->in);
}

static void conn_down(rconn *r) {
    resp_client *c = r->c;
    if (r->fd >= 0) {
        cb_loop_remove(c->loop, r->fd);
        close(r->fd);
        r->fd = -1;
    }
    bool was = c->was_up;
    r->up = r->connecting = r->want_write = false;
    cb_buf_reset(&r->out);
    cb_buf_reset(&r->in);
    if (was) { c->was_up = false; c->on_state(false, c->ud); }
    schedule_reconnect(c);
}

static void conn_want(rconn *r) {
    if (r->fd < 0) return;
    int ev = CB_EV_READ | (r->out.len ? CB_EV_WRITE : 0);
    if (r->connecting) ev = CB_EV_WRITE;
    cb_loop_set(r->c->loop, r->fd, ev);
}

static bool conn_flush(rconn *r) {
    while (r->out.len) {
        ssize_t k = send(r->fd, r->out.p, r->out.len, MSG_DONTWAIT);
        if (k < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            CB_WARN(MOD, "send: %s", strerror(errno));
            conn_down(r);
            return false;
        }
        cb_buf_consume(&r->out, (size_t)k);
    }
    r->want_write = r->out.len > 0;   // the socket is full: the loop says when to go on
    conn_want(r);
    return true;
}

static void conn_send(rconn *r, const void *p, size_t n) {
    cb_buf_append(&r->out, p, n);
    if (r->up && !r->want_write) conn_flush(r);
}

static void bulk(cb_buf *b, const char *s, size_t n) {
    cb_buf_printf(b, "$%zu\r\n", n);
    cb_buf_append(b, s, n);
    cb_buf_append(b, "\r\n", 2);
}

static void send_subscribes(resp_client *c) {
    if (!c->n_chans) return;
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_printf(&b, "*%d\r\n", c->n_chans + 1);
    bulk(&b, "SUBSCRIBE", 9);
    for (int i = 0; i < c->n_chans; i++) bulk(&b, c->chans[i], strlen(c->chans[i]));
    c->sub.pending_subs = c->n_chans;
    conn_send(&c->sub, b.p, b.len);
    cb_buf_free(&b);
}

static void check_up(resp_client *c) {
    if (c->sub.up && c->pub.up && c->sub.pending_subs == 0 && !c->was_up) {
        c->was_up = true;
        c->on_state(true, c->ud);
    }
}

static int start_connect(rconn *r) {
    resp_client *c = r->c;
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", c->port);
    if (getaddrinfo(c->host, ps, &hints, &res) != 0 || !res) { CB_WARN(MOD, "cannot resolve %s", c->host); return -1; }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    r->fd = fd;
    r->connecting = rc < 0;
    r->up = rc == 0;
    if (cb_loop_add(c->loop, fd, r->connecting ? CB_EV_WRITE : CB_EV_READ, conn_io, r) != CB_OK) { close(fd); r->fd = -1; return -1; }
    return 0;
}

static void on_connected(rconn *r) {
    r->connecting = false;
    r->up = true;
    if (r == &r->c->sub) send_subscribes(r->c);
    conn_flush(r);
    check_up(r->c);
}

// ---- the parser: one RESP2 value at a time ----

// Parses one value at in[0..n). Returns bytes consumed, 0 when incomplete, -1 on a protocol
// error. For arrays of bulk strings (the pushes), fills parts[0..np) with (ptr, len); any other
// shape is consumed and ignored (the publishing connection's integer replies).
typedef struct { const char *p; size_t n; } rspan;
static long parse_value(const char *in, size_t n, rspan *parts, int cap, int *np);

static long find_crlf(const char *in, size_t n) {
    for (size_t i = 0; i + 1 < n; i++)
        if (in[i] == '\r' && in[i + 1] == '\n') return (long)i;
    return -1;
}

static long parse_value(const char *in, size_t n, rspan *parts, int cap, int *np) {
    if (n < 3) return 0;
    long eol = find_crlf(in, n);
    if (eol < 0) return n > 65536 ? -1 : 0;
    char t = in[0];
    if (t == '+' || t == '-' || t == ':') {
        if (t == '-') CB_WARN(MOD, "error reply: %.*s", (int)(eol - 1), in + 1);
        return eol + 2;
    }
    long len = strtol(in + 1, NULL, 10);
    if (t == '$') {
        if (len < 0) return eol + 2;   // null bulk
        size_t need = (size_t)eol + 2 + (size_t)len + 2;
        if (need > n) return 0;
        if (parts && *np < cap) { parts[*np].p = in + eol + 2; parts[*np].n = (size_t)len; (*np)++; }
        return (long)need;
    }
    if (t == '*') {
        size_t at = (size_t)eol + 2;
        for (long i = 0; i < len; i++) {
            long k = parse_value(in + at, n - at, parts, cap, np);
            if (k <= 0) return k;
            at += (size_t)k;
        }
        return (long)at;
    }
    return -1;
}

static void sub_dispatch(resp_client *c, rspan *parts, int np) {
    if (np == 3 && parts[0].n == 7 && memcmp(parts[0].p, "message", 7) == 0) {
        c->received++;
        char chan[128];
        size_t cl = parts[1].n < sizeof chan - 1 ? parts[1].n : sizeof chan - 1;
        memcpy(chan, parts[1].p, cl);
        chan[cl] = 0;
        c->on_msg(chan, cl, parts[2].p, parts[2].n, c->ud);
    } else if (np >= 2 && parts[0].n == 9 && memcmp(parts[0].p, "subscribe", 9) == 0) {
        if (c->sub.pending_subs > 0) c->sub.pending_subs--;
        check_up(c);
    }
}

static void conn_read(rconn *r) {
    resp_client *c = r->c;
    for (;;) {
        if (!cb_buf_reserve(&r->in, READ_CHUNK)) { conn_down(r); return; }
        ssize_t k = recv(r->fd, r->in.p + r->in.len, READ_CHUNK, MSG_DONTWAIT);
        if (k < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            CB_WARN(MOD, "recv: %s", strerror(errno));
            conn_down(r);
            return;
        }
        if (k == 0) { CB_WARN(MOD, "redis closed the %s connection", r == &c->sub ? "subscribing" : "publishing"); conn_down(r); return; }
        r->in.len += (size_t)k;
        r->in.p[r->in.len] = 0;
        if ((size_t)k < READ_CHUNK) break;
    }
    size_t at = 0;
    while (at < r->in.len) {
        rspan parts[8];
        int np = 0;
        long used = parse_value((const char *)r->in.p + at, r->in.len - at, parts, 8, &np);
        if (used < 0) { CB_WARN(MOD, "protocol error"); conn_down(r); return; }
        if (used == 0) break;
        if (r == &c->sub) sub_dispatch(c, parts, np);
        at += (size_t)used;
    }
    cb_buf_consume(&r->in, at);
}

static void conn_io(cb_loop *l, int fd, int events, void *ud) {
    rconn *r = ud;
    if (r->connecting) {
        int err = 0; socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err || (events & CB_EV_ERR)) { CB_WARN(MOD, "connect to %s:%d: %s", r->c->host, r->c->port, strerror(err ? err : ECONNREFUSED)); conn_down(r); return; }
        on_connected(r);
        return;
    }
    if (events & CB_EV_ERR) { conn_down(r); return; }
    if (events & CB_EV_READ) { conn_read(r); if (r->fd < 0) return; }
    if (events & CB_EV_WRITE) conn_flush(r);
}

static void reconnect_timer(cb_loop *l, void *ud) {
    resp_client *c = ud;
    c->timer = 0;
    if (c->sub.fd < 0 && start_connect(&c->sub) != 0) schedule_reconnect(c);
    if (c->pub.fd < 0 && start_connect(&c->pub) != 0) schedule_reconnect(c);
}

static void schedule_reconnect(resp_client *c) {
    if (!c->started || c->timer) return;
    c->timer = cb_loop_timer(c->loop, RECONNECT_MS, reconnect_timer, c);
}

resp_client *resp_new(cb_loop *l, const char *host, int port, resp_msg_fn on_msg, resp_state_fn on_state, void *ud) {
    resp_client *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->loop = l;
    cb_strlcpy(c->host, host, sizeof c->host);
    c->port = port;
    c->on_msg = on_msg;
    c->on_state = on_state;
    c->ud = ud;
    conn_init(&c->sub, c);
    conn_init(&c->pub, c);
    return c;
}

void resp_free(resp_client *c) {
    if (!c) return;
    c->started = false;
    if (c->timer) cb_loop_cancel(c->loop, c->timer);
    rconn *rs[2] = { &c->sub, &c->pub };
    for (int i = 0; i < 2; i++) {
        if (rs[i]->fd >= 0) { cb_loop_remove(c->loop, rs[i]->fd); close(rs[i]->fd); }
        cb_buf_free(&rs[i]->out);
        cb_buf_free(&rs[i]->in);
    }
    for (int i = 0; i < c->n_chans; i++) free(c->chans[i]);
    free(c->chans);
    free(c);
}

void resp_start(resp_client *c) {
    c->started = true;
    if (start_connect(&c->sub) != 0 || start_connect(&c->pub) != 0) schedule_reconnect(c);
    if (c->sub.up) on_connected(&c->sub);
    if (c->pub.up) on_connected(&c->pub);
}

int resp_subscribe(resp_client *c, const char *chan) {
    if (c->n_chans == c->cap_chans) {
        int ncap = c->cap_chans ? c->cap_chans * 2 : 8;
        char **nc = realloc(c->chans, (size_t)ncap * sizeof *nc);
        if (!nc) return CB_E_NOMEM;
        c->chans = nc;
        c->cap_chans = ncap;
    }
    if (!(c->chans[c->n_chans] = strdup(chan))) return CB_E_NOMEM;
    c->n_chans++;
    if (c->sub.up) {
        cb_buf b;
        cb_buf_init(&b);
        cb_buf_puts(&b, "*2\r\n");
        bulk(&b, "SUBSCRIBE", 9);
        bulk(&b, chan, strlen(chan));
        c->sub.pending_subs++;
        conn_send(&c->sub, b.p, b.len);
        cb_buf_free(&b);
    }
    return CB_OK;
}

int resp_publish(resp_client *c, const char *chan, const char *msg, size_t n) {
    if (!c->pub.up) return CB_E_STATE;
    cb_buf *o = &c->pub.out;
    cb_buf_puts(o, "*3\r\n$7\r\nPUBLISH\r\n");
    bulk(o, chan, strlen(chan));
    bulk(o, msg, n);
    c->published++;
    if (!c->pub.want_write) { if (!conn_flush(&c->pub)) return CB_E_STATE; }
    return CB_OK;
}

bool resp_up(const resp_client *c) { return c->was_up; }
void resp_stats(const resp_client *c, long *published, long *received) { *published = c->published; *received = c->received; }
