// wasm_net_api.c - the backend's client (src/netclient.h) as the web reaches it.
//
// Nothing here decides anything. Inputs (a body, a frame, a game id, a name and
// a password back to back) are written at wasm_net_inbox(len) with their lengths
// as call arguments; answers are netclient.h's structs, read through
// sdk/ts/gen/net_layout.bots.ts. A span in an answer (an envelope, a replay's
// moves) is an offset into the body as it was written, so the host slices its
// own copy. Times and the action's intent cross as doubles.
//
// THE INBOX is the IO buffer, unless a body is longer than it (a page of match
// history can be): then it is a buffer of its own, allocated once at the
// largest length asked for and kept, so linear memory grows only for a player
// whose history needs it.
#include "../src/netclient.h"
#include <stdlib.h>

extern unsigned char *wasm_io_ptr(void);
extern int wasm_io_cap(void);

static unsigned char *g_big;
static int g_big_cap;
static unsigned char *g_in;
static int g_in_cap;

// Where the host writes `len` bytes of input; 0 when they cannot be held.
unsigned char *wasm_net_inbox(int len) {
    if (len < 0) return 0;
    if (len <= wasm_io_cap()) { g_in = wasm_io_ptr(); g_in_cap = wasm_io_cap(); return g_in; }
    if (len > g_big_cap) {
        unsigned char *b = malloc((size_t)len);
        if (!b) return 0;
        g_big = b;   // the bump allocator never frees; the old one is simply not used again
        g_big_cap = len;
    }
    g_in = g_big;
    g_in_cap = g_big_cap;
    return g_in;
}

static const unsigned char *io_of(int len) { return g_in && len >= 0 && len <= g_in_cap ? g_in : 0; }
static int64_t i64(double v) { return (int64_t)v; }

void wasm_net_reset(void) { nc_reset(); }

// inbox = [blob]
int wasm_net_session_load(int len) {
    const unsigned char *io = io_of(len);
    return io ? nc_session_load(io, len) : NW_E_WIRE;
}
// -> io = the blob; its length, 0 signed out.
int wasm_net_session_save(void) { g_in = 0; return nc_session_save(wasm_io_ptr(), wasm_io_cap()); }
const NcSessionInfo *wasm_net_session_info(void) { return nc_session_info(); }
const NcError *wasm_net_error(void) { return nc_error(); }

// ---- the pump: an intent in, requests out, answers in, tickets done ----
static NcIntent g_intent;
// Where the host writes the intent (the generated NcIntent writer) before wasm_net_do.
NcIntent *wasm_net_intent_ptr(void) { return &g_intent; }
unsigned wasm_net_do(double now_ms) { return nc_do(&g_intent, i64(now_ms)); }
const NcRequest *wasm_net_next_request(double now_ms) { return nc_next_request(i64(now_ms)); }
// inbox = [body]
const NcDone *wasm_net_on_response(unsigned id, int status, int len, double now_ms) {
    const unsigned char *io = io_of(len);
    return nc_on_response(id, status, io, io ? len : 0, i64(now_ms));
}
const NcDone *wasm_net_next_done(void) { return nc_next_done(); }
// inbox = [body]: row i of a finished list.
const void *wasm_net_row(int kind, int len, int i) { return io_of(len) ? nc_row(kind, g_in, len, i) : 0; }
// The stream's address (GET, upgraded to a WebSocket).
const NcRequest *wasm_net_stream_request(void) { return nc_req_stream(); }

// inbox = [game id]
int wasm_net_stream_sub(int what, int gid_len) { return io_of(gid_len) ? nc_stream_sub(what, (const char *)g_in, gid_len) : NW_E_WIRE; }
int wasm_net_stream_unsub(int what, int gid_len) { return io_of(gid_len) ? nc_stream_unsub(what, (const char *)g_in, gid_len) : NW_E_WIRE; }
void wasm_net_stream_opened(double now_ms) { nc_stream_opened(i64(now_ms)); }
int wasm_net_stream_closed(double now_ms) { return nc_stream_closed(i64(now_ms)); }
// inbox = [frame]: how many events it made.
int wasm_net_stream_in(int len, int is_text, double now_ms) {
    const unsigned char *io = io_of(len);
    return nc_stream_in(io, io ? len : 0, is_text, i64(now_ms));
}
// What the last feed (a frame, an answer) changed: event i, NULL past the last.
const NcEvent *wasm_net_event(int i) { return nc_event(i); }
const uint8_t *wasm_net_event_payload(int i) { return nc_event_payload(i); }
const NcOut *wasm_net_stream_next(double now_ms) { return nc_stream_next(i64(now_ms)); }
// When to call again: the sooner of the pump's and the stream's, -1 for neither.
int wasm_net_due_ms(double now_ms) { return nc_due_ms(i64(now_ms)); }
