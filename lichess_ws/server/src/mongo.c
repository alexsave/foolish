// mongo.c - mongo.h. OP_MSG (opcode 2013): a 16-byte header, 4 flag bytes, one section of
// kind 0 holding the command document.
#include "mongo.h"

#include "util/err.h"
#include "util/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MOD "mongo"
#define OP_MSG 2013

struct mongo_conn {
    int       fd;
    int32_t   req_id;
    int       timeout_ms;
    char      db[64];
    cb_buf    out, in;
};

static uint32_t get_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void app_le32(cb_buf *b, uint32_t v) { uint8_t p[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; cb_buf_append(b, p, 4); }
static void put_le32(cb_buf *b, size_t at, uint32_t v) { b->p[at] = (uint8_t)v; b->p[at + 1] = (uint8_t)(v >> 8); b->p[at + 2] = (uint8_t)(v >> 16); b->p[at + 3] = (uint8_t)(v >> 24); }

static int connect_to(const char *host, int port, int timeout_ms) {
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            if (poll(&p, 1, timeout_ms) == 1) {
                int err = 0; socklen_t el = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                rc = err ? -1 : 0;
            } else rc = -1;
        }
        if (rc == 0) {
            fcntl(fd, F_SETFL, fl);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int mongo_connect(mongo_conn **out, const mongo_cfg *cfg) {
    int fd = connect_to(cfg->host, cfg->port, cfg->timeout_ms);
    if (fd < 0) { CB_WARN(MOD, "cannot connect to %s:%d: %s", cfg->host, cfg->port, strerror(errno)); return CB_E_IO; }
    struct timeval tv = { .tv_sec = cfg->timeout_ms / 1000, .tv_usec = (cfg->timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    mongo_conn *c = calloc(1, sizeof *c);
    if (!c) { close(fd); return CB_E_NOMEM; }
    c->fd = fd;
    c->timeout_ms = cfg->timeout_ms;
    cb_strlcpy(c->db, cfg->db, sizeof c->db);
    cb_buf_init(&c->out);
    cb_buf_init(&c->in);
    *out = c;
    return CB_OK;
}

void mongo_close(mongo_conn *c) {
    if (!c) return;
    close(c->fd);
    cb_buf_free(&c->out);
    cb_buf_free(&c->in);
    free(c);
}

static int send_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        p += k; n -= (size_t)k;
    }
    return 0;
}

static int recv_exact(int fd, cb_buf *b, size_t n) {
    if (!cb_buf_reserve(b, n)) return -1;
    while (b->len < n) {
        ssize_t k = recv(fd, b->p + b->len, n - b->len, 0);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        if (k == 0) return -1;
        b->len += (size_t)k;
        b->p[b->len] = 0;
    }
    return 0;
}

// Starts an OP_MSG with a command document open in w (the caller adds the command's fields and
// calls bson_w_end), then exchange() sends it and reads the reply document into *reply.
static void command_begin(mongo_conn *c, bson_w *w, const char *name, const char *coll) {
    cb_buf_reset(&c->out);
    cb_buf *o = &c->out;
    app_le32(o, 0);                 // messageLength, patched
    app_le32(o, (uint32_t)++c->req_id);
    app_le32(o, 0);                 // responseTo
    app_le32(o, OP_MSG);
    app_le32(o, 0);                 // flagBits
    cb_buf_putc(o, 0);              // section kind 0
    bson_w_init(w, o);
    bson_w_begin(w);
    bson_w_str(w, name, coll);
}

static int exchange(mongo_conn *c, bson_w *w, bson_doc *reply) {
    bson_w_str(w, "$db", c->db);
    bson_w_end(w);
    if (!bson_w_ok(w)) return CB_E_NOMEM;
    cb_buf *o = &c->out;
    put_le32(o, 0, (uint32_t)o->len);
    if (send_all(c->fd, o->p, o->len) != 0) return CB_E_IO;
    cb_buf_reset(&c->in);
    if (recv_exact(c->fd, &c->in, 16) != 0) return CB_E_IO;
    uint32_t len = get_le32(c->in.p);
    if (len < 21 || len > (16u << 20)) return CB_E_IO;
    if (recv_exact(c->fd, &c->in, len) != 0) return CB_E_IO;
    if (get_le32(c->in.p + 12) != OP_MSG || (int32_t)get_le32(c->in.p + 8) != c->req_id) return CB_E_IO;
    if (c->in.p[20] != 0) return CB_E_IO;   // a section kind we do not speak
    if (!bson_doc_init(reply, c->in.p + 21, len - 21)) return CB_E_IO;
    bson_el el;
    int64_t ok = 0;
    if (!bson_find(reply, "ok", &el) || !bson_el_int(&el, &ok) || ok != 1) {
        const char *msg = "?"; size_t n;
        if (bson_find(reply, "errmsg", &el)) bson_el_str(&el, &msg, &n);
        CB_WARN(MOD, "command failed: %s", msg);
        return CB_E_IO;
    }
    return CB_OK;
}

int mongo_find_one(mongo_conn *c, const char *coll, const cb_buf *filter, const cb_buf *proj, cb_buf *doc) {
    bson_w w;
    command_begin(c, &w, "find", coll);
    cb_buf *o = &c->out;
    cb_buf_putc(o, BSON_DOC); cb_buf_append(o, "filter", 7);
    cb_buf_append(o, filter->p, filter->len);
    if (proj) {
        cb_buf_putc(o, BSON_DOC); cb_buf_append(o, "projection", 11);
        cb_buf_append(o, proj->p, proj->len);
    }
    bson_w_int32(&w, "limit", 1);
    bson_w_bool(&w, "singleBatch", true);
    bson_doc reply, cursor, batch;
    int rc = exchange(c, &w, &reply);
    if (rc != CB_OK) return rc;
    bson_el el;
    if (!bson_find(&reply, "cursor", &el) || !bson_el_doc(&el, &cursor)) return CB_E_IO;
    if (!bson_find(&cursor, "firstBatch", &el) || !bson_el_doc(&el, &batch)) return CB_E_IO;
    size_t it = 0;
    if (!bson_next(&batch, &it, &el) || el.type != BSON_DOC) return CB_E_NOT_FOUND;
    cb_buf_reset(doc);
    cb_buf_append(doc, el.val, el.vlen);
    return doc->oom ? CB_E_NOMEM : CB_OK;
}

int mongo_insert_one(mongo_conn *c, const char *coll, const cb_buf *doc) {
    bson_w w;
    command_begin(c, &w, "insert", coll);
    cb_buf *o = &c->out;
    bson_w_array(&w, "documents");
    cb_buf_putc(o, BSON_DOC); cb_buf_append(o, "0", 2);
    cb_buf_append(o, doc->p, doc->len);
    bson_w_end(&w);
    bson_doc reply, errs;
    int rc = exchange(c, &w, &reply);
    if (rc != CB_OK) return rc;
    bson_el el;
    if (bson_find(&reply, "writeErrors", &el) && bson_el_doc(&el, &errs)) {
        size_t it = 0;
        bson_doc e;
        if (bson_next(&errs, &it, &el) && bson_el_doc(&el, &e)) {
            int64_t code = 0;
            if (bson_find(&e, "code", &el) && bson_el_int(&el, &code) && code == 11000) return CB_E_CONFLICT;
            return CB_E_IO;
        }
    }
    return CB_OK;
}

// ---- the thread's connection ----

static mongo_cfg g_cfg;
static bool g_cfg_set;
static __thread mongo_conn *t_conn;

void mongo_thread_configure(const mongo_cfg *cfg) { g_cfg = *cfg; g_cfg_set = true; }

mongo_conn *mongo_thread_conn(void) {
    if (t_conn) return t_conn;
    if (!g_cfg_set) return NULL;
    mongo_conn *c = NULL;
    if (mongo_connect(&c, &g_cfg) != CB_OK) return NULL;
    t_conn = c;
    return c;
}

void mongo_thread_drop(void) {
    mongo_close(t_conn);
    t_conn = NULL;
}
