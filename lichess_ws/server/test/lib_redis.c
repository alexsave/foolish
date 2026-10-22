// lib_redis.c - lib_redis.h.
#include "lib_redis.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int tr_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    return fd;
}

static int send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; }
        p += k; n -= (size_t)k;
    }
    return 0;
}

static void bulk(cb_buf *b, const char *s) {
    cb_buf_printf(b, "$%zu\r\n", strlen(s));
    cb_buf_puts(b, s);
    cb_buf_puts(b, "\r\n");
}

// Reads one RESP value: arrays of bulk strings into parts (others consumed and ignored).
typedef struct { char *p; size_t n; } part;
static int read_byte(int fd, int timeout_ms, char *c) {
    struct pollfd p = { .fd = fd, .events = POLLIN };
    if (poll(&p, 1, timeout_ms) != 1) return -1;
    return recv(fd, c, 1, 0) == 1 ? 0 : -1;
}
static int read_line(int fd, int timeout_ms, char *out, size_t cap) {
    size_t i = 0;
    for (;;) {
        char c;
        if (read_byte(fd, timeout_ms, &c) != 0) return -1;
        if (c == '\n' && i && out[i - 1] == '\r') { out[i - 1] = 0; return 0; }
        if (i + 1 >= cap) return -1;
        out[i++] = c;
    }
}
static int read_exact(int fd, int timeout_ms, char *out, size_t n) {
    size_t got = 0;
    while (got < n) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, timeout_ms) != 1) return -1;
        ssize_t k = recv(fd, out + got, n - got, 0);
        if (k <= 0) return -1;
        got += (size_t)k;
    }
    return 0;
}
static int read_value(int fd, int timeout_ms, part *parts, int cap, int *np) {
    char line[256];
    if (read_line(fd, timeout_ms, line, sizeof line) != 0) return -1;
    if (line[0] == '+' || line[0] == '-' || line[0] == ':') return 0;
    long len = strtol(line + 1, NULL, 10);
    if (line[0] == '$') {
        if (len < 0) return 0;
        char *p = malloc((size_t)len + 3);
        if (!p || read_exact(fd, timeout_ms, p, (size_t)len + 2) != 0) { free(p); return -1; }
        p[len] = 0;
        if (*np < cap) { parts[*np].p = p; parts[*np].n = (size_t)len; (*np)++; } else free(p);
        return 0;
    }
    if (line[0] == '*') {
        for (long i = 0; i < len; i++)
            if (read_value(fd, timeout_ms, parts, cap, np) != 0) return -1;
        return 0;
    }
    return -1;
}
static void free_parts(part *parts, int np) { for (int i = 0; i < np; i++) free(parts[i].p); }

int tr_publish(int fd, const char *chan, const char *msg) {
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_puts(&b, "*3\r\n");
    bulk(&b, "PUBLISH"); bulk(&b, chan); bulk(&b, msg);
    int rc = send_all(fd, cb_buf_cstr(&b), b.len);
    cb_buf_free(&b);
    if (rc != 0) return -1;
    part parts[4]; int np = 0;
    rc = read_value(fd, 5000, parts, 4, &np);
    free_parts(parts, np);
    return rc;
}

int tr_psubscribe(int fd, const char *pattern) {
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_puts(&b, "*2\r\n");
    bulk(&b, "PSUBSCRIBE"); bulk(&b, pattern);
    int rc = send_all(fd, cb_buf_cstr(&b), b.len);
    cb_buf_free(&b);
    if (rc != 0) return -1;
    part parts[4]; int np = 0;
    rc = read_value(fd, 5000, parts, 4, &np);
    bool ok = rc == 0 && np >= 1 && strcmp(parts[0].p, "psubscribe") == 0;
    free_parts(parts, np);
    return ok ? 0 : -1;
}

int tr_next(int fd, int timeout_ms, char *chan, size_t ccap, cb_buf *msg) {
    for (;;) {
        part parts[4]; int np = 0;
        if (read_value(fd, timeout_ms, parts, 4, &np) != 0) { free_parts(parts, np); return -1; }
        bool pm = np == 4 && strcmp(parts[0].p, "pmessage") == 0;
        if (pm) {
            snprintf(chan, ccap, "%s", parts[2].p);
            cb_buf_reset(msg);
            cb_buf_append(msg, parts[3].p, parts[3].n);
        }
        free_parts(parts, np);
        if (pm) return 0;
    }
}

static long now_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000L + tv.tv_usec / 1000; }

int tr_wait_prefix(int fd, int timeout_ms, const char *chan_prefix, const char *prefix, char *chan, size_t ccap, cb_buf *msg) {
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) return -1;
        if (tr_next(fd, (int)left, chan, ccap, msg) != 0) return -1;
        if (strncmp(chan, chan_prefix, strlen(chan_prefix)) == 0 && strncmp(cb_buf_cstr(msg), prefix, strlen(prefix)) == 0) return 0;
    }
}

int tr_wait(int fd, int timeout_ms, const char *want_chan, const char *prefix, cb_buf *msg) {
    char chan[64];
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) return -1;
        if (tr_next(fd, (int)left, chan, sizeof chan, msg) != 0) return -1;
        if (strcmp(chan, want_chan) == 0 && strncmp(cb_buf_cstr(msg), prefix, strlen(prefix)) == 0) return 0;
    }
}
