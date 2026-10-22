// sf_wasm.c - the site's end of the platform's push socket, in C compiled to wasm: the stream
// frames (shared/c/stream_frame.h) a client writes (HELLO, SUB, PING) and reads (READY, REPLY, a
// push), through the header's own inline writers and parser, so the one owner of those bytes stays
// stream_frame.h and the site spells none of them. The bindings TypeScript calls are generated
// from this file's definitions by structgen (web/src/gen/sf_wasm.ts), and --check-wasm holds the
// built module to them.
//
// The host copies a token or a room id into `in` (sfw_in, sfw_in_cap bytes), calls a writer, and
// sends `out` (sfw_out) for the length returned; it copies each WebSocket message into `in` and
// asks sfw_kind what it is. A push's product frame (wire.h: [SF_V][BE_F_*][record]) is read in
// place: sfw_push_kind names it, sfw_push_body is where its record starts.
#include "../../../shared/c/stream_frame.h"

#include <stddef.h>
#include <stdint.h>

#define SFW_CAP 65536

static uint8_t g_in[SFW_CAP];
static uint8_t g_out[SF_TOKEN_MAX + 16];

uint8_t *sfw_in(void) { return g_in; }
int      sfw_in_cap(void) { return SFW_CAP; }
uint8_t *sfw_out(void) { return g_out; }

// HELLO with the token_len bytes of token in `in`: the frame's length in `out`, or -1.
int sfw_hello(int token_len) {
    if (token_len < 0) return -1;
    return sf_put_hello(g_out, sizeof g_out, (const char *)g_in, (size_t)token_len);
}

// SUB (unsub 0) or UNSUB (unsub 1) of `what` (netwire.h NW_SUB_*) for the gid_len bytes of room
// id in `in`, with the reply id `id`: the frame's length in `out`, or -1.
int sfw_sub(uint32_t id, int what, int gid_len, int unsub) {
    if (gid_len < 0 || what < 0 || what > 255) return -1;
    return sf_put_sub(g_out, sizeof g_out, unsub ? SF_UNSUB : SF_SUB, id, (uint8_t)what, (const char *)g_in, (size_t)gid_len);
}

int sfw_ping(void) { return sf_put_bare(g_out, sizeof g_out, SF_PING); }

// The n bytes in `in`: a frame of the socket's own (its SF_* kind), 0 for a push (the product's
// frame), or -1 for bytes that are neither.
int sfw_kind(int n) {
    if (n < 0 || n > SFW_CAP) return -1;
    if (sf_is_push(g_in, (size_t)n)) return 0;
    sf_frame f;
    if (sf_parse(g_in, (size_t)n, &f) != 0) return -1;
    return f.kind;
}

// A REPLY's id and status (the n bytes in `in`); 0 for a frame that is not a reply.
uint32_t sfw_reply_id(int n) {
    sf_frame f;
    if (n < 0 || n > SFW_CAP || sf_parse(g_in, (size_t)n, &f) != 0 || f.kind != SF_REPLY) return 0;
    return f.id;
}
int sfw_reply_status(int n) {
    sf_frame f;
    if (n < 0 || n > SFW_CAP || sf_parse(g_in, (size_t)n, &f) != 0 || f.kind != SF_REPLY) return 0;
    return f.status;
}

// A push's product kind (the byte after SF_V), or -1 for bytes that are not a push.
int sfw_push_kind(int n) {
    if (n < 2 || n > SFW_CAP || !sf_is_push(g_in, (size_t)n)) return -1;
    return g_in[1];
}
// Where a push's record starts in `in`.
int sfw_push_body(void) { return 2; }
