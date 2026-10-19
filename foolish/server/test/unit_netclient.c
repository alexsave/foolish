// unit_netclient.c - the client library every host runs (foolish/c/src/
// netclient.h), with no server: the session as a blob, what each request
// says, when the token is refreshed, how an answer is read, and the stream's
// own decisions over MQTT (what to send in what order, retries, reconnects,
// pings, the token on CONNECT, and the version gates), against a fake broker
// built from the same codec (shared/c/mqtt).
#include "t.h"

#include "ask.h"
#include "netclient.h"
#include "netwire.h"
#include "roster.h"
#include "table.h"
#include "view.h"
#include "evwire.h"
#include "game.h"

#include "../../../shared/c/stream_frame.h"

#include <stdlib.h>

#define T0 1700000000000LL

static void pending_drop(void);

// A request's id, 0 for none: a request a mutant never made fails an
// assertion instead of crashing the suite.
static uint32_t id_of(const NcRequest *q) { return q ? q->id : 0; }

static void signed_in(uint64_t expires_at_ms) {
    pending_drop();
    NwSession s;
    memset(&s, 0, sizeof s);
    s.access_len = 3; memcpy(s.access, "jwt", 3);
    s.refresh_len = 2; memcpy(s.refresh, "rt", 2);
    s.expires_at_ms = expires_at_ms;
    s.user_id_len = 2; memcpy(s.user_id, "u1", 2);
    s.username_len = 3; memcpy(s.username, "ANA", 3);
    uint8_t b[64];
    int n = nw_session_encode(&s, b, sizeof b);
    nc_reset();
    T_EQ_INT(nc_session_load(b, n), NW_OK);
}

static void session_blob(void) {
    nc_reset();
    uint8_t b[NC_SESSION_BLOB_MAX];
    T_EQ_INT(nc_session_save(b, sizeof b), 0);   // signed out: nothing to keep
    T_EQ_INT(nc_session_info()->signed_in, 0);
    signed_in(T0 + 3600000);
    int n = nc_session_save(b, sizeof b);
    T_CHECK(n > 0);
    nc_reset();
    T_EQ_INT(nc_session_load(b, n), NW_OK);
    const NcSessionInfo *i = nc_session_info();
    T_CHECK(i->signed_in && i->username_len == 3 && memcmp(i->username, "ANA", 3) == 0);
    // A blob cut short signs the client out.
    T_EQ_INT(nc_session_load(b, n - 1), NW_E_WIRE);
    T_EQ_INT(nc_session_info()->signed_in, 0);
    T_EQ_INT(nc_session_save(b, sizeof b), 0);
}

static void requests(void) {
    nc_reset();
    // Nobody signed in: nothing that needs a user is built.
    T_CHECK(nc_req_games(T0) == NULL && nc_req_create(T0) == NULL && nc_req_refresh() == NULL);
    const NcRequest *q = nc_req_signin("ANA", 3, "pw", 2);
    T_CHECK(q && q->method == NC_POST && q->auth_len == 0 && !q->is_refresh);
    T_EQ_MEM(q->path, q->path_len, "/v1/auth/signin", 15);
    const uint8_t signin[] = { 1, NW_SIGNIN, 3, 'A', 'N', 'A', 2, 'p', 'w' };
    T_EQ_MEM(q->body, q->body_len, signin, sizeof signin);
    T_CHECK(nc_req_signup("ANA", 3, "x", NW_PASSWORD_MAX + 1) == NULL);
    q = nc_req_board();
    T_CHECK(q && q->method == NC_GET && q->auth_len == 0 && q->body_len == 0);

    signed_in(T0 + 3600000);
    q = nc_req_game("ab12cd", 6, T0);
    T_CHECK(q && q->method == NC_GET && !q->is_refresh);
    T_EQ_MEM(q->path, q->path_len, "/v1/games/ab12cd", 16);
    T_EQ_MEM(q->auth, q->auth_len, "Bearer jwt", 10);
    T_CHECK(nc_req_game("ab/../x", 7, T0) == NULL);   // never a path the id did not say
    q = nc_req_action("ab12cd", 6, (const uint8_t *)"\x03\x00", 2, 9, T0);
    const uint8_t action[] = { 2, 6, 'a', 'b', '1', '2', 'c', 'd', 9, 0, 0, 0, 3, 0 };   // table.h v2
    T_CHECK(q != NULL);
    if (q) T_EQ_MEM(q->body, q->body_len, action, sizeof action);
    q = nc_req_action("ab12cd", 6, (const uint8_t *)"\x03\x00", 2, -1, T0);
    const uint8_t action1[] = { 1, 6, 'a', 'b', '1', '2', 'c', 'd', 3, 0 };   // no intent: v1
    if (q) T_EQ_MEM(q->body, q->body_len, action1, sizeof action1);
    q = nc_req_chat_send("ab12cd", 6, "yo", 2, T0);
    T_EQ_MEM(q->path, q->path_len, "/v1/chat/ab12cd", 15);
    const uint8_t chat[] = { 1, NW_CHAT_SEND, 2, 0, 'y', 'o' };
    T_EQ_MEM(q->body, q->body_len, chat, sizeof chat);
    q = nc_req_signout(1, T0);
    const uint8_t out_g[] = { 1, NW_SIGNOUT, NW_SCOPE_GLOBAL };
    T_EQ_MEM(q->body, q->body_len, out_g, sizeof out_g);
    q = nc_req_stream();
    T_EQ_MEM(q->path, q->path_len, "/ws", 3);

    // Due within NC_REFRESH_EARLY_MS of the expiry: the refresh comes first.
    q = nc_req_games(T0 + 3600000 - NC_REFRESH_EARLY_MS - 1);
    T_CHECK(q && !q->is_refresh);
    q = nc_req_games(T0 + 3600000 - NC_REFRESH_EARLY_MS);
    T_CHECK(q && q->is_refresh && q->auth_len == 0);
    T_EQ_MEM(q->path, q->path_len, "/v1/auth/refresh", 16);
    const uint8_t refresh[] = { 1, NW_REFRESH, 2, 'r', 't' };
    T_EQ_MEM(q->body, q->body_len, refresh, sizeof refresh);
}

static void answers(void) {
    signed_in(T0 + 3600000);
    // A refused refresh for the token's own sake signs out; any other refusal does not.
    uint8_t r[ASK_REFUSAL_BYTES];
    AskRefusal ar = { ASK_WHY_ACCOUNT, NW_ACCOUNT_E_BAD_LOGIN, 0 };
    ask_refusal_encode(&ar, r, sizeof r);
    T_EQ_INT(nc_session_read(400, r, sizeof r), ASK_WHY_ACCOUNT);
    T_EQ_INT(nc_session_info()->signed_in, 1);
    ar.code = NW_ACCOUNT_E_REFRESH_USED;
    ask_refusal_encode(&ar, r, sizeof r);
    T_EQ_INT(nc_session_read(400, r, sizeof r), ASK_WHY_ACCOUNT);
    T_EQ_INT(nc_session_info()->signed_in, 0);
    T_EQ_INT(nc_error()->code, NW_ACCOUNT_E_REFRESH_USED);
    // No refusal body (a proxy's error): the status speaks.
    T_EQ_INT(nc_status_read(502, (const uint8_t *)"<html>", 6), ASK_WHY_INTERNAL);
    T_EQ_INT(nc_status_read(401, NULL, 0), ASK_WHY_AUTH);
    T_EQ_INT(nc_status_read(204, NULL, 0), 0);
    T_EQ_INT(nc_error()->why, 0);
    // A 200 that is not a session is not taken for one.
    T_EQ_INT(nc_session_read(200, (const uint8_t *)"\x01\x14", 2), ASK_WHY_MALFORMED);
    T_EQ_INT(nc_session_info()->signed_in, 0);

    const uint8_t act[] = { 1, 1, 7, 9, 0, 0, 0 };   // rejected, code 7, version 9
    const NcAction *a = nc_action_read(200, act, sizeof act);
    T_CHECK(a && a->status == 1 && a->reject_code == 7 && a->version == 9);
    T_CHECK(nc_action_read(200, act, 3) == NULL);

    const uint8_t view[] = { 1, NW_VIEW, 4, 0, 0, 0, 2, 0, 0, 0, 0xaa, 0xbb };
    const NcGameRow *g = nc_view_read(200, view, sizeof view);
    T_CHECK(g && g->version == 4 && g->env_off == 10 && g->env_len == 2);
    const uint8_t games[] = { 1, NW_GAMES, 1, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0xcc };
    T_EQ_INT(nc_list_read(200, games, sizeof games, NW_GAMES), 1);
    g = nc_game_row(games, sizeof games, 0);
    T_CHECK(g && g->version == 3 && g->env_off == 12 && g->env_len == 1);
    T_CHECK(nc_game_row(games, sizeof games, 1) == NULL);
    T_EQ_INT(nc_list_read(200, games, sizeof games - 1, NW_GAMES), -ASK_WHY_MALFORMED);
}

// ---- the stream: a fake server ------------------------------------------------------------

// The frames the client wrote, read one at a time (one nc_stream_next call, one
// frame). A frame is described by its kind and request id, and for (UN)SUB the
// subscription it names: what (NW_SUB_*) and gid.
enum { PK_NONE = 0, PK_HELLO, PK_SUB, PK_UNSUB, PK_PING, PK_REQ };
typedef struct {
    int      type;            // PK_*, 0 for none
    uint32_t pid;             // the request id
    int      what;            // SUB, UNSUB
    char     gid[16];
    char     token[NW_ACCESS_MAX + 1];   // HELLO
    char     path[32];        // REQ
    int      body_len;
} cpkt;

static void pending_drop(void) {}

static cpkt next_pkt(int64_t now) {
    cpkt k;
    memset(&k, 0, sizeof k);
    const NcOut *o = nc_stream_next(now);
    if (!o) return k;
    T_CHECK(o->len > 0 && o->len <= NC_OUT_MAX);
    sf_frame f;
    T_EQ_INT(sf_parse(o->bytes, o->len, &f), 0);
    k.pid = f.id;
    switch (f.kind) {
    case SF_HELLO:
        k.type = PK_HELLO;
        snprintf(k.token, sizeof k.token, "%.*s", f.tok_len, (const char *)f.tok);
        break;
    case SF_SUB: case SF_UNSUB:
        k.type = f.kind == SF_SUB ? PK_SUB : PK_UNSUB;
        k.what = f.what;
        snprintf(k.gid, sizeof k.gid, "%.*s", f.gid_len, f.gid);
        break;
    case SF_PING: k.type = PK_PING; break;
    case SF_REQ:
        k.type = PK_REQ;
        snprintf(k.path, sizeof k.path, "%.*s", f.path_len, f.path);
        k.body_len = (int)f.body_len;
        break;
    default: T_CHECK(0);
    }
    return k;
}

// The first event a feed made that is not the session's (a session loaded by
// the test is told with the next feed), or a NONE one; g_at is its index.
static int g_at;
static const NcEvent *first(int n) {
    static NcEvent none;
    for (g_at = 0; g_at < n; g_at++)
        if (nc_event(g_at)->kind != NC_EV_SESSION) return nc_event(g_at);
    memset(&none, 0, sizeof none);
    return &none;
}

static const NcEvent *feed_bytes(const uint8_t *b, int n, int64_t now) { return first(nc_stream_in(b, n, 0, now)); }

// The server's READY (rc 0). A refusal is the server closing the socket: no frame.
static const NcEvent *connack(int rc, int64_t now) {
    uint8_t b[4];
    if (rc != 0) return first(0);
    return feed_bytes(b, sf_put_bare(b, sizeof b, SF_READY), now);
}

// The REPLY to a SUB: status 200 takes it, anything else refuses it.
static const NcEvent *suback(uint32_t pid, int refuse, int n, int64_t now) {
    uint8_t b[16];
    (void)n;
    return feed_bytes(b, sf_put_reply_head(b, sizeof b, pid, refuse ? 403 : 200), now);
}
#define SUBACK_FAILURE 1

// The socket opened, the client's HELLO read (with its token) and accepted.
static cpkt live(int64_t now) {
    pending_drop();
    nc_stream_opened(now);
    cpkt c = next_pkt(now);
    T_EQ_INT(c.type, PK_HELLO);
    connack(0, now);
    return c;
}

// A push the server published, as the frame it is: a cast on the seat's kind, or the
// spectators' when `watch`.
static const NcEvent *feed(const NwFrame *f0, int watch, int64_t now) {
    static uint8_t raw[1100];
    nc_stream_hear_all(1);   // the tests that look at what is dropped turn it off
    NwFrame f = *f0;
    if (watch && f.kind == NW_WS_CAST) f.kind = NW_WS_SPECT;
    int n = nw_frame_encode(&f, raw, sizeof raw);
    T_CHECK(n > 0);
    return feed_bytes(raw, n, now);
}

static int path_is(const NcRequest *q, const char *path) {
    return q && q->path_len == strlen(path) && memcmp(q->path, path, q->path_len) == 0;
}

// The answer to the action the pump has next: over the socket when it is live (the REQ frame
// is read and its REPLY fed back; the done comes from nc_next_done), else over HTTP.
static const NcDone *action_answer(const uint8_t *r, int rn, int64_t now) {
    const NcRequest *q = nc_next_request(now);
    if (q) {
        T_CHECK(path_is(q, "/v1/action"));
        return nc_on_response(id_of(q), 200, r, rn, now);
    }
    cpkt k = next_pkt(now);
    T_CHECK(k.type == PK_REQ && strcmp(k.path, "/v1/action") == 0);
    uint8_t b[128];
    int h = sf_put_reply_head(b, sizeof b, k.pid, 200);
    memcpy(b + h, r, (size_t)rn);
    nc_stream_in(b, h + rn, 0, now);
    return nc_next_done();
}

static NwFrame push(int kind, const char *gid, uint32_t v, const uint8_t *p, uint32_t n) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = (uint8_t)kind;
    f.gid_len = (uint8_t)strlen(gid);
    memcpy(f.gid, gid, f.gid_len);
    f.version = v;
    f.blob.p = p;
    f.blob.len = n;
    return f;
}

// The CONNECT: the token as its password, a client id of the user's, a keep-alive.
static void connect_carries_the_token(void) {
    signed_in(T0 + 3600000);
    // Shut: nothing to send, nothing due.
    T_CHECK(nc_stream_next(T0) == NULL);
    T_EQ_INT(nc_stream_due_ms(T0), -1);
    nc_stream_opened(T0);
    T_EQ_INT(nc_stream_due_ms(T0), 0);
    cpkt c = next_pkt(T0);
    T_EQ_INT(c.type, PK_HELLO);
    T_EQ_STR(c.token, "jwt");
    // Nothing else until the CONNACK: no SUBSCRIBE a refused CONNECT would drop.
    T_EQ_INT(nc_stream_sub(NW_SUB_GAMES, NULL, 0), NW_OK);
    T_EQ_INT(next_pkt(T0).type, 0);
    T_EQ_INT(nc_stream_due_ms(T0), -1);
    T_EQ_INT(connack(0, T0)->kind, NC_EV_NONE);
    cpkt s = next_pkt(T0);
    T_CHECK(s.type == PK_SUB && s.what == NW_SUB_GAMES);
    // Another connection, in the same millisecond: another client id (two
    // tabs never share one, or the broker would drop the older).
    nc_stream_closed(T0);
    nc_stream_opened(T0);
    cpkt c2 = next_pkt(T0);
    T_CHECK(c2.type == PK_HELLO);
    // A CONNACK refusing it is no subscription either; the broker closes the
    // socket, and the next CONNECT waits for a refreshed token.
    connack(1, T0 + 1);
    T_EQ_INT(next_pkt(T0 + 1).type, 0);
    nc_stream_closed(T0 + 1);
    nc_stream_opened(T0 + 2);
    T_EQ_INT(next_pkt(T0 + 2).type, 0);
    const NcRequest *q = nc_next_request(T0 + 2);
    T_CHECK(q && q->is_refresh);
    uint8_t b[256];
    NwSession ses;
    memset(&ses, 0, sizeof ses);
    ses.access_len = 4; memcpy(ses.access, "jwt3", 4);
    ses.refresh_len = 3; memcpy(ses.refresh, "rt3", 3);
    ses.expires_at_ms = T0 + 3600000;
    ses.user_id_len = 2; memcpy(ses.user_id, "u1", 2);
    ses.username_len = 3; memcpy(ses.username, "ANA", 3);
    if (q) nc_on_response(id_of(q), 200, b, nw_session_encode(&ses, b, sizeof b), T0 + 2);
    cpkt c3 = next_pkt(T0 + 2);
    T_CHECK(c3.type == PK_HELLO && strcmp(c3.token, "jwt3") == 0);
    // A second CONNACK is not the protocol.
    connack(0, T0 + 2);
    T_EQ_INT(connack(0, T0 + 2)->kind, NC_EV_BAD);
    // Signed out: nothing at all.
    nc_reset();
    nc_stream_opened(T0);
    T_CHECK(nc_stream_next(T0) == NULL);
}

// A token about to expire is refreshed before the CONNECT carries it: the pump
// sends the refresh itself, and the CONNECT goes with the new token.
static void connect_waits_for_a_fresh_token(void) {
    signed_in(T0 + NC_REFRESH_EARLY_MS / 2);
    nc_stream_opened(T0);
    T_EQ_INT(next_pkt(T0).type, 0);
    T_EQ_INT(nc_stream_due_ms(T0), -1);
    T_EQ_INT(nc_pump_due_ms(T0), 0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(q && q->is_refresh);
    T_CHECK(nc_next_request(T0) == NULL);   // one refresh at a time
    T_EQ_INT(nc_pump_due_ms(T0), -1);
    uint8_t b[256];
    NwSession ses;
    memset(&ses, 0, sizeof ses);
    ses.access_len = 4; memcpy(ses.access, "jwt2", 4);
    ses.refresh_len = 3; memcpy(ses.refresh, "rt2", 3);
    ses.expires_at_ms = T0 + 3600000;
    ses.user_id_len = 2; memcpy(ses.user_id, "u1", 2);
    ses.username_len = 3; memcpy(ses.username, "ANA", 3);
    int n = nw_session_encode(&ses, b, sizeof b);
    T_CHECK(nc_on_response(id_of(q), 200, b, n, T0) == NULL);
    T_EQ_INT(nc_stream_due_ms(T0), 0);
    cpkt c = next_pkt(T0);
    T_CHECK(c.type == PK_HELLO && strcmp(c.token, "jwt2") == 0);
    T_CHECK(nc_next_request(T0) == NULL);   // connected: no refresh wanted
    // A refresh that fails is tried again later, as the pump's always is.
    signed_in(T0 + NC_REFRESH_EARLY_MS / 2);
    nc_stream_opened(T0);
    q = nc_next_request(T0);
    T_CHECK(q && q->is_refresh);
    nc_on_response(id_of(q), 0, NULL, 0, T0);
    T_CHECK(nc_next_request(T0) == NULL);
    T_CHECK(nc_pump_due_ms(T0) > 0);
    q = nc_next_request(T0 + NC_RETRY_FIRST_MS);
    T_CHECK(q && q->is_refresh);
}

// A socket closed before its CONNACK is the broker's refusal: the token is
// refreshed before the next CONNECT, but not more than once every
// NC_FORCE_REFRESH_MS.
static void closed_before_connack_refreshes(void) {
    signed_in(T0 + 3600000);
    nc_stream_opened(T0);
    T_EQ_INT(next_pkt(T0).type, PK_HELLO);
    nc_stream_closed(T0);
    nc_stream_opened(T0 + 500);
    T_EQ_INT(next_pkt(T0 + 500).type, 0);   // waits for the refresh
    const NcRequest *q = nc_next_request(T0 + 500);
    T_CHECK(q && q->is_refresh);
    uint8_t b[256];
    NwSession ses;
    memset(&ses, 0, sizeof ses);
    ses.access_len = 3; memcpy(ses.access, "jw2", 3);
    ses.refresh_len = 3; memcpy(ses.refresh, "rt2", 3);
    ses.expires_at_ms = T0 + 3600000;
    ses.user_id_len = 2; memcpy(ses.user_id, "u1", 2);
    ses.username_len = 3; memcpy(ses.username, "ANA", 3);
    nc_on_response(id_of(q), 200, b, nw_session_encode(&ses, b, sizeof b), T0 + 500);
    T_EQ_INT(next_pkt(T0 + 500).type, PK_HELLO);
    // Refused again at once: no second refresh inside NC_FORCE_REFRESH_MS.
    nc_stream_closed(T0 + 600);
    nc_stream_opened(T0 + 1000);
    T_EQ_INT(next_pkt(T0 + 1000).type, PK_HELLO);
    T_CHECK(nc_next_request(T0 + 1000) == NULL);
    nc_stream_closed(T0 + NC_FORCE_REFRESH_MS + 1);   // refused again, past the window: refreshed again
    nc_stream_opened(T0 + NC_FORCE_REFRESH_MS + 1000);
    T_EQ_INT(next_pkt(T0 + NC_FORCE_REFRESH_MS + 1000).type, 0);
    // A socket that closes after its CONNACK refreshes nothing.
    signed_in(T0 + 3600000);
    live(T0);
    nc_stream_closed(T0);
    nc_stream_opened(T0);
    T_EQ_INT(next_pkt(T0).type, PK_HELLO);
}

static void stream_order_and_retries(void) {
    signed_in(T0 + 3600000);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAMES, NULL, 0), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, NULL, 0), NW_E_WIRE);
    T_EQ_INT(nc_stream_sub(9, "g1", 2), NW_E_WIRE);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g/1", 3), NW_E_WIRE);   // not a topic level
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g+", 2), NW_E_WIRE);
    live(T0);
    // Each subscription its own SUB frame.
    cpkt s1 = next_pkt(T0), s2 = next_pkt(T0);
    T_CHECK(s1.type == PK_SUB && s1.what == NW_SUB_GAMES);
    T_CHECK(s2.type == PK_SUB && s2.what == NW_SUB_GAME && s2.pid != s1.pid);
    T_EQ_INT(next_pkt(T0).type, 0);
    T_EQ_INT(nc_stream_due_ms(T0), NC_ACK_WAIT_MS);   // unanswered, they are asked again then

    // Answered: the game's says whether the user sits there, as the book knows.
    T_EQ_INT(suback(s1.pid, 0, 1, T0)->kind, NC_EV_NONE);
    const NcEvent *e = suback(s2.pid, 0, 2, T0);
    T_CHECK(e->kind == NC_EV_SEATED && e->seated == 0 && e->gid_len == 2);
    // A SUBACK nobody waits on is nothing.
    T_EQ_INT(suback(999, 0, 1, T0)->kind, NC_EV_NONE);

    // A refused chat is retried after 0.5 s, then 1 s, then 2 s.
    T_EQ_INT(nc_stream_sub(NW_SUB_CHAT, "g1", 2), NW_OK);
    cpkt c = next_pkt(T0);
    T_CHECK(c.type == PK_SUB && c.what == NW_SUB_CHAT);
    e = suback(c.pid, SUBACK_FAILURE, 1, T0);
    T_CHECK(e->kind == NC_EV_REFUSED && e->what == NW_SUB_CHAT);
    T_EQ_INT(next_pkt(T0 + NC_RETRY_FIRST_MS - 1).type, 0);
    T_EQ_INT(nc_stream_due_ms(T0), NC_RETRY_FIRST_MS);
    c = next_pkt(T0 + NC_RETRY_FIRST_MS);
    T_CHECK(c.type == PK_SUB && c.what == NW_SUB_CHAT);
    suback(c.pid, SUBACK_FAILURE, 1, T0 + 1000);
    T_EQ_INT(next_pkt(T0 + 1000 + 2 * NC_RETRY_FIRST_MS - 1).type, 0);
    cpkt c3 = next_pkt(T0 + 1000 + 2 * NC_RETRY_FIRST_MS);
    T_EQ_INT(c3.type, PK_SUB);
    // Held twice, let go once: still held, and nothing is sent.
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(next_pkt(T0 + 2000).type, 0);
    // Let go and held again: asked again (the pending unsubscribe is moot).
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    cpkt s3 = next_pkt(T0 + 2000);
    T_CHECK(s3.type == PK_SUB && s3.what == NW_SUB_GAME);
    T_EQ_INT(next_pkt(T0 + 2000).type, 0);

    // Unsubscribing a live subscription tells the broker, every topic it held;
    // one never sent does not.
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAMES, NULL, 0), NW_OK);
    cpkt u = next_pkt(T0 + 2000);
    T_CHECK(u.type == PK_UNSUB && u.what == NW_SUB_GAMES);
    suback(s3.pid, 0, 2, T0 + 2000);   // answered, so neither is asked again below
    suback(c3.pid, 0, 1, T0 + 2000);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);
    u = next_pkt(T0 + 2000);
    T_CHECK(u.type == PK_UNSUB && u.what == NW_SUB_GAME);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g9", 2), NW_OK);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g9", 2), NW_OK);   // never sent: nothing to take back
    while (next_pkt(T0 + 2000).type) {}

    // The PINGREQ, once nothing went out for NC_PING_MS.
    T_EQ_INT(next_pkt(T0 + 2000 + NC_PING_MS - 1).type, 0);
    T_EQ_INT(nc_stream_due_ms(T0 + 2000), NC_PING_MS);
    T_EQ_INT(next_pkt(T0 + 2000 + NC_PING_MS).type, PK_PING);
    T_EQ_INT(next_pkt(T0 + 2000 + NC_PING_MS).type, 0);
    uint8_t pr[2];
    T_EQ_INT(feed_bytes(pr, sf_put_bare(pr, sizeof pr, SF_PONG), T0)->kind, NC_EV_NONE);

    // Closed: the reopen waits 0.5 s, doubling to 10 s, and starts over once a CONNACK arrives.
    T_EQ_INT(nc_stream_closed(T0), NC_RETRY_FIRST_MS);
    T_EQ_INT(nc_stream_closed(T0), 2 * NC_RETRY_FIRST_MS);
    for (int i = 0; i < 10; i++) nc_stream_closed(T0);
    T_EQ_INT(nc_stream_closed(T0), NC_RETRY_MAX_MS);
    T_CHECK(nc_stream_next(T0) == NULL);
    live(T0);
    T_EQ_INT(nc_stream_closed(T0), NC_RETRY_FIRST_MS);
    // Reconnected: every subscription again.
    live(T0);
    int subs = 0;
    for (cpkt x; (x = next_pkt(T0)).type == PK_SUB;) subs++;
    T_EQ_INT(subs, 1);   // the chat; the dashboard and the games were let go
}

// A page holds its streams before its kept session is restored (the web's
// providers know the user without the kernel): the holds are that user's.
static void holds_made_before_the_session_are_kept(void) {
    nc_reset();   // the book is nobody's again
    T_EQ_INT(nc_stream_sub(NW_SUB_GAMES, NULL, 0), NW_OK);
    NwSession s;
    memset(&s, 0, sizeof s);
    s.access_len = 3; memcpy(s.access, "jwt", 3);
    s.refresh_len = 2; memcpy(s.refresh, "rt", 2);
    s.expires_at_ms = T0 + 3600000;
    s.user_id_len = 2; memcpy(s.user_id, "u1", 2);
    uint8_t b[64];
    T_EQ_INT(nc_session_load(b, nw_session_encode(&s, b, sizeof b)), NW_OK);
    live(T0);
    cpkt k = next_pkt(T0);
    T_CHECK(k.type == PK_SUB && k.what == NW_SUB_GAMES);
    // Another user's session afterwards: their book starts from nothing.
    memcpy(s.user_id, "u2", 2);
    T_EQ_INT(nc_session_load(b, nw_session_encode(&s, b, sizeof b)), NW_OK);
    live(T0);
    T_EQ_INT(next_pkt(T0).type, 0);
}

// What the broker sends that is not the protocol, or not this user's.
static void what_is_not_the_brokers(void) {
    signed_in(T0 + 3600000);
    nc_stream_opened(T0);
    next_pkt(T0);
    const uint8_t env[] = { 1, 2, 3 };
    NwFrame v = push(NW_WS_VIEW, "g1", 5, env, 3);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAMES, NULL, 0), NW_OK);
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_BAD);   // a push before the READY
    connack(0, T0);
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    // A text message, bytes that are not a frame, a frame only a client writes.
    T_EQ_INT(first(nc_stream_in((const uint8_t *)"\xD0\x00", 2, 1, T0))->kind, NC_EV_BAD);   // a PINGRESP, as text
    T_EQ_INT(first(nc_stream_in((const uint8_t *)"\x01\x05", 2, 0, T0))->kind, NC_EV_BAD);
    T_EQ_INT(first(nc_stream_in((const uint8_t *)"\x01\x42", 2, 0, T0))->kind, NC_EV_BAD);   // a PING
    // A push for a game nobody here holds is not for this client (dropped, no event);
    // a payload that is not a push, and a frame only a client writes, are BAD.
    static uint8_t raw[256];
    nc_stream_hear_all(0);
    NwFrame other = push(NW_WS_VIEW, "zz9", 6, env, 1);
    int rn = nw_frame_encode(&other, raw, sizeof raw);
    T_CHECK(rn > 0);
    T_EQ_INT(nc_stream_in(raw, rn, 0, T0) == 0 || first(nc_stream_in(raw, rn, 0, T0))->kind == NC_EV_NONE, 1);
    T_EQ_INT(feed_bytes((const uint8_t *)"\x01\x33\x01", 3, T0)->kind, NC_EV_BAD);   // a view frame cut short
    uint8_t cf[16];
    T_EQ_INT(feed_bytes(cf, sf_put_sub(cf, sizeof cf, SF_SUB, 1, 1, "", 0), T0)->kind, NC_EV_BAD);
    // After junk the stream still reads: the buffer starts over.
    v.version = 7;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    // More than the client ever takes in one go: BAD, and the buffer is let go.
    static uint8_t huge[NC_MQ_IN_MAX + 1];
    T_EQ_INT(first(nc_stream_in(huge, (int)sizeof huge, 0, T0))->kind, NC_EV_BAD);
    v.version = 8;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    T_EQ_INT(first(nc_stream_in((const uint8_t *)"", 0, 0, T0))->kind, NC_EV_BAD);   // an empty message is no frame
    T_EQ_INT(first(nc_stream_in(NULL, 3, 0, T0))->kind, NC_EV_BAD);
}

static void gates(void) {
    signed_in(T0 + 3600000);
    live(T0);
    const uint8_t env[] = { 1, 2, 3 };
    NwFrame v = push(NW_WS_VIEW, "g1", 5, env, 3);
    const NcEvent *e = feed(&v, 0, T0);
    T_CHECK(e->kind == NC_EV_VIEW && e->version == 5 && e->payload_len == 3 && e->what == NW_SUB_GAMES);
    T_EQ_MEM(nc_event_payload(g_at), e->payload_len, env, 3);
    // At or below what was applied: stale.
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_NONE);
    v.version = 4;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_NONE);
    v.version = 6;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    // Casts have a gate of their own.
    NwFrame c = push(NW_WS_CAST, "g1", 6, env, 1);
    T_EQ_INT(feed(&c, 0, T0)->kind, NC_EV_CAST);
    T_EQ_INT(feed(&c, 0, T0)->kind, NC_EV_NONE);
    // Another game is its own.
    NwFrame o = push(NW_WS_VIEW, "g2", 1, env, 3);
    T_EQ_INT(feed(&o, 0, T0)->kind, NC_EV_VIEW);
    // Held on its own stream, a game's dashboard views are not handed out (its
    // casts animate it), and do not count as applied; let go, they are again.
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g2", 2), NW_OK);
    o.version = 2;
    T_EQ_INT(feed(&o, 0, T0)->kind, NC_EV_NONE);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g2", 2), NW_OK);
    T_EQ_INT(feed(&o, 0, T0)->kind, NC_EV_VIEW);
    // The resync: the gate forgets, and an older view applies.
    nc_gate_reset("g1", 2);
    v.version = 2;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    // The seat is gone: always applied, and the gate starts over.
    NwFrame gone = push(NW_WS_VIEW, "g1", 0, NULL, 0);
    e = feed(&gone, 0, T0);
    T_CHECK(e->kind == NC_EV_VIEW && e->payload_len == 0);
    v.version = 1;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    // Many games: the least recently seen gives way, and nothing breaks.
    char gid[16];
    for (int i = 0; i < NC_GATES_MAX + 5; i++) {
        snprintf(gid, sizeof gid, "x%d", i);
        NwFrame w = push(NW_WS_VIEW, gid, 1, env, 3);
        T_EQ_INT(feed(&w, 0, T0)->kind, NC_EV_VIEW);
    }
    NwFrame chat;
    memset(&chat, 0, sizeof chat);
    chat.kind = NW_WS_CHAT;
    chat.gid_len = 2;
    memcpy(chat.gid, "g1", 2);
    chat.chat.message_len = 2;
    memcpy(chat.chat.message, "hi", 2);
    e = feed(&chat, 0, T0);
    T_CHECK(e->kind == NC_EV_CHAT && e->chat.message_len == 2 && memcmp(e->chat.message, "hi", 2) == 0);
}

// ---- the pump ------------------------------------------------------------------------

// A session answer, as the server writes it.
static int session_body(uint8_t *b, int cap, const char *access, uint64_t expires) {
    NwSession s;
    memset(&s, 0, sizeof s);
    s.access_len = (uint16_t)strlen(access); memcpy(s.access, access, s.access_len);
    s.refresh_len = 3; memcpy(s.refresh, "rt2", 3);
    s.expires_at_ms = expires;
    s.user_id_len = 2; memcpy(s.user_id, "u1", 2);
    s.username_len = 3; memcpy(s.username, "ANA", 3);
    return nw_session_encode(&s, b, cap);
}

static NcIntent intent(int kind, const char *gid) {
    NcIntent in;
    memset(&in, 0, sizeof in);
    in.kind = (uint8_t)kind;
    if (gid) { in.gid_len = (uint8_t)strlen(gid); memcpy(in.gid, gid, in.gid_len); }
    in.intent = -1;
    return in;
}

static int refusal_body(uint8_t *b, int why, int code) {
    AskRefusal r;
    memset(&r, 0, sizeof r);
    r.why = (uint8_t)why;
    r.code = code;
    return ask_refusal_encode(&r, b, 16);
}

static void pump_signs_in_and_reads_answers(void) {
    nc_reset();
    // Nobody signed in: an intent that needs the user is refused at once; the leaderboard is not.
    NcIntent games = intent(NC_DO_GAMES, NULL);
    T_EQ_INT(nc_do(&games, T0), 0);
    T_EQ_INT(nc_error()->why, ASK_WHY_AUTH);
    T_EQ_INT(nc_pump_due_ms(T0), -1);

    NcIntent in = intent(NC_DO_SIGN_IN, NULL);
    in.name_len = 3; memcpy(in.name, "ANA", 3);
    in.password_len = 2; memcpy(in.password, "pw", 2);
    uint32_t t = nc_do(&in, T0);
    T_CHECK(t != 0);
    T_EQ_INT(nc_pump_due_ms(T0), 0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/signin") && q->method == NC_POST && q->auth_len == 0 && q->id != 0);
    const uint8_t signin[] = { 1, NW_SIGNIN, 3, 'A', 'N', 'A', 2, 'p', 'w' };
    T_EQ_MEM(q->body, q->body_len, signin, sizeof signin);
    uint32_t id = id_of(q);
    T_CHECK(nc_next_request(T0) == NULL);   // nothing else to send
    uint8_t b[256];
    int n = session_body(b, sizeof b, "jwt", T0 + 3600000);
    const NcDone *d = nc_on_response(id, 200, b, n, T0);
    T_CHECK(d && d->ticket == t && d->kind == NC_DO_SIGN_IN && d->why == 0);
    T_EQ_INT(nc_session_info()->signed_in, 1);
    T_CHECK(nc_on_response(id, 200, b, n, T0) == NULL);   // the same answer twice finishes nothing

    // A move: the request is the kernel's, the answer read as a table response.
    NcIntent play = intent(NC_DO_PLAY, "ab12cd");
    play.bytes_len = 2; play.bytes[0] = 3; play.bytes[1] = 0;
    play.intent = 9;
    t = nc_do(&play, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/action"));
    T_EQ_MEM(q->auth, q->auth_len, "Bearer jwt", 10);
    const uint8_t action[] = { 2, 6, 'a', 'b', '1', '2', 'c', 'd', 9, 0, 0, 0, 3, 0 };
    T_EQ_MEM(q->body, q->body_len, action, sizeof action);
    uint8_t r[TABLE_RESP_BYTES];
    T_EQ_INT(table_action_response(TABLE_REJECTED, 4, 11, r, sizeof r), TABLE_RESP_BYTES);
    d = nc_on_response(id_of(q), 200, r, sizeof r, T0);
    T_CHECK(d && d->ticket == t && d->why == 0 && d->action_status == TABLE_STATUS_REJECTED && d->reject_code == 4 && d->version == 11);

    // One game: the view's version and the envelope's span.
    NcIntent open = intent(NC_DO_OPEN_GAME, "ab12cd");
    t = nc_do(&open, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/ab12cd") && q->method == NC_GET);
    const uint8_t env[] = { 0xe1, 0xe2, 0xe3 };
    NwBlob eb = { env, 3 };
    n = nw_view_encode(7, eb, b, sizeof b);
    d = nc_on_response(id_of(q), 200, b, n, T0);
    T_CHECK(d && d->version == 7 && d->a_len == 3);
    if (d) T_EQ_MEM(b + d->a_off, d->a_len, env, 3);
    // A refusal is read as one.
    t = nc_do(&open, T0);
    q = nc_next_request(T0);
    n = refusal_body(b, ASK_WHY_NOT_FOUND, 0);
    d = nc_on_response(id_of(q), 404, b, n, T0);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_NOT_FOUND && d->status == 404);

    // A list: its rows and their kind; the history carries the rating.
    NcIntent hist = intent(NC_DO_HISTORY, NULL);
    t = nc_do(&hist, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/history"));
    n = nw_history_head(1, 1234, 0, b, sizeof b);
    d = nc_on_response(id_of(q), 200, b, n, T0);
    T_CHECK(d && d->list_kind == NW_HISTORY && d->n_rows == 0 && d->has_rating == 1 && d->rating == 1234);
}

static void pump_refreshes_first_and_after_a_401(void) {
    signed_in(T0 + 30000);   // within NC_REFRESH_EARLY_MS of expiring
    NcIntent games = intent(NC_DO_GAMES, NULL), board = intent(NC_DO_LEADERBOARD, NULL);
    uint32_t tg = nc_do(&games, T0), tb = nc_do(&board, T0);
    T_CHECK(tg && tb);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/refresh"));
    uint32_t refresh = id_of(q);
    // While it is out, only what needs nobody goes.
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/leaderboard"));
    uint32_t board_id = id_of(q);
    T_CHECK(nc_next_request(T0) == NULL);
    T_EQ_INT(nc_pump_due_ms(T0), -1);   // the refresh's answer wakes the rest
    uint8_t b[256];
    int n = session_body(b, sizeof b, "jwt2", T0 + 3600000);
    T_CHECK(nc_on_response(refresh, 200, b, n, T0) == NULL);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games"));
    T_EQ_MEM(q->auth, q->auth_len, "Bearer jwt2", 11);
    uint32_t games_id = id_of(q);
    n = nw_list_head(NW_BOARD, 0, b, sizeof b);
    const NcDone *d = nc_on_response(board_id, 200, b, n, T0);
    T_CHECK(d && d->ticket == tb && d->list_kind == NW_BOARD);

    // A 401: refresh, then the same request once more.
    T_CHECK(nc_on_response(games_id, 401, NULL, 0, T0) == NULL);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/refresh"));
    n = session_body(b, sizeof b, "jwt3", T0 + 3600000);
    nc_on_response(id_of(q), 200, b, n, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games"));
    T_EQ_MEM(q->auth, q->auth_len, "Bearer jwt3", 11);
    // A second 401 is the answer.
    d = nc_on_response(id_of(q), 401, NULL, 0, T0);
    T_CHECK(d && d->ticket == tg && d->why == ASK_WHY_AUTH);

    // A refresh refused for its token ends the session and what waited on it.
    signed_in(T0 + 30000);
    tg = nc_do(&games, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/refresh"));
    n = refusal_body(b, ASK_WHY_ACCOUNT, NW_ACCOUNT_E_BAD_REFRESH);
    T_CHECK(nc_on_response(id_of(q), 400, b, n, T0) == NULL);
    T_EQ_INT(nc_session_info()->signed_in, 0);
    T_EQ_INT(nc_pump_due_ms(T0), 0);
    d = nc_next_done();
    T_CHECK(d && d->ticket == tg && d->why == ASK_WHY_AUTH);
    T_CHECK(nc_next_done() == NULL);
}

static void pump_retries_reads_not_writes(void) {
    signed_in(T0 + 3600000);
    NcIntent games = intent(NC_DO_GAMES, NULL);
    uint32_t t = nc_do(&games, T0);
    int64_t now = T0;
    for (int k = 1; k < NC_PUMP_TRIES; k++) {
        const NcRequest *q = nc_next_request(now);
        T_CHECK(path_is(q, "/v1/games"));
        T_CHECK(nc_on_response(id_of(q), 0, NULL, 0, now) == NULL);   // it never arrived
        int due = nc_pump_due_ms(now);
        T_CHECK(due > 0 && due <= NC_RETRY_MAX_MS);
        T_CHECK(nc_next_request(now + due - 1) == NULL);           // not before its backoff
        now += due;
    }
    const NcRequest *q = nc_next_request(now);
    T_CHECK(path_is(q, "/v1/games"));
    const NcDone *d = nc_on_response(id_of(q), 0, NULL, 0, now);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_INTERNAL && d->status == 0);

    // A write that never arrived is not sent again: it may have landed.
    NcIntent create = intent(NC_DO_CREATE, NULL);
    t = nc_do(&create, now);
    q = nc_next_request(now);
    T_CHECK(path_is(q, "/v1/create"));
    d = nc_on_response(id_of(q), 0, NULL, 0, now);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_INTERNAL);
    T_CHECK(nc_next_request(now + NC_RETRY_MAX_MS) == NULL);

    // At most NC_PUMP_MAX wait at once: one more is refused at once as busy
    // (no status: it never left), not as a server failure.
    for (int i = 0; i < NC_PUMP_MAX; i++) T_CHECK(nc_do(&games, now) != 0);
    T_EQ_INT(nc_do(&games, now), 0);
    T_EQ_INT(nc_error()->why, ASK_WHY_BUSY);
    T_EQ_INT(nc_error()->status, 0);
}

static void pump_signs_out(void) {
    signed_in(T0 + 3600000);
    NcIntent games = intent(NC_DO_GAMES, NULL), out = intent(NC_DO_SIGN_OUT, NULL);
    out.flag = 1;
    uint32_t to = nc_do(&out, T0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/signout"));
    const uint8_t out_g[] = { 1, NW_SIGNOUT, NW_SCOPE_GLOBAL };
    T_EQ_MEM(q->body, q->body_len, out_g, sizeof out_g);
    uint32_t tg = nc_do(&games, T0);   // still signed in when asked
    T_CHECK(tg != 0);
    // Whatever the server answers, the session is over here, and so is what waited for it.
    const NcDone *d = nc_on_response(id_of(q), 500, NULL, 0, T0);
    T_CHECK(d && d->ticket == to);
    T_EQ_INT(nc_session_info()->signed_in, 0);
    d = nc_next_done();
    T_CHECK(d && d->ticket == tg && d->why == ASK_WHY_AUTH);
}

// ---- one state, two feeds ----------------------------------------------------------------

// What a host does with what changed: the board it shows for one game (the
// last payload it was handed for the table, and its version), its dashboard
// row, and every event kind it was told.
typedef struct {
    uint8_t  table[64];  int table_len;  uint32_t table_v;
    uint8_t  row[64];    int row_len;    uint32_t row_v;
    int      views, casts, refetched, sessions, tickets, stale;
} host_t;

// Every event the last feed made: nc_event(i) until NULL, as a host reads them.
static void host_events(host_t *h) {
    const NcEvent *e;
    for (int i = 0; (e = nc_event(i)); i++) {
        const uint8_t *p = nc_event_payload(i);
        int len = (int)e->payload_len;
        if (e->kind == NC_EV_SESSION) h->sessions++;
        if (e->kind == NC_EV_CAST || (e->kind == NC_EV_VIEW && e->what == NW_SUB_GAME)) {
            if (e->kind == NC_EV_CAST) h->casts++; else h->refetched++;
            if (len) memcpy(h->table, p, (size_t)len);
            h->table_len = len; h->table_v = e->version;
        } else if (e->kind == NC_EV_VIEW) {
            h->views++;
            if (len) memcpy(h->row, p, (size_t)len);   // a gone seat's view has no payload
            h->row_len = len; h->row_v = e->version;
        }
    }
}

static void host_done(host_t *h, const NcDone *d, const uint8_t *body) {
    if (d) h->tickets++;
    if (d && d->stale) h->stale++;
    if (!d || d->why || d->stale || !d->a_len) return;
    memcpy(h->table, body + d->a_off, d->a_len); h->table_len = (int)d->a_len; h->table_v = d->version;
}

// A push on its topic (a cast on the seat's, or the spectators' when `watch`).
static void host_push_on(host_t *h, int kind, const char *gid, uint32_t v, const uint8_t *p, uint32_t n, int watch,
                         int64_t now) {
    NwFrame f = push(kind, gid, v, p, n);
    feed(&f, watch, now);
    host_events(h);
}

static void host_push(host_t *h, int kind, const char *gid, uint32_t v, const uint8_t *p, uint32_t n, int64_t now) {
    host_push_on(h, kind, gid, v, p, n, 0, now);
}

static void host_answer(host_t *h, uint32_t id, uint32_t v, const uint8_t *env, uint32_t n, int64_t now) {
    static uint8_t b[256];
    NwBlob eb = { env, n };
    int len = nw_view_encode(v, eb, b, sizeof b);
    const NcDone *d = nc_on_response(id, 200, b, len, now);
    host_events(h);
    host_done(h, d, b);
}

// The one the coordinator asked for: a push and a stale answer for the same game
// reach the client in both orders, and the board ends the same.
static void a_cast_and_a_stale_answer_end_alike(void) {
    const uint8_t old_env[] = { 0xe4 }, cast5[] = { 0x55, 0x05 }, view5[] = { 0x5f };
    host_t end[2][2];
    for (int kind = 0; kind < 2; kind++) {           // the push: a cast (the table's), a view (the dashboard's)
        for (int order = 0; order < 2; order++) {    // 0: the push first; 1: the answer first
            signed_in(T0 + 3600000);
            live(T0);
            host_t *h = &end[kind][order];
            memset(h, 0, sizeof *h);
            // The table already holds version 4 (a cast).
            uint8_t c4[] = { 0x44 };
            host_push(h, NW_WS_CAST, "g1", 4, c4, 1, T0);
            NcIntent open = intent(NC_DO_OPEN_GAME, "g1");
            T_CHECK(nc_do(&open, T0) != 0);
            const NcRequest *q = nc_next_request(T0);
            T_CHECK(path_is(q, "/v1/games/g1"));
            uint32_t id = id_of(q);
            // The server answered at 4, then committed 5 and pushed it: the
            // answer is stale whichever reaches the client first.
            if (order == 0) {
                host_push(h, kind == 0 ? NW_WS_CAST : NW_WS_VIEW, "g1", 5, kind == 0 ? cast5 : view5, kind == 0 ? 2 : 1, T0);
                host_answer(h, id, 4, old_env, 1, T0);
            } else {
                host_answer(h, id, 4, old_env, 1, T0);
                host_push(h, kind == 0 ? NW_WS_CAST : NW_WS_VIEW, "g1", 5, kind == 0 ? cast5 : view5, kind == 0 ? 2 : 1, T0);
            }
            T_CHECK(nc_next_request(T0 + 60000) == NULL);   // nothing left to fetch
        }
        host_t *a = &end[kind][0], *b = &end[kind][1];
        T_EQ_INT(a->table_v, b->table_v);
        T_EQ_MEM(a->table, a->table_len, b->table, b->table_len);
        T_EQ_INT(a->row_v, b->row_v);
        T_EQ_MEM(a->row, a->row_len, b->row, b->row_len);
    }
    T_EQ_INT(end[0][0].table_v, 5);   // the cast: the table is at 5 whichever came first
    T_EQ_INT(end[1][0].row_v, 5);     // the view: the dashboard is
}

// A move that ends a game is pushed before its own answer reaches the client
// (the stream has no after-the-answer window): the cast has no room for the
// rating stamp, so the stamped envelope at the cast's own version applies
// once, and only that. The finish screen reads the game for exactly that.
static void a_stamped_answer_applies_at_the_casts_version(void) {
    uint8_t env[64];
    GameRatings gr;
    memset(&gr, 0, sizeof gr);
    gr.n = 2; gr.before[0] = 1000; gr.before[1] = 1000; gr.delta[0] = 16; gr.delta[1] = -16;
    const int at = env_header_write(env, sizeof env, 0, 5, &gr);
    T_CHECK(at > 0);
    env[at + 2] = GAME_STATUS_GAME_OVER;
    env_header_set_view_len(env, at, 3);
    const int n = at + 3;
    uint8_t bare[64];
    memcpy(bare, env, (size_t)n);
    const int bare_at = env_header_write(bare, sizeof bare, 0, 5, NULL);
    bare[bare_at + 2] = GAME_STATUS_GAME_OVER;
    env_header_set_view_len(bare, bare_at, 3);

    signed_in(T0 + 3600000);
    live(T0);
    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t c5[] = { 0x55, 0x05 };
    host_push(&h, NW_WS_CAST, "g1", 5, c5, 2, T0);
    for (int round = 0; round < 3; round++) {
        NcIntent open = intent(NC_DO_OPEN_GAME, "g1");
        T_CHECK(nc_do(&open, T0) != 0);
        const NcRequest *q = nc_next_request(T0);
        T_CHECK(path_is(q, "/v1/games/g1"));
        const int stale = h.stale;
        // Round 0: an unstamped read at the cast's version (stale, as ever).
        // Round 1: the stamped one (applies). Round 2: the stamped one again (stale).
        if (round == 0) host_answer(&h, id_of(q), 5, bare, (uint32_t)(bare_at + 3), T0);
        else host_answer(&h, id_of(q), 5, env, (uint32_t)n, T0);
        T_EQ_INT(h.stale - stale, round == 1 ? 0 : 1);
    }
    T_EQ_INT(h.table_v, 5);
    T_EQ_INT(h.table_len, n);
    T_EQ_MEM(h.table, n, env, n);
}

static void a_skipped_version_refetches(void) {
    signed_in(T0 + 3600000);
    live(T0);
    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t c[] = { 0xc5 }, c7[] = { 0xc7 }, c8[] = { 0xc8 }, env7[] = { 0xe7 };
    host_push(&h, NW_WS_CAST, "g1", 5, c, 1, T0);
    T_CHECK(nc_next_request(T0) == NULL);
    // 6 never came: 7 is not applied on a board it does not follow; the game is fetched.
    host_push(&h, NW_WS_CAST, "g1", 7, c7, 1, T0);
    T_EQ_INT(h.casts, 1);
    T_EQ_INT(h.table_v, 5);
    T_EQ_INT(nc_pump_due_ms(T0), 0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/g1"));
    T_CHECK(q && nc_next_request(T0) == NULL);
    host_push(&h, NW_WS_CAST, "g1", 7, c7, 1, T0);
    T_EQ_INT(h.casts, 1);
    T_CHECK(nc_next_request(T0) == NULL);   // one fetch, however many gaps
    // Its answer is the host's as an event (no ticket of its own), and the casts go on from it.
    host_answer(&h, id_of(q), 7, env7, 1, T0);
    T_EQ_INT(h.refetched, 1);
    T_EQ_INT(h.table_v, 7);
    T_EQ_MEM(h.table, h.table_len, env7, 1);
    T_EQ_INT(h.tickets, 0);
    T_CHECK(nc_next_done() == NULL);
    // The host's own read of the game at that very version: stale, nothing to take.
    NcIntent open = intent(NC_DO_OPEN_GAME, "g1");
    nc_do(&open, T0);
    q = nc_next_request(T0);
    const uint8_t again[] = { 0xa7 };
    host_answer(&h, id_of(q), 7, again, 1, T0);
    T_EQ_INT(h.tickets, 1);
    T_EQ_INT(h.stale, 1);
    T_EQ_MEM(h.table, h.table_len, env7, 1);
    // A dashboard view at 9 the table may not have shown: an answer at 9 is taken.
    const uint8_t v9[] = { 0x99 }, e9[] = { 0xe9 };
    host_push(&h, NW_WS_VIEW, "g1", 9, v9, 1, T0);
    nc_do(&open, T0);
    q = nc_next_request(T0);
    host_answer(&h, id_of(q), 9, e9, 1, T0);
    T_EQ_INT(h.stale, 1);
    T_EQ_INT(h.table_v, 9);
    T_EQ_MEM(h.table, h.table_len, e9, 1);
    host_push(&h, NW_WS_CAST, "g1", 7, c7, 1, T0);
    T_EQ_INT(h.casts, 1);
    host_push(&h, NW_WS_CAST, "g1", 10, c8, 1, T0);
    T_EQ_INT(h.casts, 2);
    T_EQ_INT(h.table_v, 10);
}

static void a_refused_move_catches_up(void) {
    uint8_t r[TABLE_RESP_BYTES];
    const uint8_t c[] = { 0xc5 };
    for (int pushed = 0; pushed < 2; pushed++) {
        signed_in(T0 + 3600000);
        live(T0);
        host_t h;
        memset(&h, 0, sizeof h);
        host_push(&h, NW_WS_VIEW, "g1", 5, c, 1, T0);
        NcIntent play = intent(NC_DO_PLAY, "g1");
        play.bytes_len = 2; play.bytes[0] = 3; play.bytes[1] = 0;
        play.intent = 5;
        nc_do(&play, T0);
        // Refused: the server is at 9.
        T_EQ_INT(table_action_response(TABLE_REJECTED, 4, 9, r, sizeof r), TABLE_RESP_BYTES);
        const NcDone *d = action_answer(r, (int)sizeof r, T0);
        T_CHECK(d && d->action_status == TABLE_STATUS_REJECTED);
        // The pushes get a moment to arrive.
        T_EQ_INT(nc_pump_due_ms(T0), NC_RECONCILE_MS);
        T_CHECK(nc_next_request(T0 + NC_RECONCILE_MS - 1) == NULL);
        if (pushed) {
            // They did: nothing is fetched.
            uint8_t c9[] = { 0xc9 };
            host_push(&h, NW_WS_CAST, "g1", 9, c9, 1, T0 + 10);
            T_CHECK(nc_next_request(T0 + NC_RECONCILE_MS) == NULL);
            T_EQ_INT(nc_pump_due_ms(T0 + NC_RECONCILE_MS), -1);
        } else {
            const NcRequest *q = nc_next_request(T0 + NC_RECONCILE_MS);
            T_CHECK(path_is(q, "/v1/games/g1"));
        }
    }
    // A move that applied fetches nothing.
    signed_in(T0 + 3600000);
    NcIntent play = intent(NC_DO_PLAY, "g1");
    play.bytes_len = 2; play.bytes[0] = 3;
    nc_do(&play, T0);
    const NcRequest *q = nc_next_request(T0);
    T_EQ_INT(table_action_response(TABLE_APPLIED, 0, 9, r, sizeof r), TABLE_RESP_BYTES);
    nc_on_response(id_of(q), 200, r, sizeof r, T0);
    T_EQ_INT(nc_pump_due_ms(T0), -1);
}


// An envelope as the server writes one, for `viewer` (a seat, or -1 for a
// spectator) of a one-seat lobby of `gid` at `v`.
static int envelope_as(const char *gid, uint32_t v, int viewer, uint8_t *out, int cap) {
    int at = env_header_write(out, cap, viewer, v, NULL);
    T_CHECK(at > 0);
    env_header_set_view_len(out, at, 2);
    static Roster r;
    memset(&r, 0, sizeof r);
    T_EQ_INT(roster_seat_add(&r, "u1", 2, "ANA", 3, NULL, 0), 0);
    int t = roster_trailer_write(&r, gid, (int)strlen(gid), 0, 0, 1, out + at + 2, cap - at - 2);
    T_CHECK(t > 0);
    return at + 2 + t;
}

// Seat 0's.
static int envelope(const char *gid, uint32_t v, uint8_t *out, int cap) { return envelope_as(gid, v, 0, out, cap); }

// An ask's request, answered with `env`: the NcDone.
static const NcDone *ask_answered(int kind, const char *gid, const uint8_t *env, int n, int64_t now) {
    TableAsk ask;
    memset(&ask, 0, sizeof ask);
    ask.kind = (uint8_t)kind;
    ask.gid_len = (uint8_t)strlen(gid);
    memcpy(ask.gid, gid, ask.gid_len);
    NcIntent in = intent(NC_DO_ASK, NULL);
    int an = ask_encode(&ask, in.bytes, sizeof in.bytes);
    T_CHECK(an > 0);
    in.bytes_len = (uint16_t)an;
    T_CHECK(nc_do(&in, now) != 0);
    const NcRequest *q = nc_next_request(now);
    T_CHECK(path_is(q, "/v1/meta"));
    return q ? nc_on_response(id_of(q), 200, env, n, now) : NULL;
}

// A game subscription that goes live takes the game whole once (its catch-up),
// unless something newer than what is held is applied first; a chat's does not.
static void suback_takes_the_game_whole(void) {
    signed_in(T0 + 3600000);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_CHAT, "g1", 2), NW_OK);
    live(T0);
    cpkt s = next_pkt(T0), c = next_pkt(T0);
    T_CHECK(s.what == NW_SUB_GAME && c.what == NW_SUB_CHAT);
    T_CHECK(nc_next_request(T0) == NULL);   // nothing before the subscription is live
    suback(c.pid, 0, 1, T0);
    T_CHECK(nc_next_request(T0) == NULL);   // a chat's fetches no game
    suback(s.pid, 0, 2, T0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/g1"));
    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t e5[] = { 0xe5 }, c6[] = { 0xc6 };
    host_answer(&h, id_of(q), 5, e5, 1, T0);
    T_EQ_INT(h.refetched, 1);
    T_EQ_INT(h.table_v, 5);
    // Reconnected: live again, and the read is queued, but the next version
    // arriving first (nothing was skipped) makes it moot.
    nc_stream_closed(T0);
    live(T0 + 1);
    while ((s = next_pkt(T0 + 1)).type && s.what != NW_SUB_GAME) {}
    suback(s.pid, 0, 2, T0 + 1);
    host_push(&h, NW_WS_CAST, "g1", 6, c6, 1, T0 + 1);
    T_EQ_INT(h.casts, 1);
    T_CHECK(nc_next_request(T0 + 1) == NULL);
    // ... and a version past the next does not: the game is read.
    nc_stream_closed(T0 + 2);
    live(T0 + 2);
    while ((s = next_pkt(T0 + 2)).type && s.what != NW_SUB_GAME) {}
    suback(s.pid, 0, 2, T0 + 2);
    T_CHECK(path_is(nc_next_request(T0 + 2), "/v1/games/g1"));
}

// A subscription the broker never answers is asked again after
// NC_ACK_WAIT_MS, with a new packet id; its SUBACK then takes the game whole.
static void an_unanswered_subscription_is_asked_again(void) {
    signed_in(T0 + 3600000);
    nc_stream_sub(NW_SUB_GAME, "g1", 2);
    live(T0);
    cpkt s1 = next_pkt(T0);
    T_EQ_INT(s1.type, PK_SUB);
    T_EQ_INT(nc_stream_due_ms(T0), NC_ACK_WAIT_MS);
    T_EQ_INT(next_pkt(T0 + NC_ACK_WAIT_MS - 1).type, 0);
    cpkt s2 = next_pkt(T0 + NC_ACK_WAIT_MS);
    T_EQ_INT(s2.type, PK_SUB);
    T_CHECK(s2.pid != s1.pid);
    T_EQ_INT(suback(s1.pid, 0, 2, T0 + NC_ACK_WAIT_MS)->kind, NC_EV_NONE);   // the first one's, late
    T_CHECK(nc_next_request(T0 + NC_ACK_WAIT_MS) == NULL);
    T_EQ_INT(suback(s2.pid, 0, 2, T0 + NC_ACK_WAIT_MS)->kind, NC_EV_SEATED);
    T_CHECK(path_is(nc_next_request(T0 + NC_ACK_WAIT_MS), "/v1/games/g1"));
    // Answered: never asked again; only the keep-alive goes.
    T_EQ_INT(next_pkt(T0 + 3 * NC_ACK_WAIT_MS + NC_PING_MS).type, PK_PING);
    // Asked again and again, the packet id wraps past 65535 and is never 0.
    signed_in(T0 + 100000000LL);
    nc_stream_sub(NW_SUB_GAMES, NULL, 0);
    live(T0);
    int zero = 0;
    int64_t t = T0;
    for (int i = 0; i < 65540; i++, t += NC_ACK_WAIT_MS) {
        cpkt k = next_pkt(t);
        zero += k.type == PK_SUB && k.pid == 0;
    }
    T_EQ_INT(zero, 0);
}

// A game's subscription hears the seat's casts and the spectators': the book
// takes the seat's while the user sits there, the spectators' otherwise, as
// the last envelope or cast says; a create subscribes the new game itself.
static void seat_and_spectator_casts(void) {
    signed_in(T0 + 3600000);
    live(T0);
    uint8_t b[512];
    // A create: the new game is named, and heard from now on, both its streams.
    NcIntent create = intent(NC_DO_CREATE, NULL);
    nc_do(&create, T0);
    const NcRequest *q = nc_next_request(T0);
    int n = envelope("newg01", 1, b, sizeof b);
    const NcDone *d = nc_on_response(id_of(q), 200, b, n, T0);
    T_CHECK(d && d->why == 0 && d->gid_len == 6 && memcmp(d->gid, "newg01", 6) == 0);
    T_CHECK(d && d->version == 1 && d->a_off == 0 && (int)d->a_len == n && !d->stale);
    cpkt s = next_pkt(T0);
    T_CHECK(s.type == PK_SUB && s.what == NW_SUB_GAME && strcmp(s.gid, "newg01") == 0);
    // The host holds it and lets it go: it is let go.
    nc_stream_sub(NW_SUB_GAME, "newg01", 6);
    nc_stream_unsub(NW_SUB_GAME, "newg01", 6);
    T_EQ_INT(next_pkt(T0).type, PK_UNSUB);

    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t c[] = { 1 };
    // Watching g2: until the book knows the seat, a spectators' cast is a read
    // of the game, never applied (a seated user hears its own cast of it too).
    nc_stream_sub(NW_SUB_GAME, "g2", 2);
    s = next_pkt(T0);
    T_EQ_INT(suback(s.pid, 0, 2, T0)->seated, 0);
    q = nc_next_request(T0);   // the catch-up
    T_CHECK(path_is(q, "/v1/games/g2"));
    uint32_t catch_id = id_of(q);
    host_push_on(&h, NW_WS_CAST, "g2", 7, c, 1, 1, T0);
    T_EQ_INT(h.casts, 0);
    // The read says: a spectator. The spectators' casts apply from then on.
    n = envelope_as("g2", 7, -1, b, sizeof b);
    NwBlob eb = { b, (uint32_t)n };
    static uint8_t vb[600];
    int vn = nw_view_encode(7, eb, vb, sizeof vb);
    nc_on_response(catch_id, 200, vb, vn, T0);
    host_events(&h);
    T_EQ_INT(h.table_v, 7);
    host_push_on(&h, NW_WS_CAST, "g2", 8, c, 1, 1, T0);
    T_EQ_INT(h.casts, 1);
    T_EQ_INT(h.table_v, 8);
    // Joining: the answer is a seat's board, and the seat's casts take over.
    n = envelope("g2", 9, b, sizeof b);
    d = ask_answered(ASK_JOIN, "g2", b, n, T0);
    T_CHECK(d && d->version == 9 && (int)d->a_len == n);
    T_EQ_INT(next_pkt(T0).type, 0);   // nothing to subscribe again: both streams are held
    host_push_on(&h, NW_WS_CAST, "g2", 9, c, 1, 1, T0);   // the join's spectators' cast: not a seat's board
    host_push_on(&h, NW_WS_CAST, "g2", 9, c, 1, 0, T0);   // its seat cast: stale at 9
    T_EQ_INT(h.casts, 1);
    host_push_on(&h, NW_WS_CAST, "g2", 10, c, 1, 1, T0);
    T_EQ_INT(h.casts, 1);
    host_push_on(&h, NW_WS_CAST, "g2", 10, c, 1, 0, T0);
    T_EQ_INT(h.casts, 2);
    // Leaving (an exit of one's own): the answer is the spectators' board, and
    // so are the casts again.
    n = envelope_as("g2", 11, -1, b, sizeof b);
    d = ask_answered(ASK_EXIT, "g2", b, n, T0);
    T_CHECK(d && d->version == 11);
    host_push_on(&h, NW_WS_CAST, "g2", 12, c, 1, 1, T0);
    T_EQ_INT(h.casts, 3);
    // A seat's cast says the user sits there (a join from another device).
    host_push_on(&h, NW_WS_CAST, "g2", 13, c, 1, 0, T0);
    T_EQ_INT(h.casts, 4);
    host_push_on(&h, NW_WS_CAST, "g2", 14, c, 1, 1, T0);
    T_EQ_INT(h.casts, 4);
    // Removed by someone else: the dashboard's empty view; spectators' casts again.
    host_push(&h, NW_WS_VIEW, "g2", 0, NULL, 0, T0);
    host_push_on(&h, NW_WS_CAST, "g2", 15, c, 1, 1, T0);
    T_EQ_INT(h.casts, 5);
    // Any other ask's answer is not a board: its cast animates instead.
    n = envelope("g2", 16, b, sizeof b);
    d = ask_answered(ASK_START, "g2", b, n, T0);
    T_CHECK(d && d->why == 0 && d->gid_len == 2 && d->a_len == 0);
}


static void the_session_is_told_once(void) {
    nc_reset();
    NcIntent in = intent(NC_DO_SIGN_IN, NULL);
    in.name_len = 3; memcpy(in.name, "ANA", 3);
    in.password_len = 2; memcpy(in.password, "pw", 2);
    nc_do(&in, T0);
    const NcRequest *q = nc_next_request(T0);
    uint8_t b[256];
    int n = session_body(b, sizeof b, "jwt", T0 + 30000);
    nc_on_response(id_of(q), 200, b, n, T0);
    host_t h;
    memset(&h, 0, sizeof h);
    host_events(&h);
    T_EQ_INT(h.sessions, 1);
    T_CHECK(nc_event(1) == NULL);
    // A refresh made by the pump itself is told with the answer that took it.
    NcIntent games = intent(NC_DO_GAMES, NULL);
    nc_do(&games, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/refresh"));
    n = session_body(b, sizeof b, "jwt2", T0 + 3600000);
    nc_on_response(id_of(q), 200, b, n, T0);
    T_CHECK(nc_event(0) && nc_event(0)->kind == NC_EV_SESSION);
    // A feed that changed no session tells none.
    q = nc_next_request(T0);
    n = nw_list_head(NW_GAMES, 0, b, sizeof b);
    nc_on_response(id_of(q), 200, b, n, T0);
    T_CHECK(nc_event(0) == NULL);
}

static void another_user_starts_the_book_over(void) {
    signed_in(T0 + 3600000);
    nc_stream_sub(NW_SUB_GAMES, NULL, 0);
    live(T0);
    const uint8_t env[] = { 1 };
    NwFrame v = push(NW_WS_VIEW, "g1", 5, env, 1);
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    // The same user again (a refreshed tab): the book holds.
    uint8_t b[256];
    int n = session_body(b, sizeof b, "jwt9", T0 + 3600000);
    T_EQ_INT(nc_session_load(b, n), NW_OK);
    v.version = 4;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_NONE);
    // Another user: their versions and subscriptions start from nothing.
    NwSession s;
    T_EQ_INT(nw_session_decode(b, n, &s), NW_OK);
    memcpy(s.user_id, "u2", 2);
    n = nw_session_encode(&s, b, sizeof b);
    T_EQ_INT(nc_session_load(b, n), NW_OK);
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    live(T0);
    T_EQ_INT(next_pkt(T0).type, 0);
}

// The bytes an intent carries wait in one shared arena: each request is built
// from its own, a full arena refuses at once, and room let go is used again.
static void waiting_intents_keep_their_bytes(void) {
    signed_in(T0 + 3600000);
    NcIntent chat[3];
    for (int k = 0; k < 3; k++) {
        chat[k] = intent(NC_DO_SEND_CHAT, "g1");
        chat[k].bytes_len = 1000;
        memset(chat[k].bytes, 'a' + k, 1000);
    }
    NcIntent move = intent(NC_DO_PLAY, "g1");
    move.bytes_len = 2; move.bytes[0] = 3; move.bytes[1] = 7;
    T_CHECK(nc_do(&chat[0], T0) != 0);
    T_CHECK(nc_do(&move, T0) != 0);
    T_CHECK(nc_do(&chat[1], T0) != 0);
    T_EQ_INT(nc_do(&chat[2], T0), 0);   // 2,002 bytes wait; 1,000 more do not fit
    T_EQ_INT(nc_error()->why, ASK_WHY_BUSY);
    // Each request carries its own bytes (the chat's: [v][kind][u16 len][message]).
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/chat/g1") && q->body_len == 1004 && q->body[4] == 'a' && q->body[1003] == 'a');
    uint32_t first = id_of(q);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/action") && q->body[q->body_len - 1] == 7);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/chat/g1") && q->body[4] == 'b' && q->body[1003] == 'b');
    // The first chat answered: its room takes the third.
    T_CHECK(nc_on_response(first, 204, NULL, 0, T0) != NULL);
    T_CHECK(nc_do(&chat[2], T0) != 0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/chat/g1") && q->body[4] == 'c' && q->body[1003] == 'c');
}

// The iOS app shows boards and never decodes a cast: a cast is the client's
// cue to take the game whole, a view of a game held on its own stream is still
// handed out, and a create does not subscribe the game's stream.
static void a_host_of_boards_gets_boards(void) {
    signed_in(T0 + 3600000);
    nc_set_boards(1);
    nc_stream_sub(NW_SUB_GAME, "g1", 2);
    live(T0);
    while (next_pkt(T0).type) {}
    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t c[] = { 0xc1 }, v[] = { 0x55 }, env[] = { 0xe6 };
    host_push(&h, NW_WS_VIEW, "g1", 5, v, 1, T0);
    T_EQ_INT(h.views, 1);   // held on its own stream, and handed out all the same
    // A cast at a version already shown: nothing to fetch.
    host_push(&h, NW_WS_CAST, "g1", 5, c, 1, T0);
    T_EQ_INT(h.casts, 0);
    T_CHECK(nc_next_request(T0) == NULL);
    // A newer one: not handed out, and the game is fetched; its board comes as a view.
    host_push(&h, NW_WS_CAST, "g1", 6, c, 1, T0);
    T_EQ_INT(h.casts, 0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/g1"));
    host_answer(&h, id_of(q), 6, env, 1, T0);
    T_EQ_INT(h.refetched, 1);
    T_EQ_INT(h.table_v, 6);
    // Its view arriving first makes the fetch moot.
    host_push(&h, NW_WS_CAST, "g1", 7, c, 1, T0);
    host_push(&h, NW_WS_VIEW, "g1", 7, v, 1, T0);
    T_CHECK(nc_next_request(T0) == NULL);
    // A create subscribes nothing.
    uint8_t b[512];
    NcIntent create = intent(NC_DO_CREATE, NULL);
    nc_do(&create, T0);
    q = nc_next_request(T0);
    int n = envelope("newg02", 1, b, sizeof b);
    T_CHECK(nc_on_response(id_of(q), 200, b, n, T0) != NULL);
    T_EQ_INT(next_pkt(T0).type, 0);
    nc_set_boards(0);
}

// 503 ASK_WHY_BUSY (the server's store was throttled): a read is sent again
// after NC_BUSY_FIRST_MS doubling, NC_BUSY_TRIES times; a write's answer stands.
static void a_busy_read_goes_again_a_write_does_not(void) {
    signed_in(T0 + 3600000);
    uint8_t busy[16];
    int n = refusal_body(busy, ASK_WHY_BUSY, 0);
    NcIntent games = intent(NC_DO_GAMES, NULL);
    uint32_t t = nc_do(&games, T0);
    int64_t now = T0;
    for (int k = 0; k < NC_BUSY_TRIES; k++) {
        const NcRequest *q = nc_next_request(now);
        T_CHECK(path_is(q, "/v1/games"));
        T_CHECK(nc_on_response(id_of(q), 503, busy, n, now) == NULL);
        T_EQ_INT(nc_pump_due_ms(now), NC_BUSY_FIRST_MS << k);
        T_CHECK(nc_next_request(now + (NC_BUSY_FIRST_MS << k) - 1) == NULL);
        now += NC_BUSY_FIRST_MS << k;
    }
    const NcRequest *q = nc_next_request(now);
    const NcDone *d = nc_on_response(id_of(q), 503, busy, n, now);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_BUSY);
    NcIntent create = intent(NC_DO_CREATE, NULL);
    t = nc_do(&create, now);
    q = nc_next_request(now);
    d = nc_on_response(id_of(q), 503, busy, n, now);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_BUSY);
}


// An action goes over the live socket as a REQ frame, and its REPLY is the HTTP answer; before the READY it goes by HTTP;
// a socket that closes with one unanswered answers it as a request that never arrived.
static void actions_ride_the_socket(void) {
    uint8_t r[TABLE_RESP_BYTES];
    NcIntent play = intent(NC_DO_PLAY, "g1");
    play.bytes_len = 2; play.bytes[0] = 3; play.bytes[1] = 0;
    play.intent = 5;
    // Not live yet (opened, no READY): HTTP.
    signed_in(T0 + 3600000);
    nc_stream_opened(T0);
    next_pkt(T0);   // the HELLO
    uint32_t t = nc_do(&play, T0);
    T_CHECK(t != 0);
    T_CHECK(path_is(nc_next_request(T0), "/v1/action"));
    // Live: the pump leaves it to the socket.
    signed_in(T0 + 3600000);
    live(T0);
    t = nc_do(&play, T0);
    T_CHECK(t != 0);
    T_CHECK(nc_next_request(T0) == NULL);
    T_EQ_INT(nc_stream_due_ms(T0), 0);
    cpkt k = next_pkt(T0);
    T_CHECK(k.type == PK_REQ && strcmp(k.path, "/v1/action") == 0 && k.body_len > 0);
    T_EQ_INT(next_pkt(T0).type, PK_NONE);   // sent once
    T_EQ_INT(table_action_response(TABLE_APPLIED, 0, 6, r, sizeof r), TABLE_RESP_BYTES);
    uint8_t b[64];
    int h = sf_put_reply_head(b, sizeof b, k.pid, 200);
    memcpy(b + h, r, sizeof r);
    nc_stream_in(b, h + (int)sizeof r, 0, T0);
    const NcDone *d = nc_next_done();
    T_CHECK(d && d->ticket == t && d->action_status == TABLE_STATUS_APPLIED && d->version == 6);
    T_CHECK(nc_next_done() == NULL);
    // A REPLY for nothing waiting is nothing.
    nc_stream_in(b, h + (int)sizeof r, 0, T0);
    T_CHECK(nc_next_done() == NULL);
    // Written and never answered: the socket closes, and the action ends as a request that never arrived.
    t = nc_do(&play, T0);
    T_CHECK(t != 0 && nc_next_request(T0) == NULL);
    k = next_pkt(T0);
    T_EQ_INT(k.type, PK_REQ);
    nc_stream_closed(T0 + 10);
    d = nc_next_done();
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_INTERNAL);
    // Queued for the socket but not yet written when it closes: it goes by HTTP instead.
    signed_in(T0 + 3600000);
    live(T0 + 20);
    t = nc_do(&play, T0 + 20);
    T_CHECK(t != 0 && nc_next_request(T0 + 20) == NULL);
    nc_stream_closed(T0 + 21);
    T_CHECK(nc_next_done() == NULL);
    T_CHECK(path_is(nc_next_request(T0 + 22), "/v1/action"));
}

// ---- the pump's own work never crowds out the host's ------------------------------------

// A move refused over version 9 of `gid`, its answer read: the reconcile is queued.
static void refused_at_9(const char *gid, int64_t now) {
    uint8_t r[TABLE_RESP_BYTES];
    NcIntent play = intent(NC_DO_PLAY, gid);
    play.bytes_len = 2; play.bytes[0] = 3; play.bytes[1] = 0;
    play.intent = 5;
    uint32_t t = nc_do(&play, now);
    T_CHECK(t != 0);   // a host intent: never refused while only the pump's own work waits
    T_EQ_INT(table_action_response(TABLE_REJECTED, 4, 9, r, sizeof r), TABLE_RESP_BYTES);
    const NcDone *d = action_answer(r, (int)sizeof r, now);
    T_CHECK(d && d->ticket == t && d->action_status == TABLE_STATUS_REJECTED);
}

// Games played back to back faster than NC_RECONCILE_MS, every move refused
// over a newer version (the p3client harness at zero think time): the
// reconciles they queue never refuse the host's next intent, and once they are
// done with, the pump is as it was: NC_PUMP_MAX tickets and the whole arena.
static void fast_games_leave_the_pump_as_it_was(void) {
    signed_in(T0 + 3600000);
    live(T0);
    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t v5[] = { 0x55 }, c9[] = { 0xc9 };
    char gid[16];   // "g" and any int: gcc checks the bound, not the loop
    int64_t now = T0;
    for (int k = 0; k < 40; k++, now += 20) {
        snprintf(gid, sizeof gid, "g%d", k);
        host_push(&h, NW_WS_VIEW, gid, 5, v5, 1, now);
        refused_at_9(gid, now);
        // Every other game's pushes catch up; the rest never come.
        if (k % 2 == 0) host_push(&h, NW_WS_CAST, gid, 9, c9, 1, now + 5);
    }
    // Only the pump's own reconciles wait: the host still has every ticket.
    NcIntent games = intent(NC_DO_GAMES, NULL);
    for (int i = 0; i < NC_PUMP_MAX; i++) T_CHECK(nc_do(&games, now) != 0);
    T_EQ_INT(nc_do(&games, now), 0);
    T_EQ_INT(nc_error()->why, ASK_WHY_BUSY);
    // Everything waiting is sent and answered (a refusal will do): no
    // reconcile for a game that caught up goes out.
    now += NC_RECONCILE_MS;
    int sent = 0, refetches = 0;
    for (const NcRequest *q; (q = nc_next_request(now)); sent++) {
        if (q->path_len > 11 && memcmp(q->path, "/v1/games/g", 11) == 0) {
            refetches++;
            T_CHECK((atoi(q->path + 11) % 2) == 1);
        }
        nc_on_response(id_of(q), 500, NULL, 0, now);
    }
    T_EQ_INT(sent, NC_PUMP_MAX + refetches);
    T_CHECK(refetches > 0);
    while (nc_next_done()) {}
    T_EQ_INT(nc_pump_due_ms(now), -1);
    // Baseline: the whole arena (exactly NC_ARENA_BYTES, 2,560) and every ticket.
    NcIntent chat = intent(NC_DO_SEND_CHAT, "g1");
    chat.bytes_len = 1000;
    memset(chat.bytes, 'a', 1000);
    T_CHECK(nc_do(&chat, now) != 0);
    T_CHECK(nc_do(&chat, now) != 0);
    chat.bytes_len = 560;
    T_CHECK(nc_do(&chat, now) != 0);
    for (int i = 3; i < NC_PUMP_MAX; i++) T_CHECK(nc_do(&games, now) != 0);
    T_EQ_INT(nc_do(&games, now), 0);
}

// A reconcile is let go the moment the game reaches the version it waits for,
// whatever brings it there (the seat's cast, the dashboard's view, an answered
// read), not when its time comes; a step short of it does not.
static void a_reconcile_lets_go_once_caught_up(void) {
    const uint8_t v5[] = { 0x55 }, c6[] = { 0xc6 }, c9[] = { 0xc9 }, v9[] = { 0x99 }, e9[] = { 0xe9 };
    for (int how = 0; how < 3; how++) {
        signed_in(T0 + 3600000);
        live(T0);
        host_t h;
        memset(&h, 0, sizeof h);
        host_push(&h, NW_WS_VIEW, "g1", 5, v5, 1, T0);
        refused_at_9("g1", T0);
        T_EQ_INT(nc_pump_due_ms(T0), NC_RECONCILE_MS);
        host_push(&h, NW_WS_CAST, "g1", 6, c6, 1, T0 + 10);
        T_EQ_INT(nc_pump_due_ms(T0 + 10), NC_RECONCILE_MS - 10);   // 6 is not 9: still waiting
        if (how == 0) {
            host_push(&h, NW_WS_CAST, "g1", 7, c6, 1, T0 + 20);
            host_push(&h, NW_WS_CAST, "g1", 8, c6, 1, T0 + 20);
            host_push(&h, NW_WS_CAST, "g1", 9, c9, 1, T0 + 20);
        } else if (how == 1) {
            host_push(&h, NW_WS_VIEW, "g1", 9, v9, 1, T0 + 20);
        } else {
            NcIntent open = intent(NC_DO_OPEN_GAME, "g1");
            T_CHECK(nc_do(&open, T0 + 20) != 0);
            const NcRequest *q = nc_next_request(T0 + 20);
            T_CHECK(path_is(q, "/v1/games/g1"));
            host_answer(&h, id_of(q), 9, e9, 1, T0 + 20);
            T_EQ_INT(h.table_v, 9);
        }
        T_EQ_INT(nc_pump_due_ms(T0 + 20), -1);
        T_CHECK(nc_next_request(T0 + NC_RECONCILE_MS) == NULL);
    }
}

// A reconcile is let go when the host lets go of the game's subscription (its
// last hold), and only that game's.
static void a_reconcile_lets_go_with_the_subscription(void) {
    signed_in(T0 + 3600000);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    live(T0);
    cpkt s = next_pkt(T0);
    T_CHECK(s.what == NW_SUB_GAME);
    suback(s.pid, 0, 2, T0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/g1"));
    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t e5[] = { 0xe5 }, v5[] = { 0x55 };
    host_answer(&h, id_of(q), 5, e5, 1, T0);
    host_push(&h, NW_WS_VIEW, "g2", 5, v5, 1, T0);
    refused_at_9("g1", T0);
    refused_at_9("g2", T0 + 100);
    T_EQ_INT(nc_pump_due_ms(T0 + 200), NC_RECONCILE_MS - 200);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);   // one hold left: still wanted
    T_EQ_INT(nc_pump_due_ms(T0 + 200), NC_RECONCILE_MS - 200);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);   // the last: g1's goes, g2's stays
    T_EQ_INT(nc_pump_due_ms(T0 + 200), NC_RECONCILE_MS - 100);
    q = nc_next_request(T0 + 100 + NC_RECONCILE_MS);
    T_CHECK(path_is(q, "/v1/games/g2"));
    T_CHECK(nc_next_request(T0 + 100 + NC_RECONCILE_MS) == NULL);
}

// ---- the quiet game ----------------------------------------------------------------------
//
// The stream is at most once: a lost LAST cast has no later one to show its
// gap, so a live game that hears nothing for NC_QUIET_MS (plus a jitter) is read.

// An envelope of `gid` at `v` for `viewer` whose board's status is `status`
// (one byte of state: GAME_STATUS_*, as view.c's state_put opens).
static int envelope_status(const char *gid, uint32_t v, int viewer, int status, uint8_t *out, int cap) {
    int at = env_header_write(out, cap, viewer, v, NULL);
    T_CHECK(at > 0);
    env_header_set_view_len(out, at, 3);
    out[at + 2] = (uint8_t)status;
    static Roster r;
    memset(&r, 0, sizeof r);
    T_EQ_INT(roster_seat_add(&r, "u1", 2, "ANA", 3, NULL, 0), 0);
    int t = roster_trailer_write(&r, gid, (int)strlen(gid), status, 0, 1,
                                 out + at + 3, cap - at - 3);
    T_CHECK(t > 0);
    return at + 3 + t;
}

// A cast at `v` whose sequence (no events) ends on a board of `status`.
static int cast_status(uint32_t v, int status, uint8_t *out, int cap) {
    int at = evwire_cast_header_put(v, out, cap);
    T_EQ_INT(at, EVW_CAST_HEADER_BYTES);
    const uint8_t seq[] = { EVWIRE_FORMAT_VERSION, 0, EVW_SEAT_NONE, 0, 1, 0, (uint8_t)status, 0 };
    memcpy(out + at, seq, sizeof seq);
    return at + (int)sizeof seq;
}

static void quiet_answer(host_t *h, uint32_t id, uint32_t v, const uint8_t *env, int n, int64_t now) {
    static uint8_t b[1024];
    NwBlob eb = { env, (uint32_t)n };
    int len = nw_view_encode(v, eb, b, sizeof b);
    T_CHECK(len > 0);
    const NcDone *d = nc_on_response(id, 200, b, len, now);
    host_events(h);
    host_done(h, d, b);
}

// A game subscription made live at `now`, its catch-up read answered with `env` at `v`.
static void quiet_live(host_t *h, const char *gid, uint32_t v, const uint8_t *env, int n, int64_t now) {
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, gid, (int)strlen(gid)), NW_OK);
    cpkt s;
    while ((s = next_pkt(now)).type && !(s.what == NW_SUB_GAME && strcmp(s.gid, gid) == 0)) {}
    T_EQ_INT(s.type, PK_SUB);
    T_EQ_INT(suback(s.pid, 0, 2, now)->kind, NC_EV_SEATED);
    const NcRequest *q = nc_next_request(now);
    char path[32];
    snprintf(path, sizeof path, "/v1/games/%s", gid);
    T_CHECK(path_is(q, path));
    quiet_answer(h, id_of(q), v, env, n, now);
}

// The host's loop from `now` until `until`: every wake does what a host does
// (requests, then the stream's frames) and sleeps nc_due_ms. Returns the time
// the first read of `gid` went out (its request in *out), or -1 for none.
static int64_t quiet_run(const char *gid, int64_t now, int64_t until, uint32_t *out) {
    char path[32];
    int pl = snprintf(path, sizeof path, "/v1/games/%s", gid);
    for (int wakes = 0; now <= until && wakes < 100000; wakes++) {
        for (const NcRequest *q; (q = nc_next_request(now));) {
            if (q->path_len == pl && memcmp(q->path, path, (size_t)pl) == 0) { *out = id_of(q); return now; }
            nc_on_response(id_of(q), 500, NULL, 0, now);
        }
        // A live broker answers a PINGREQ (a socket that does not is closed after NC_PONG_MS).
        for (cpkt k; (k = next_pkt(now)).type;) {
            if (k.type != PK_PING) continue;
            uint8_t pr[2];
            T_EQ_INT(feed_bytes(pr, sf_put_bare(pr, sizeof pr, SF_PONG), now)->kind, NC_EV_NONE);
        }
        int d = nc_due_ms(now);
        if (d < 0) break;
        now += d ? d : 1;
    }
    return -1;
}

// The lost last cast: after NC_QUIET_MS and the jitter of silence, exactly one
// read, at that moment (the host's loop woke for it), and the board is current.
// No read while the pushes keep coming, nor before the deadline.
static void a_lost_last_cast_is_read_once_after_the_quiet(void) {
    signed_in(T0 + 360000000);
    live(T0);
    host_t h;
    memset(&h, 0, sizeof h);
    uint8_t env[256], c[64];
    quiet_live(&h, "g1", 5, env, envelope_status("g1", 5, 0, GAME_STATUS_PLAYING, env, sizeof env), T0);
    T_EQ_INT(h.table_v, 5);
    // The pushes keep coming every 20 s for ten minutes: never a read.
    int64_t t = T0;
    uint32_t v = 5, id = 0;
    for (int k = 0; k < 30; k++) {
        T_EQ_INT((int)quiet_run("g1", t, t + 20000 - 1, &id), -1);
        t += 20000;
        v++;
        host_push(&h, NW_WS_CAST, "g1", v, c, (uint32_t)cast_status(v, GAME_STATUS_PLAYING, c, sizeof c), t);
    }
    T_EQ_INT(h.table_v, 35);
    // 36 is committed and its cast lost. Nothing is heard from here on.
    int64_t heard = t;
    int64_t at = quiet_run("g1", heard, heard + 10 * NC_QUIET_MS, &id);
    T_CHECK(at >= heard + NC_QUIET_MS);
    T_CHECK(at < heard + NC_QUIET_MS + NC_QUIET_JITTER_MS);
    T_CHECK(nc_next_request(at) == NULL);   // exactly one
    T_EQ_INT(nc_pump_due_ms(at), -1);
    T_CHECK(nc_stream_due_ms(at) > 0);      // and the wait starts over: no wake at once
    int64_t ans = at + 20000;               // a slow answer
    quiet_answer(&h, id, 36, env, envelope_status("g1", 36, 0, GAME_STATUS_PLAYING, env, sizeof env), ans);
    T_EQ_INT(h.table_v, 36);
    T_EQ_INT(h.refetched, 2);   // the catch-up and this one
    // The answer is heard too: the next read is a whole quiet after it.
    uint32_t id2 = 0;
    int64_t again = quiet_run("g1", ans, ans + 10 * NC_QUIET_MS, &id2);
    T_CHECK(again >= ans + NC_QUIET_MS && again < ans + NC_QUIET_MS + NC_QUIET_JITTER_MS);
    // An answer that shows nothing newer leaves the board as it is.
    quiet_answer(&h, id2, 36, env, envelope_status("g1", 36, 0, GAME_STATUS_PLAYING, env, sizeof env), again);
    T_EQ_INT(h.table_v, 36);
    T_EQ_INT(h.refetched, 2);   // a stale answer of its own read: nothing handed out
}

// A stale cast is heard as well as a new one: the game's pushes arrive.
static void a_stale_cast_is_heard_too(void) {
    signed_in(T0 + 360000000);
    live(T0);
    host_t h;
    memset(&h, 0, sizeof h);
    uint8_t env[256], c[64];
    quiet_live(&h, "g1", 5, env, envelope_status("g1", 5, 0, GAME_STATUS_PLAYING, env, sizeof env), T0);
    // A stale cast (4) at T0 + 50 s puts the quiet off by as much.
    host_push(&h, NW_WS_CAST, "g1", 4, c, (uint32_t)cast_status(4, GAME_STATUS_PLAYING, c, sizeof c), T0 + 50000);
    T_EQ_INT(h.casts, 0);
    uint32_t id = 0;
    T_EQ_INT((int)quiet_run("g1", T0 + 50000, T0 + 50000 + NC_QUIET_MS - 1, &id), -1);
    int64_t at = quiet_run("g1", T0 + 50000, T0 + 200000, &id);
    T_CHECK(at >= T0 + 50000 + NC_QUIET_MS && at < T0 + 50000 + NC_QUIET_MS + NC_QUIET_JITTER_MS);
}

// A finished game is never read: its terminal board came as a cast (a host
// that decodes casts) or as an answered envelope (a host of boards). A rematch
// at the same table (a board that is not over) is watched again.
static void a_finished_game_is_not_read(void) {
    uint8_t env[256], c[64];
    uint32_t id = 0;
    for (int how = 0; how < 2; how++) {
        signed_in(T0 + 360000000);
        live(T0);
        host_t h;
        memset(&h, 0, sizeof h);
        quiet_live(&h, "g1", 5, env, envelope_status("g1", 5, 0, GAME_STATUS_PLAYING, env, sizeof env), T0);
        if (how == 0) {
            host_push(&h, NW_WS_CAST, "g1", 6, c, (uint32_t)cast_status(6, GAME_STATUS_GAME_OVER, c, sizeof c), T0 + 10);
            T_EQ_INT(h.casts, 1);
        } else {
            NcIntent open = intent(NC_DO_OPEN_GAME, "g1");
            T_CHECK(nc_do(&open, T0 + 10) != 0);
            const NcRequest *q = nc_next_request(T0 + 10);
            T_CHECK(path_is(q, "/v1/games/g1"));
            quiet_answer(&h, id_of(q), 6, env, envelope_status("g1", 6, 0, GAME_STATUS_GAME_OVER, env, sizeof env), T0 + 10);
            T_EQ_INT(h.table_v, 6);
        }
        T_EQ_INT((int)quiet_run("g1", T0 + 10, T0 + 30 * NC_QUIET_MS, &id), -1);
        // The table starts over: watched again.
        int64_t t = T0 + 30 * NC_QUIET_MS;
        host_push(&h, NW_WS_CAST, "g1", 7, c, (uint32_t)cast_status(7, GAME_STATUS_WAITING, c, sizeof c), t);
        T_EQ_INT(h.table_v, 7);
        int64_t at = quiet_run("g1", t, t + 10 * NC_QUIET_MS, &id);
        T_CHECK(at >= t + NC_QUIET_MS && at < t + NC_QUIET_MS + NC_QUIET_JITTER_MS);
    }
}

// Nothing is read for a game whose subscription the host let go, nor while
// the socket is down (the reconnect's catch-up owns that).
static void a_released_game_is_not_read(void) {
    signed_in(T0 + 360000000);
    live(T0);
    host_t h;
    memset(&h, 0, sizeof h);
    uint8_t env[256];
    uint32_t id = 0;
    quiet_live(&h, "g1", 5, env, envelope_status("g1", 5, 0, GAME_STATUS_PLAYING, env, sizeof env), T0);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT((int)quiet_run("g1", T0, T0 + 30 * NC_QUIET_MS, &id), -1);
    // Held again and live: watched; then the socket closes: not.
    int64_t t = T0 + 30 * NC_QUIET_MS;
    quiet_live(&h, "g1", 5, env, envelope_status("g1", 5, 0, GAME_STATUS_PLAYING, env, sizeof env), t);
    nc_stream_closed(t + 1000);
    T_EQ_INT(nc_stream_due_ms(t + 1000), -1);
    for (int64_t k = t + 1000; k < t + 30 * NC_QUIET_MS; k += 1000) T_CHECK(nc_next_request(k) == NULL);
}

// A catch-up that failed is no reason to read at once: the quiet starts when
// the subscription goes live.
static void a_failed_catch_up_waits_the_quiet(void) {
    signed_in(T0 + 360000000);
    live(T0);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    cpkt s = next_pkt(T0);
    T_EQ_INT(s.what, NW_SUB_GAME);
    suback(s.pid, 0, 2, T0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/g1"));
    nc_on_response(id_of(q), 500, NULL, 0, T0);
    uint32_t id = 0;
    int64_t at = quiet_run("g1", T0, T0 + 10 * NC_QUIET_MS, &id);
    T_CHECK(at >= T0 + NC_QUIET_MS && at < T0 + NC_QUIET_MS + NC_QUIET_JITTER_MS);
}

// A spectator's game is watched alike: the spectators' casts, then silence.
static void a_spectators_game_is_read_after_the_quiet(void) {
    signed_in(T0 + 360000000);
    live(T0);
    host_t h;
    memset(&h, 0, sizeof h);
    uint8_t env[256], c[64];
    uint32_t id = 0;
    quiet_live(&h, "g2", 7, env, envelope_status("g2", 7, -1, GAME_STATUS_PLAYING, env, sizeof env), T0);
    host_push_on(&h, NW_WS_CAST, "g2", 8, c, (uint32_t)cast_status(8, GAME_STATUS_PLAYING, c, sizeof c), 1, T0 + 5000);
    T_EQ_INT(h.casts, 1);
    T_EQ_INT(h.table_v, 8);
    int64_t at = quiet_run("g2", T0 + 5000, T0 + 10 * NC_QUIET_MS, &id);
    T_CHECK(at >= T0 + 5000 + NC_QUIET_MS && at < T0 + 5000 + NC_QUIET_MS + NC_QUIET_JITTER_MS);
    quiet_answer(&h, id, 9, env, envelope_status("g2", 9, -1, GAME_STATUS_PLAYING, env, sizeof env), at);
    T_EQ_INT(h.table_v, 9);
}

// The jitter of every wait is below NC_QUIET_JITTER_MS, and it varies.
static void the_quiet_jitter_is_bounded(void) {
    signed_in(T0 + 360000000);
    live(T0);
    host_t h;
    memset(&h, 0, sizeof h);
    uint8_t env[256], c[64];
    quiet_live(&h, "g1", 1, env, envelope_status("g1", 1, 0, GAME_STATUS_PLAYING, env, sizeof env), T0);
    int lo = NC_QUIET_JITTER_MS, hi = -1, seen[64] = { 0 }, kinds = 0;
    int64_t t = T0;
    for (uint32_t v = 2; v < 402; v++) {
        t += 1000 + (v * 37) % 1000;
        host_push(&h, NW_WS_CAST, "g1", v, c, (uint32_t)cast_status(v, GAME_STATUS_PLAYING, c, sizeof c), t);
        // At the quiet's own end, with the keep-alive just sent, the stream's
        // wake is the jitter itself.
        while (next_pkt(t + NC_QUIET_MS).type) {}
        int j = nc_stream_due_ms(t + NC_QUIET_MS);
        T_CHECK(j >= 0 && j < NC_QUIET_JITTER_MS);
        if (j < lo) lo = j;
        if (j > hi) hi = j;
        int b = j * 64 / NC_QUIET_JITTER_MS;
        if (b >= 0 && b < 64 && !seen[b]++) kinds++;
        t += NC_QUIET_MS;
    }
    T_CHECK(hi - lo > NC_QUIET_JITTER_MS / 2);
    T_CHECK(kinds > 32);   // spread over the range, not one value
}

// ---- a half-open socket and the dashboard's view hint ---------------------------------------------------------

// The close signal: nc_stream_next's one frame of len 0 (NULL for nothing, a frame with bytes for a packet).
static bool dead_frame(int64_t now) {
    const NcOut *o = nc_stream_next(now);
    return o && o->len == 0;
}

// A PINGREQ nothing answers inside NC_PONG_MS is a half-open socket: ONE len-0 frame says close it, then
// nothing; the reopened socket subscribes every game again and its catch-up read is queued.
static void a_half_open_socket_is_closed_after_the_pong_wait(void) {
    signed_in(T0 + 360000000);
    live(T0);
    host_t h;
    memset(&h, 0, sizeof h);
    uint8_t env[256];
    int en = envelope_status("g1", 5, 0, GAME_STATUS_PLAYING, env, sizeof env);
    quiet_live(&h, "g1", 5, env, en, T0);
    while (next_pkt(T0).type) {}
    int64_t t = T0 + NC_PING_MS;
    T_EQ_INT(next_pkt(t).type, PK_PING);
    T_EQ_INT(nc_stream_due_ms(t), NC_PONG_MS);   // the deadline is folded into the wake
    T_CHECK(nc_stream_next(t + NC_PONG_MS - 1) == NULL);
    T_EQ_INT(nc_stream_due_ms(t + NC_PONG_MS - 1), 1);
    T_EQ_INT(nc_stream_due_ms(t + NC_PONG_MS), 0);   // the close frame is due
    const NcOut *o = nc_stream_next(t + NC_PONG_MS);
    T_CHECK(o != NULL && o->len == 0);
    T_CHECK(nc_stream_next(t + NC_PONG_MS) == NULL);   // once
    T_EQ_INT(nc_stream_due_ms(t + NC_PONG_MS), -1);    // the host's close wakes it
    // The host closes: the reopen waits, and the new connection puts every game back.
    T_EQ_INT(nc_stream_closed(t + NC_PONG_MS), NC_RETRY_FIRST_MS);
    int64_t t2 = t + NC_PONG_MS + NC_RETRY_FIRST_MS;
    live(t2);
    cpkt s = next_pkt(t2);
    T_CHECK(s.type == PK_SUB && s.what == NW_SUB_GAME);
    T_EQ_INT(suback(s.pid, 0, 2, t2)->kind, NC_EV_SEATED);
    const NcRequest *q = nc_next_request(t2);
    T_CHECK(path_is(q, "/v1/games/g1"));   // the catch-up read
    // A reset socket pings normally and can die again (the dead flag and the ping clock start over).
    while (next_pkt(t2).type) {}
    T_EQ_INT(next_pkt(t2 + NC_PING_MS).type, PK_PING);
    T_CHECK(!dead_frame(t2 + NC_PING_MS + NC_PONG_MS - 1));
    T_CHECK(dead_frame(t2 + NC_PING_MS + NC_PONG_MS));
}

// A socket that answers is never closed: a PINGRESP a little late, a long run of them, and any other packet from the
// broker (a stream that delivers pushes is alive) clear the wait.
static void a_live_socket_is_never_closed(void) {
    signed_in(T0 + 360000000);
    live(T0);
    int64_t now = T0;
    uint8_t pr[2];
    for (int k = 0; k < 20; k++) {
        now += NC_PING_MS;
        T_EQ_INT(next_pkt(now).type, PK_PING);
        now += 100;
        T_EQ_INT(feed_bytes(pr, sf_put_bare(pr, sizeof pr, SF_PONG), now)->kind, NC_EV_NONE);
        T_CHECK(!dead_frame(now + NC_PONG_MS + 1));
    }
    // The answer NC_PONG_MS - 1 late is in time.
    now += NC_PING_MS;
    T_EQ_INT(next_pkt(now).type, PK_PING);
    T_EQ_INT(feed_bytes(pr, sf_put_bare(pr, sizeof pr, SF_PONG), now + NC_PONG_MS - 1)->kind, NC_EV_NONE);
    T_CHECK(!dead_frame(now + NC_PONG_MS + 5));
    // Any packet clears it, not only the PINGRESP: a PUBLISH does.
    now += NC_PING_MS;
    T_EQ_INT(next_pkt(now).type, PK_PING);
    uint8_t pub[64];
    int k = sf_put_bare(pub, sizeof pub, SF_PONG);
    T_CHECK(k > 0);
    nc_stream_in(pub, k, 0, now + 100);
    T_CHECK(!dead_frame(now + 100 + NC_PONG_MS + 1));
}

// A dashboard view of a game held on its own stream is not applied; a view newer than the board hints that a cast was lost
// and queues ONE read NC_RECONCILE_MS later, which the cast that normally follows settles for free.
static void a_newer_view_of_a_held_game_queues_one_read(void) {
    const uint8_t view[] = { 0x5f }, cast[] = { 0xc6, 0x01 };
    uint8_t env[256];
    int en = envelope_status("g1", 5, 0, GAME_STATUS_PLAYING, env, sizeof env);
    for (int how = 0; how < 3; how++) {
        signed_in(T0 + 360000000);
        live(T0);
        host_t h;
        memset(&h, 0, sizeof h);
        quiet_live(&h, "g1", 5, env, en, T0);
        T_EQ_INT(h.table_v, 5);
        // Older and equal views: nothing.
        host_push(&h, NW_WS_VIEW, "g1", 5, view, 1, T0 + 10);
        host_push(&h, NW_WS_VIEW, "g1", 4, view, 1, T0 + 10);
        T_CHECK(nc_next_request(T0 + 10 + NC_RECONCILE_MS) == NULL);
        // A newer one: not applied, one read at its time.
        host_push(&h, NW_WS_VIEW, "g1", 6, view, 1, T0 + 20);
        T_EQ_INT(h.views, 0);
        T_EQ_INT(h.table_v, 5);
        T_CHECK(nc_next_request(T0 + 20 + NC_RECONCILE_MS - 1) == NULL);
        if (how == 1) {   // a second and a newer view of the same game are the same slot
            host_push(&h, NW_WS_VIEW, "g1", 6, view, 1, T0 + 30);
            host_push(&h, NW_WS_VIEW, "g1", 7, view, 1, T0 + 30);
        }
        if (how == 2) {   // the seat's cast arrives first: the read is dropped
            host_push(&h, NW_WS_CAST, "g1", 6, cast, 2, T0 + 40);
            T_CHECK(nc_next_request(T0 + 20 + NC_RECONCILE_MS + 100) == NULL);
            continue;
        }
        const NcRequest *q = nc_next_request(T0 + 30 + NC_RECONCILE_MS);
        T_CHECK(path_is(q, "/v1/games/g1"));
        T_CHECK(nc_next_request(T0 + 30 + NC_RECONCILE_MS) == NULL);   // exactly one
        T_EQ_INT(h.views, 0);
    }
}

// A game with no stream of its own still takes the dashboard's view (the host of rows).
static void a_view_of_an_unheld_game_applies(void) {
    const uint8_t view[] = { 0x5f };
    signed_in(T0 + 360000000);
    live(T0);
    host_t h;
    memset(&h, 0, sizeof h);
    host_push(&h, NW_WS_VIEW, "g2", 6, view, 1, T0);
    T_EQ_INT(h.views, 1);
    T_EQ_INT(h.row_v, 6);
    T_CHECK(nc_next_request(T0 + 10 * NC_RECONCILE_MS) == NULL);
}

int main(void) {
    T_RUN(session_blob);
    T_RUN(requests);
    T_RUN(answers);
    T_RUN(connect_carries_the_token);
    T_RUN(connect_waits_for_a_fresh_token);
    T_RUN(closed_before_connack_refreshes);
    T_RUN(stream_order_and_retries);
    T_RUN(actions_ride_the_socket);
    T_RUN(holds_made_before_the_session_are_kept);
    T_RUN(what_is_not_the_brokers);
    T_RUN(an_unanswered_subscription_is_asked_again);
    T_RUN(gates);
    T_RUN(pump_signs_in_and_reads_answers);
    T_RUN(pump_refreshes_first_and_after_a_401);
    T_RUN(pump_retries_reads_not_writes);
    T_RUN(pump_signs_out);
    T_RUN(a_cast_and_a_stale_answer_end_alike);
    T_RUN(a_stamped_answer_applies_at_the_casts_version);
    T_RUN(a_skipped_version_refetches);
    T_RUN(a_refused_move_catches_up);
    T_RUN(suback_takes_the_game_whole);
    T_RUN(seat_and_spectator_casts);
    T_RUN(the_session_is_told_once);
    T_RUN(another_user_starts_the_book_over);
    T_RUN(waiting_intents_keep_their_bytes);
    T_RUN(a_host_of_boards_gets_boards);
    T_RUN(a_busy_read_goes_again_a_write_does_not);
    T_RUN(fast_games_leave_the_pump_as_it_was);
    T_RUN(a_reconcile_lets_go_once_caught_up);
    T_RUN(a_reconcile_lets_go_with_the_subscription);
    T_RUN(a_lost_last_cast_is_read_once_after_the_quiet);
    T_RUN(a_stale_cast_is_heard_too);
    T_RUN(a_finished_game_is_not_read);
    T_RUN(a_released_game_is_not_read);
    T_RUN(a_failed_catch_up_waits_the_quiet);
    T_RUN(a_half_open_socket_is_closed_after_the_pong_wait);
    T_RUN(a_live_socket_is_never_closed);
    T_RUN(a_newer_view_of_a_held_game_queues_one_read);
    T_RUN(a_view_of_an_unheld_game_applies);
    T_RUN(a_spectators_game_is_read_after_the_quiet);
    T_RUN(the_quiet_jitter_is_bounded);
    return T_DONE();
}
