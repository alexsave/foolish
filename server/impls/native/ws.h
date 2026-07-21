// ws.h — a minimal RFC 6455 WebSocket layer for foolish_server / foolish_hammer.
//
// Just enough of the spec to carry the hot loop (action in, masked state out)
// over ONE persistent TCP connection instead of one HTTP request per move
// (see PROFILE_HOTPATH.md T1: thread-per-request was 85.8% of instructions
// under load). Handshake (SHA-1 + base64, RFC 6455 section 1.3), frame
// read/write (section 5.2) with masking in both directions, PING/PONG/CLOSE,
// and minimal fragmentation assembly. No compression, no permessage-deflate,
// no subprotocols — this server and this load client are the only two peers
// that need to agree.
//
// Both foolish_server.c (mask_outgoing=0: server frames MUST NOT be masked)
// and foolish_hammer.c (mask_outgoing=1: client frames MUST be masked) share
// this file, so the wire format can never drift between the two sides of the
// same process family.
#ifndef FOOLISH_WS_H
#define FOOLISH_WS_H

#include <stddef.h>
#include <stdint.h>

#include "conn.h"   // Stage 3 (TLS): every frame read/write below goes through a Conn* now, plain fd or SSL* alike — see conn.h

// ---------------------------------------------------------------------------
// Handshake primitives: SHA-1 (public-domain-style textbook implementation,
// mirroring c/src/sha256.c's shape — the kernel has SHA-256 but not SHA-1,
// and the handshake is specified against SHA-1, not swappable) + base64.
// ---------------------------------------------------------------------------

void ws_sha1(const unsigned char *data, size_t len, unsigned char out[20]);

// Standard base64 (with '=' padding). Returns bytes written (excl. the NUL
// terminator, which IS written) into out, or 0 if it wouldn't fit in cap.
int ws_base64_encode(const unsigned char *data, int len, char *out, int cap);

// Sec-WebSocket-Accept = base64(SHA1(client_key + the RFC 6455 magic GUID)).
// out must be >= 32 bytes. Returns bytes written, or 0 on failure.
int ws_accept_from_key(const char *client_key, char *out, int cap);

// ---------------------------------------------------------------------------
// Frame I/O
// ---------------------------------------------------------------------------

#define WS_OP_CONT  0x0
#define WS_OP_TEXT  0x1
#define WS_OP_BIN   0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING  0x9
#define WS_OP_PONG  0xA

// One WsConn per live connection (server: one per accepted+upgraded
// connection, living in that connection's own thread; client: one per
// persistent hammer worker). `conn` carries either a plain fd or a
// per-connection TLS session (Stage 3 — see conn.h); every read/write below
// goes through it, never a bare fd. `pending` holds bytes the caller
// already read off the socket before handing it to the WS layer (e.g. any
// bytes that rode in the same TCP segment as the HTTP upgrade
// request/response, past its blank line) — ws_conn_prime seeds it,
// ws_recv_message drains it before reading again, so a coalesced read can
// never lose bytes.
typedef struct {
    Conn conn;
    int mask_outgoing;   // 1 = WE must mask frames we SEND (i.e. we are the client)
    unsigned char pending[4096];
    int pending_len;
    int pending_off;
} WsConn;

// `conn` is copied by value into `c->conn` — the caller's own Conn variable
// is no longer needed after this call (both foolish_server.c's
// ws_conn_thread and foolish_hammer.c's ws_worker treat it that way).
void ws_conn_init(WsConn *c, Conn conn, int mask_outgoing);
// Seed already-read bytes (clamped to the pending capacity — in practice this
// is 0 bytes almost always, since a compliant peer waits for the handshake to
// finish before sending frames).
void ws_conn_prime(WsConn *c, const unsigned char *data, int len);

// Reads one complete application message (assembling fragmentation if the
// peer split it across CONT frames), transparently answering PING with PONG
// and swallowing PONG, and reads-and-closes-out on a peer CLOSE frame.
// Returns the message length (>= 0) and sets *opcode to WS_OP_TEXT or
// WS_OP_BIN on success. Returns -1 on I/O error, protocol violation, a
// message too large for `cap`, or a CLOSE (either received or sent in
// reply) — in every -1 case the connection should be torn down.
int ws_recv_message(WsConn *c, unsigned char *buf, int cap, int *opcode);

// Sends one unfragmented frame (FIN=1). Masks the payload iff
// c->mask_outgoing. Returns the payload length sent, or -1 on I/O error.
int ws_send_frame(WsConn *c, int opcode, const unsigned char *payload, int64_t len);

// Sends a CLOSE frame carrying `code` (network byte order per RFC 6455
// section 5.5.1) with no reason string.
void ws_send_close(WsConn *c, uint16_t code);

// Reliable conn_read()/conn_write() loops (short read/write is not an error
// in POSIX sockets, especially under load, and SSL_read/SSL_write can hand
// back less than requested too) — exported since foolish_server.c also
// needs one to send the plain-HTTP 101 response reliably before the
// connection switches to being a WsConn, and foolish_hammer.c needs one for
// its own client-side handshake request.
int ws_read_full(Conn *c, void *buf, int n);
int ws_write_full(Conn *c, const void *buf, int n);

#endif
