// client.c - client.h.
#include "client.h"

#include "be_metrics.h"
#include "lib_client.h"
#include "net/ws.h"
#include "util/clock.h"
#include "util/log.h"
#include "../../../shared/c/stream_frame.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define IN_CAP_START (16 * 1024)
#define IN_CAP_MAX   CB_WS_MAX_MESSAGE

static atomic_uint g_sockets;

unsigned be_client_sockets(void) { return atomic_load(&g_sockets); }

bool be_client_init(be_client *c, const char *name, const char *password) {
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->password, sizeof c->password, "%s", password);
    c->ws = -1;
    c->in_cap = IN_CAP_START;
    c->in = malloc(c->in_cap);
    return c->in != NULL;
}

void be_client_free(be_client *c) {
    be_client_close(c);
    free(c->in);
    c->in = NULL;
}

static void send_bytes(be_client *c, const void *bytes, size_t n) {
    if (c->ws < 0) return;
    if (tc_ws_send(c->ws, CB_WS_OP_BIN, bytes, n) != 0) be_client_close(c);
}

bool be_client_sign_in(be_client *c, int port) {
    for (int attempt = 0; attempt < 2; attempt++) {
        NwRequest q;
        memset(&q, 0, sizeof q);
        q.kind = attempt == 0 ? NW_SIGNUP : NW_SIGNIN;
        q.name_len = (uint8_t)strlen(c->name);
        memcpy(q.name, c->name, q.name_len);
        q.password_len = (uint8_t)strlen(c->password);
        memcpy(q.password, c->password, q.password_len);
        uint8_t body[256];
        int n = nw_request_encode(&q, body, sizeof body);
        if (n <= 0) return false;
        tc_response r;
        int rc = tc_http(port, "POST", attempt == 0 ? "/v1/auth/signup" : "/v1/auth/signin",
                         "Content-Type: application/octet-stream\r\n", body, (size_t)n, &r);
        NwSession ses;
        bool ok = rc == 0 && r.status == 200 && nw_session_decode(r.body.p, (int)r.body.len, &ses) == NW_OK;
        int status = rc == 0 ? r.status : -1;
        tc_response_free(&r);
        if (ok) {
            snprintf(c->token, sizeof c->token, "%.*s", (int)ses.access_len, ses.access);
            snprintf(c->uid, sizeof c->uid, "%.*s", (int)ses.user_id_len, ses.user_id);
            return true;
        }
        if (attempt == 0 && status == 409) continue;   // taken: sign in
        CB_WARN("birdseye", "%s: %s answered %d", c->name, attempt == 0 ? "signup" : "signin", status);
        if (status == 429) {
            struct timespec ts = { 1, 0 };
            nanosleep(&ts, NULL);
        }
        return false;
    }
    return false;
}

bool be_client_open(be_client *c, int port) {
    c->ws = tc_ws_open(port, "/ws", NULL);
    if (c->ws < 0) return false;
    atomic_fetch_add(&g_sockets, 1);
    c->in_len = 0;
    c->n_pend = 0;
    uint8_t frame[SF_TOKEN_MAX + 4];
    int n = sf_put_hello(frame, sizeof frame, c->token, strlen(c->token));
    if (n < 0) {
        be_client_close(c);
        return false;
    }
    send_bytes(c, frame, (size_t)n);
    c->next_ping_ms = cb_mono_ms() + BE_PING_MS;
    return c->ws >= 0;
}

void be_client_close(be_client *c) {
    if (c->ws >= 0) {
        close(c->ws);
        c->ws = -1;
        atomic_fetch_sub(&g_sockets, 1);
    }
    c->n_pend = 0;
}

static be_pending *pend_add(be_client *c, int kind, uint8_t hostile, uint32_t intent) {
    if (c->n_pend >= BE_PEND_MAX) {
        // The oldest is forgotten: its reply, if it ever comes, is counted as unknown.
        memmove(c->pend, c->pend + 1, (size_t)(BE_PEND_MAX - 1) * sizeof c->pend[0]);
        c->n_pend = BE_PEND_MAX - 1;
    }
    be_pending *q = &c->pend[c->n_pend++];
    q->id = ++c->req_seq;
    q->kind = (uint8_t)kind;
    q->hostile = hostile;
    q->intent = intent;
    q->t0_us = cb_mono_us();
    return q;
}

bool be_client_req(be_client *c, int kind, int meth, const char *path, const uint8_t *body, size_t n, uint8_t hostile,
                   uint32_t intent) {
    if (c->ws < 0) return false;
    be_pending *q = pend_add(c, kind, hostile, intent);
    uint8_t frame[SF_PATH_MAX + 9 + 512];
    int len = sf_put_req(frame, sizeof frame, q->id, (uint8_t)meth, path, strlen(path), body, n);
    if (len < 0) {
        c->n_pend--;
        return false;
    }
    send_bytes(c, frame, (size_t)len);
    cb_metrics_rate(be_mx, BE_M_REQS, cb_now_ms(), 1);
    return c->ws >= 0;
}

void be_client_sub(be_client *c, const char *gid, int gid_len, bool unsub) {
    uint8_t frame[SF_GID_MAX + 8];
    int n = sf_put_sub(frame, sizeof frame, unsub ? SF_UNSUB : SF_SUB, ++c->req_seq, NW_SUB_GAME, gid, (size_t)gid_len);
    if (n > 0) send_bytes(c, frame, (size_t)n);
}

bool be_client_take(be_client *c, uint32_t id, be_pending *out) {
    for (int i = 0; i < c->n_pend; i++)
        if (c->pend[i].id == id) {
            *out = c->pend[i];
            memmove(c->pend + i, c->pend + i + 1, (size_t)(c->n_pend - i - 1) * sizeof c->pend[0]);
            c->n_pend--;
            return true;
        }
    return false;
}

void be_client_ping(be_client *c, int64_t now_ms) {
    if (c->ws < 0 || now_ms < c->next_ping_ms) return;
    uint8_t pg[2];
    int n = sf_put_bare(pg, sizeof pg, SF_PING);
    if (n > 0) send_bytes(c, pg, (size_t)n);
    c->next_ping_ms = now_ms + BE_PING_MS;
}

void be_client_read(be_client *c, be_client_msg_fn fn, void *ud) {
    if (c->ws < 0) return;
    if (c->in_len == c->in_cap) {
        if (c->in_cap >= IN_CAP_MAX) {
            be_client_close(c);
            return;
        }
        size_t cap = c->in_cap * 2;
        uint8_t *b = realloc(c->in, cap);
        if (!b) {
            be_client_close(c);
            return;
        }
        c->in = b;
        c->in_cap = cap;
    }
    ssize_t r = recv(c->ws, c->in + c->in_len, c->in_cap - c->in_len, 0);
    if (r <= 0) {
        if (r < 0 && (errno == EINTR || errno == EAGAIN)) return;
        be_client_close(c);
        return;
    }
    c->in_len += (size_t)r;
    size_t off = 0;
    for (;;) {
        cb_ws_frame f;
        int n = cb_ws_frame_parse(c->in + off, c->in_len - off, IN_CAP_MAX, &f);
        if (n < 0) {
            be_client_close(c);
            return;
        }
        if (n == 0) break;
        uint8_t *pl = c->in + off + f.header_len;
        if (f.masked) cb_ws_unmask(pl, (size_t)f.len, f.mask);
        if (f.opcode == CB_WS_OP_PING) tc_ws_send(c->ws, CB_WS_OP_PONG, pl, (size_t)f.len);
        else if (f.opcode == CB_WS_OP_CLOSE) {
            be_client_close(c);
            return;
        } else if (f.opcode == CB_WS_OP_BIN) fn(ud, pl, (size_t)f.len);
        off += (size_t)n;
        if (c->ws < 0) return;
    }
    if (off) {
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
}
