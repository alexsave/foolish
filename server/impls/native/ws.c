// ws.c — see ws.h.
#define _GNU_SOURCE
#include "ws.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// SHA-1 (FIPS 180-4 / RFC 3174). Textbook implementation, same shape as
// c/src/sha256.c: a spec-defined function with known-answer vectors, nothing
// to invent. Freestanding beyond memcpy.
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t state[5];
    uint64_t bitlen;
    uint8_t  buf[64];
    int      buflen;
} Sha1;

static uint32_t sha1_rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(Sha1 *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3], e = c->state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | ((~b) & d);       k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                  k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ cc ^ d;                  k = 0xCA62C1D6u; }
        uint32_t t = sha1_rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = sha1_rol(b, 30); b = a; a = t;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d; c->state[4] += e;
}

static void sha1_init(Sha1 *c) {
    c->state[0] = 0x67452301u; c->state[1] = 0xEFCDAB89u; c->state[2] = 0x98BADCFEu;
    c->state[3] = 0x10325476u; c->state[4] = 0xC3D2E1F0u;
    c->bitlen = 0; c->buflen = 0;
}

static void sha1_update(Sha1 *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->bitlen += (uint64_t)len * 8;
    while (len > 0) {
        size_t want = (size_t)(64 - c->buflen);
        size_t take = len < want ? len : want;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += (int)take; p += take; len -= take;
        if (c->buflen == 64) { sha1_block(c, c->buf); c->buflen = 0; }
    }
}

static void sha1_final(Sha1 *c, uint8_t out[20]) {
    uint64_t bits = c->bitlen;
    uint8_t pad = 0x80; sha1_update(c, &pad, 1);
    pad = 0x00; while (c->buflen != 56) sha1_update(c, &pad, 1);
    c->bitlen = bits;   // padding must not count toward the length field
    uint8_t lenbe[8];
    for (int i = 0; i < 8; i++) lenbe[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, lenbe, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

void ws_sha1(const unsigned char *data, size_t len, unsigned char out[20]) {
    Sha1 c; sha1_init(&c); sha1_update(&c, data, len); sha1_final(&c, out);
}

// ---------------------------------------------------------------------------
// base64
// ---------------------------------------------------------------------------

int ws_base64_encode(const unsigned char *data, int len, char *out, int cap) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int oi = 0;
    for (int i = 0; i < len; i += 3) {
        if (oi + 4 >= cap) return 0;
        unsigned int v = (unsigned int)data[i] << 16;
        if (i + 1 < len) v |= (unsigned int)data[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned int)data[i + 2];
        out[oi++] = tbl[(v >> 18) & 0x3F];
        out[oi++] = tbl[(v >> 12) & 0x3F];
        out[oi++] = (i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=';
        out[oi++] = (i + 2 < len) ? tbl[v & 0x3F] : '=';
    }
    if (oi >= cap) return 0;
    out[oi] = 0;
    return oi;
}

int ws_accept_from_key(const char *client_key, char *out, int cap) {
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char buf[256];
    int n = snprintf(buf, sizeof buf, "%s%s", client_key, GUID);
    if (n <= 0 || n >= (int)sizeof buf) return 0;
    unsigned char digest[20];
    ws_sha1((const unsigned char *)buf, (size_t)n, digest);
    return ws_base64_encode(digest, 20, out, cap);
}

// ---------------------------------------------------------------------------
// Reliable read/write
// ---------------------------------------------------------------------------

int ws_read_full(int fd, void *buf, int n) {
    unsigned char *p = (unsigned char *)buf;
    int got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, (size_t)(n - got));
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;   // peer closed mid-frame
        got += (int)r;
    }
    return got;
}

int ws_write_full(int fd, const void *buf, int n) {
    const unsigned char *p = (const unsigned char *)buf;
    int sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, p + sent, (size_t)(n - sent));
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        sent += (int)w;
    }
    return sent;
}

// ---------------------------------------------------------------------------
// WsConn: buffered read (drains `pending` before touching the socket again)
// ---------------------------------------------------------------------------

void ws_conn_init(WsConn *c, int fd, int mask_outgoing) {
    c->fd = fd;
    c->mask_outgoing = mask_outgoing;
    c->pending_len = 0;
    c->pending_off = 0;
}

void ws_conn_prime(WsConn *c, const unsigned char *data, int len) {
    if (len <= 0) return;
    int cap = (int)sizeof c->pending;
    if (len > cap) len = cap;   // realistic peers never pipeline frames ahead of the handshake
    memcpy(c->pending, data, (size_t)len);
    c->pending_len = len;
    c->pending_off = 0;
}

static int ws_fill(WsConn *c, unsigned char *out, int n) {
    int got = 0;
    while (got < n) {
        if (c->pending_off < c->pending_len) {
            int have = c->pending_len - c->pending_off;
            int take = n - got; if (take > have) take = have;
            memcpy(out + got, c->pending + c->pending_off, (size_t)take);
            c->pending_off += take; got += take;
            continue;
        }
        int r = ws_read_full(c->fd, out + got, n - got);
        if (r < 0) return -1;
        got += r;
    }
    return got;
}

// ---------------------------------------------------------------------------
// Frame write: 0..125 / 126 (+u16) / 127 (+u64) length encoding, masked iff
// c->mask_outgoing (RFC 6455 5.1: client->server MUST be masked, server->
// client MUST NOT be).
// ---------------------------------------------------------------------------

int ws_send_frame(WsConn *c, int opcode, const unsigned char *payload, int64_t len) {
    unsigned char hdr[14];
    int hn = 0;
    hdr[hn++] = (unsigned char)(0x80 | (opcode & 0x0F));   // FIN=1
    unsigned char maskbit = c->mask_outgoing ? 0x80 : 0x00;
    if (len <= 125) {
        hdr[hn++] = (unsigned char)(maskbit | (unsigned char)len);
    } else if (len <= 0xFFFF) {
        hdr[hn++] = (unsigned char)(maskbit | 126);
        hdr[hn++] = (unsigned char)((len >> 8) & 0xFF);
        hdr[hn++] = (unsigned char)(len & 0xFF);
    } else {
        hdr[hn++] = (unsigned char)(maskbit | 127);
        for (int i = 7; i >= 0; i--) hdr[hn++] = (unsigned char)((len >> (8 * i)) & 0xFF);
    }
    unsigned char mkey[4] = {0, 0, 0, 0};
    if (c->mask_outgoing) {
        for (int i = 0; i < 4; i++) mkey[i] = (unsigned char)(rand() & 0xFF);
        memcpy(hdr + hn, mkey, 4); hn += 4;
    }
    if (ws_write_full(c->fd, hdr, hn) != hn) return -1;
    if (len <= 0) return 0;

    if (!c->mask_outgoing) {
        if (ws_write_full(c->fd, payload, (int)len) != (int)len) return -1;
        return (int)len;
    }
    // Masked send: XOR through a bounded scratch buffer so the caller's
    // payload (often a shared response buffer) is never mutated.
    unsigned char chunk[4096];
    int64_t off = 0;
    while (off < len) {
        int64_t take = len - off; if (take > (int64_t)sizeof chunk) take = (int64_t)sizeof chunk;
        for (int64_t i = 0; i < take; i++) chunk[i] = payload[off + i] ^ mkey[(off + i) & 3];
        if (ws_write_full(c->fd, chunk, (int)take) != (int)take) return -1;
        off += take;
    }
    return (int)len;
}

void ws_send_close(WsConn *c, uint16_t code) {
    unsigned char payload[2] = { (unsigned char)(code >> 8), (unsigned char)(code & 0xFF) };
    ws_send_frame(c, WS_OP_CLOSE, payload, 2);
}

// ---------------------------------------------------------------------------
// Frame read + message assembly
// ---------------------------------------------------------------------------

int ws_recv_message(WsConn *c, unsigned char *buf, int cap, int *opcode) {
    int total = 0;
    int msg_opcode = -1;

    for (;;) {
        unsigned char hdr[2];
        if (ws_fill(c, hdr, 2) < 0) return -1;
        int fin    = (hdr[0] >> 7) & 1;
        int op     = hdr[0] & 0x0F;
        int masked = (hdr[1] >> 7) & 1;
        int64_t len = hdr[1] & 0x7F;

        if (len == 126) {
            unsigned char ext[2];
            if (ws_fill(c, ext, 2) < 0) return -1;
            len = ((int64_t)ext[0] << 8) | ext[1];
        } else if (len == 127) {
            unsigned char ext[8];
            if (ws_fill(c, ext, 8) < 0) return -1;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
            if (len < 0) return -1;   // top bit set is a protocol violation per spec
        }

        unsigned char mkey[4] = {0, 0, 0, 0};
        if (masked) { if (ws_fill(c, mkey, 4) < 0) return -1; }

        if (op == WS_OP_PING || op == WS_OP_PONG || op == WS_OP_CLOSE) {
            if (len > 125) return -1;   // control frames are never fragmented/oversized
            unsigned char ctrl[125];
            if (len > 0 && ws_fill(c, ctrl, (int)len) < 0) return -1;
            if (masked) for (int i = 0; i < (int)len; i++) ctrl[i] ^= mkey[i & 3];
            if (op == WS_OP_PING) { ws_send_frame(c, WS_OP_PONG, ctrl, len); continue; }
            if (op == WS_OP_PONG) continue;
            // CLOSE: echo it back (RFC 6455 5.5.1 closing handshake), then
            // this connection is done.
            ws_send_close(c, 1000);
            return -1;
        }

        if (op != WS_OP_CONT && op != WS_OP_TEXT && op != WS_OP_BIN) return -1;   // reserved/unknown opcode
        if (op != WS_OP_CONT) {
            if (msg_opcode != -1) return -1;   // a new message started before the last one finished
            msg_opcode = op;
        } else if (msg_opcode == -1) {
            return -1;   // continuation with nothing to continue
        }
        if (len < 0 || total + len > cap) return -1;   // oversized for the caller's buffer

        if (len > 0) {
            if (ws_fill(c, buf + total, (int)len) < 0) return -1;
            if (masked) for (int64_t i = 0; i < len; i++) buf[total + i] ^= mkey[i & 3];
        }
        total += (int)len;
        if (fin) { if (opcode) *opcode = msg_opcode; return total; }
        // else: loop for the next fragment (must arrive as WS_OP_CONT)
    }
}
