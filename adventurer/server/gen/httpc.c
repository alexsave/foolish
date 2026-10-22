// httpc.c - httpc.h.
#include "httpc.h"

#include "util/err.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

int httpc_init(httpc *c, const char *host_port) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    const char *colon = strrchr(host_port, ':');
    size_t hl = colon ? (size_t)(colon - host_port) : strlen(host_port);
    if (hl >= sizeof c->host) return CB_E_INVALID;
    if (hl == 0) strcpy(c->host, "127.0.0.1");
    else {
        memcpy(c->host, host_port, hl);
        c->host[hl] = 0;
    }
    const char *port = colon ? colon + 1 : "80";
    if (!*port || strlen(port) >= sizeof c->port) return CB_E_INVALID;
    for (const char *p = port; *p; p++)
        if (*p < '0' || *p > '9') return CB_E_INVALID;
    strcpy(c->port, port);
    return CB_OK;
}

void httpc_close(httpc *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static int connect_once(httpc *c) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(c->host, c->port, &hints, &res) != 0 || !res) return CB_E_IO;
    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { HTTPC_TIMEOUT_S, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0) return CB_E_IO;
    c->fd = fd;
    c->connects++;
    return CB_OK;
}

// Writes everything or fails; a timeout counts as a failure (the socket is then unusable: the
// server has half a request).
static int write_all(int fd, struct iovec *iov, int n) {
    int guard = 1 << 20;   // a byte per iteration at worst, and every iteration makes progress or fails
    while (n > 0 && guard-- > 0) {
#ifdef MSG_NOSIGNAL
        ssize_t w = -1;
        if (n == 1) w = send(fd, iov[0].iov_base, iov[0].iov_len, MSG_NOSIGNAL);
        else w = writev(fd, iov, n);
#else
        ssize_t w = writev(fd, iov, n);
#endif
        if (w < 0) {
            if (errno == EINTR) continue;
            return CB_E_IO;
        }
        if (w == 0) return CB_E_IO;
        size_t done = (size_t)w;
        while (n > 0 && done >= iov[0].iov_len) {
            done -= iov[0].iov_len;
            iov++;
            n--;
        }
        if (n > 0) {
            iov[0].iov_base = (char *)iov[0].iov_base + done;
            iov[0].iov_len -= done;
        }
    }
    return n == 0 ? CB_OK : CB_E_IO;
}

// Reads at least `need` bytes into buf (have of them already there), at most cap. The count, or
// 0 when the peer closed before a byte, -1 on error or timeout.
static ssize_t read_more(int fd, uint8_t *buf, size_t have, size_t need, size_t cap) {
    int guard = 1 << 20;
    while (have < need && guard-- > 0) {
        ssize_t r = read(fd, buf + have, cap - have);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return (ssize_t)have;   // closed
        have += (size_t)r;
    }
    return (ssize_t)have;
}

// One attempt on the open socket. CB_OK with the status; CB_E_IO when the socket is dead (the
// caller may reconnect if *read_any is false); CB_E_INVALID when the bytes are not an answer this
// understands; CB_E_FULL when the body does not fit.
static int attempt(httpc *c, const char *path, const uint8_t *body, size_t len, int *status,
                   uint8_t *resp, size_t cap, size_t *resp_len, bool *read_any) {
    char head[512];
    int hn = snprintf(head, sizeof head,
                      "POST %s HTTP/1.1\r\nHost: %s:%s\r\nContent-Type: application/octet-stream\r\n"
                      "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
                      path, c->host, c->port, len);
    if (hn <= 0 || hn >= (int)sizeof head) return CB_E_INVALID;
    struct iovec iov[2] = { { head, (size_t)hn }, { (void *)body, len } };
    *read_any = false;
    if (write_all(c->fd, iov, len ? 2 : 1) != CB_OK) return CB_E_IO;
    // The head: up to the blank line, at most HTTPC_HDR_CAP bytes (anything past it is body).
    uint8_t hb[HTTPC_HDR_CAP + 1];
    size_t have = 0, head_end = 0;
    int guard = HTTPC_HDR_CAP;
    while (head_end == 0 && guard-- > 0) {
        ssize_t r = read_more(c->fd, hb, have, have + 1, HTTPC_HDR_CAP);
        if (r < 0) return CB_E_IO;
        if ((size_t)r == have) return CB_E_IO;   // closed
        have = (size_t)r;
        *read_any = true;
        hb[have] = 0;
        char *e = strstr((char *)hb, "\r\n\r\n");
        if (e) head_end = (size_t)(e - (char *)hb) + 4;
        else if (have >= HTTPC_HDR_CAP) return CB_E_INVALID;
    }
    if (head_end == 0) return CB_E_INVALID;
    // The status line and the two headers that matter.
    int st = 0;
    if (sscanf((char *)hb, "HTTP/1.%*d %d", &st) != 1 || st < 100 || st > 999) return CB_E_INVALID;
    long clen = -1;
    bool close_after = false;
    char *line = strstr((char *)hb, "\r\n");
    for (int i = 0; line && i < 256; i++) {
        line += 2;
        if (line >= (char *)hb + head_end - 2) break;
        char *end = strstr(line, "\r\n");
        if (!end) break;
        *end = 0;
        if (strncasecmp(line, "Content-Length:", 15) == 0) clen = strtol(line + 15, NULL, 10);
        else if (strncasecmp(line, "Connection:", 11) == 0 && strcasestr(line + 11, "close")) close_after = true;
        else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) return CB_E_INVALID;   // the server never chunks
        *end = '\r';
        line = end;
    }
    if (clen < 0) {
        if (st == 204 || st == 304 || st < 200) clen = 0;
        else return CB_E_INVALID;
    }
    if ((size_t)clen > cap) {
        // Not ours to hold: drop the connection rather than desynchronise it.
        httpc_close(c);
        *status = st;
        *resp_len = 0;
        return CB_E_FULL;
    }
    size_t body_have = have - head_end;
    if (body_have > (size_t)clen) {
        // Bytes past the body: a pipelined answer we never asked for. Unusable connection.
        httpc_close(c);
        return CB_E_INVALID;
    }
    memcpy(resp, hb + head_end, body_have);
    if (body_have < (size_t)clen) {
        ssize_t r = read_more(c->fd, resp, body_have, (size_t)clen, cap);
        if (r < 0 || (size_t)r < (size_t)clen) {
            httpc_close(c);
            return CB_E_IO;
        }
    }
    *status = st;
    *resp_len = (size_t)clen;
    if (close_after) httpc_close(c);
    return CB_OK;
}

int httpc_post(httpc *c, const char *path, const uint8_t *body, size_t len, int *status,
               uint8_t *resp, size_t cap, size_t *resp_len) {
    *status = 0;
    *resp_len = 0;
    // Two attempts at most: the second only when the first found the socket dead before any
    // byte of an answer arrived (a keep-alive the server had closed).
    for (int tries = 0; tries < 2; tries++) {
        if (c->fd < 0 && connect_once(c) != CB_OK) return CB_E_IO;
        bool read_any = false;
        int rc = attempt(c, path, body, len, status, resp, cap, resp_len, &read_any);
        if (rc == CB_OK || rc == CB_E_FULL) return rc;
        httpc_close(c);
        if (rc != CB_E_IO || read_any) return rc;
    }
    return CB_E_IO;
}
