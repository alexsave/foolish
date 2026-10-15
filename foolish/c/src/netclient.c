// netclient.c - the client of the backend's binary surface (netclient.h).
#include "netclient.h"
#include "table.h"
#include <string.h>

// ---- state -------------------------------------------------------------------------

typedef struct {
    uint8_t  what;
    uint8_t  gid_len;
    char     gid[NW_GID_MAX];
    uint8_t  state;        // SUB_WANT, SUB_SENT, SUB_LIVE, SUB_REFUSED
    uint16_t ref;
    int64_t  retry_at;
    int32_t  backoff;
} sub;
enum { SUB_FREE = 0, SUB_WANT, SUB_SENT, SUB_LIVE, SUB_REFUSED };

typedef struct {
    uint8_t  used;
    uint8_t  gid_len;
    char     gid[NW_GID_MAX];
    uint8_t  has_view, has_cast;
    uint32_t view_v, cast_v;
    uint32_t touched;      // LRU stamp
} gate;

typedef struct {
    uint8_t  what;
    uint8_t  gid_len;
    char     gid[NW_GID_MAX];
} unsub;

static NwSession     g_session;
static uint8_t       g_signed_in;
static uint32_t      g_session_gen;     // bumped whenever the session changes
static NcSessionInfo g_info;
static NcError       g_error;
static NcRequest     g_req;

static sub      g_subs[NC_SUBS_MAX];
static unsub    g_unsubs[NC_SUBS_MAX];
static int      g_n_unsubs;
static gate     g_gates[NC_GATES_MAX];
static uint32_t g_gate_clock;
static uint8_t  g_open;
static uint32_t g_auth_sent_gen;        // the session generation the socket was told, 0 none
static int64_t  g_last_ping;
static uint16_t g_next_ref;
static int32_t  g_reconnect_ms;

static NcEvent  g_event;
static uint8_t  g_frame[NC_FRAME_MAX];  // the message decoded from a text frame
static const uint8_t *g_payload;
static NcOut    g_out;

static NcGameRow     g_game_row;
static NcChatRow     g_chat_row;
static NcBotRow      g_bot_row;
static NcBoardRow    g_board_row;
static NcHistoryRow  g_hist_row;
static NcHistoryHead g_hist_head;
static NcReplay      g_replay;
static NcAction      g_action;

static void info_refresh(void) {
    memset(&g_info, 0, sizeof g_info);
    g_info.signed_in = g_signed_in;
    if (!g_signed_in) return;
    g_info.user_id_len = g_session.user_id_len;
    memcpy(g_info.user_id, g_session.user_id, g_session.user_id_len);
    g_info.username_len = g_session.username_len;
    memcpy(g_info.username, g_session.username, g_session.username_len);
}

static void signed_out(void) {
    memset(&g_session, 0, sizeof g_session);
    g_signed_in = 0;
    g_session_gen++;
    info_refresh();
}

void nc_reset(void) {
    signed_out();
    memset(&g_error, 0, sizeof g_error);
    memset(g_subs, 0, sizeof g_subs);
    memset(g_gates, 0, sizeof g_gates);
    g_n_unsubs = 0;
    g_gate_clock = 0;
    g_open = 0;
    g_auth_sent_gen = 0;
    g_last_ping = 0;
    g_next_ref = 0;
    g_reconnect_ms = 0;
}

// ---- the session -------------------------------------------------------------------

int nc_session_save(uint8_t *out, int cap) {
    if (!g_signed_in) return 0;
    return nw_session_encode(&g_session, out, cap);
}

int nc_session_load(const uint8_t *p, int len) {
    NwSession s;
    int rc = nw_session_decode(p, len, &s);
    if (rc != NW_OK || !s.access_len || !s.refresh_len || !s.user_id_len) {
        signed_out();
        return NW_E_WIRE;
    }
    g_session = s;
    g_signed_in = 1;
    g_session_gen++;
    info_refresh();
    return NW_OK;
}

const NcSessionInfo *nc_session_info(void) {
    info_refresh();
    return &g_info;
}

const NcError *nc_error(void) { return &g_error; }

// Reads a refusal body (or a status with none) into g_error: its why.
static int refused(int status, const uint8_t *body, int len) {
    memset(&g_error, 0, sizeof g_error);
    g_error.status = status;
    AskRefusal r;
    if (body && ask_refusal_decode(body, len, &r) == ASK_OK) {
        g_error.why = r.why;
        g_error.code = r.code;
        g_error.detail = r.detail;
    } else {
        // No refusal of ours (a proxy's 502, a body cut short): the status says enough.
        g_error.why = status == 401 ? ASK_WHY_AUTH : status == 404 ? ASK_WHY_NOT_FOUND : ASK_WHY_INTERNAL;
    }
    return g_error.why;
}

static int ok_status(int status) { return status >= 200 && status < 300; }

int nc_session_read(int status, const uint8_t *body, int len) {
    if (!ok_status(status)) {
        int why = refused(status, body, len);
        // A refresh that is refused for the token itself ends the session.
        if (why == ASK_WHY_ACCOUNT &&
            (g_error.code == NW_ACCOUNT_E_BAD_REFRESH || g_error.code == NW_ACCOUNT_E_REFRESH_USED))
            signed_out();
        return why;
    }
    NwSession s;
    if (nw_session_decode(body, len, &s) != NW_OK || !s.access_len || !s.refresh_len || !s.user_id_len) {
        memset(&g_error, 0, sizeof g_error);
        g_error.status = status;
        g_error.why = ASK_WHY_MALFORMED;
        return ASK_WHY_MALFORMED;
    }
    memset(&g_error, 0, sizeof g_error);
    g_session = s;
    g_signed_in = 1;
    g_session_gen++;
    info_refresh();
    return 0;
}

// ---- requests ------------------------------------------------------------------------

static int put_path(const char *a, const char *gid, int gid_len, const char *b) {
    int la = (int)strlen(a), lb = b ? (int)strlen(b) : 0;
    if (gid_len < 0 || la + gid_len + lb > NC_PATH_MAX - 1) return 0;
    // An id goes into a path as it is: refuse a byte that is not one.
    for (int i = 0; i < gid_len; i++) {
        char c = gid[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    memcpy(g_req.path, a, (size_t)la);
    if (gid_len) memcpy(g_req.path + la, gid, (size_t)gid_len);
    if (lb) memcpy(g_req.path + la + gid_len, b, (size_t)lb);
    g_req.path_len = (uint8_t)(la + gid_len + lb);
    return 1;
}

static const NcRequest *begin(uint8_t method, const char *path) {
    memset(&g_req, 0, sizeof g_req);
    g_req.method = method;
    return put_path(path, NULL, 0, NULL) ? &g_req : NULL;
}

static void bearer(void) {
    static const char B[] = "Bearer ";
    memcpy(g_req.auth, B, sizeof B - 1);
    memcpy(g_req.auth + sizeof B - 1, g_session.access, g_session.access_len);
    g_req.auth_len = (uint16_t)(sizeof B - 1 + g_session.access_len);
}

const NcRequest *nc_req_refresh(void) {
    if (!g_signed_in) return NULL;
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_REFRESH;
    q.refresh_len = g_session.refresh_len;
    memcpy(q.refresh, g_session.refresh, g_session.refresh_len);
    begin(NC_POST, "/v1/auth/refresh");
    int n = nw_request_encode(&q, g_req.body, NC_BODY_MAX);
    if (n < 0) return NULL;
    g_req.body_len = (uint16_t)n;
    g_req.is_refresh = 1;
    return &g_req;
}

// A request that needs the user: NULL when nobody is signed in, the refresh
// when the token is due, else the request begun with its bearer.
static const NcRequest *authed(uint8_t method, const char *path, int64_t now_ms, int *refresh) {
    *refresh = 0;
    if (!g_signed_in) return NULL;
    if ((int64_t)g_session.expires_at_ms - NC_REFRESH_EARLY_MS <= now_ms) {
        *refresh = 1;
        return nc_req_refresh();
    }
    if (!begin(method, path)) return NULL;
    bearer();
    return &g_req;
}

static const NcRequest *body_of(const NwRequest *q) {
    int n = nw_request_encode(q, g_req.body, NC_BODY_MAX);
    if (n < 0) return NULL;
    g_req.body_len = (uint16_t)n;
    return &g_req;
}

static const NcRequest *credentials(uint8_t kind, const char *path, const char *name, int name_len, const char *pw, int pw_len) {
    if (!name || !pw || name_len < 0 || pw_len < 0 || name_len > NW_NAME_MAX || pw_len > NW_PASSWORD_MAX) return NULL;
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = kind;
    q.name_len = (uint8_t)name_len;
    memcpy(q.name, name, (size_t)name_len);
    q.password_len = (uint8_t)pw_len;
    memcpy(q.password, pw, (size_t)pw_len);
    if (!begin(NC_POST, path)) return NULL;
    return body_of(&q);
}

const NcRequest *nc_req_signup(const char *name, int name_len, const char *pw, int pw_len) {
    return credentials(NW_SIGNUP, "/v1/auth/signup", name, name_len, pw, pw_len);
}

const NcRequest *nc_req_signin(const char *name, int name_len, const char *pw, int pw_len) {
    return credentials(NW_SIGNIN, "/v1/auth/signin", name, name_len, pw, pw_len);
}

const NcRequest *nc_req_signout(int global, int64_t now_ms) {
    int r;
    const NcRequest *q = authed(NC_POST, "/v1/auth/signout", now_ms, &r);
    if (!q || r) return q;
    NwRequest w;
    memset(&w, 0, sizeof w);
    w.kind = NW_SIGNOUT;
    w.scope = global ? NW_SCOPE_GLOBAL : NW_SCOPE_LOCAL;
    return body_of(&w);
}

const NcRequest *nc_req_password(const char *pw, int pw_len, int64_t now_ms) {
    if (!pw || pw_len < 0 || pw_len > NW_PASSWORD_MAX) return NULL;
    int r;
    const NcRequest *q = authed(NC_POST, "/v1/auth/password", now_ms, &r);
    if (!q || r) return q;
    NwRequest w;
    memset(&w, 0, sizeof w);
    w.kind = NW_PASSWORD;
    w.password_len = (uint8_t)pw_len;
    memcpy(w.password, pw, (size_t)pw_len);
    return body_of(&w);
}

const NcRequest *nc_req_delete_account(int64_t now_ms) {
    int r;
    return authed(NC_POST, "/v1/account/delete", now_ms, &r);
}

const NcRequest *nc_req_create(int64_t now_ms) {
    int r;
    return authed(NC_POST, "/v1/create", now_ms, &r);
}

const NcRequest *nc_req_action(const char *gid, int gid_len, const uint8_t *wire, int wire_len, int64_t intent, int64_t now_ms) {
    if (!gid || gid_len <= 0 || gid_len > NW_GID_MAX || wire_len < 0 || (wire_len && !wire) || intent > (int64_t)UINT32_MAX)
        return NULL;
    int r;
    const NcRequest *q = authed(NC_POST, "/v1/action", now_ms, &r);
    if (!q || r) return q;
    if (gid_len + wire_len > NC_BODY_MAX) return NULL;
    // table_request_encode rewrites [gid][wire] in place as the request.
    memcpy(g_req.body, gid, (size_t)gid_len);
    if (wire_len) memcpy(g_req.body + gid_len, wire, (size_t)wire_len);
    int n = table_request_encode(g_req.body, NC_BODY_MAX, gid_len, wire_len, intent >= 0, intent >= 0 ? (uint32_t)intent : 0);
    if (n < 0) return NULL;
    g_req.body_len = (uint16_t)n;
    return &g_req;
}

const NcRequest *nc_req_meta(const uint8_t *ask, int ask_len, int64_t now_ms) {
    if (!ask || ask_len <= 0 || ask_len > NC_BODY_MAX) return NULL;
    int r;
    const NcRequest *q = authed(NC_POST, "/v1/meta", now_ms, &r);
    if (!q || r) return q;
    memcpy(g_req.body, ask, (size_t)ask_len);
    g_req.body_len = (uint16_t)ask_len;
    return &g_req;
}

// A GET of `a<gid>`, with the user when `user`.
static const NcRequest *get(const char *a, const char *gid, int gid_len, int user, int64_t now_ms) {
    if (gid && (gid_len <= 0 || gid_len > NW_GID_MAX)) return NULL;
    const NcRequest *q;
    if (user) {
        int r;
        q = authed(NC_GET, "/", now_ms, &r);
        if (!q || r) return q;
    } else {
        q = begin(NC_GET, "/");
    }
    if (!q) return NULL;
    return put_path(a, gid, gid ? gid_len : 0, NULL) ? &g_req : NULL;
}

const NcRequest *nc_req_games(int64_t now_ms) { return get("/v1/games", NULL, 0, 1, now_ms); }
const NcRequest *nc_req_game(const char *gid, int gid_len, int64_t now_ms) { return get("/v1/games/", gid, gid_len, 1, now_ms); }
const NcRequest *nc_req_chats(const char *gid, int gid_len, int64_t now_ms) { return get("/v1/chat/", gid, gid_len, 1, now_ms); }
const NcRequest *nc_req_bots(int64_t now_ms) { return get("/v1/bots", NULL, 0, 1, now_ms); }
const NcRequest *nc_req_board(void) { return get("/v1/leaderboard", NULL, 0, 0, 0); }
const NcRequest *nc_req_history(int64_t now_ms) { return get("/v1/history", NULL, 0, 1, now_ms); }
const NcRequest *nc_req_replay(const char *gid, int gid_len, int64_t now_ms) { return get("/v1/replay/", gid, gid_len, 1, now_ms); }

const NcRequest *nc_req_chat_send(const char *gid, int gid_len, const char *msg, int msg_len, int64_t now_ms) {
    if (!gid || gid_len <= 0 || gid_len > NW_GID_MAX || !msg || msg_len <= 0 || msg_len > NW_CHAT_MAX) return NULL;
    int r;
    const NcRequest *q = authed(NC_POST, "/", now_ms, &r);
    if (!q || r) return q;
    if (!put_path("/v1/chat/", gid, gid_len, NULL)) return NULL;
    NwRequest w;
    memset(&w, 0, sizeof w);
    w.kind = NW_CHAT_SEND;
    w.message_len = (uint16_t)msg_len;
    memcpy(w.message, msg, (size_t)msg_len);
    return body_of(&w);
}

const NcRequest *nc_req_stream(void) { return begin(NC_GET, "/v1/stream?proto=nw1"); }

// ---- answers -------------------------------------------------------------------------

int nc_status_read(int status, const uint8_t *body, int len) {
    if (!ok_status(status)) return refused(status, body, len);
    memset(&g_error, 0, sizeof g_error);
    return 0;
}

const NcAction *nc_action_read(int status, const uint8_t *body, int len) {
    if (!ok_status(status)) { refused(status, body, len); return NULL; }
    TableResponse t;
    if (!body || table_response_decode(body, len, &t) != 0) {
        memset(&g_error, 0, sizeof g_error);
        g_error.status = status;
        g_error.why = ASK_WHY_MALFORMED;
        return NULL;
    }
    memset(&g_error, 0, sizeof g_error);
    g_action.status = t.status;
    g_action.reject_code = t.reject_code;
    g_action.version = t.version;
    return &g_action;
}

static int malformed(int status) {
    memset(&g_error, 0, sizeof g_error);
    g_error.status = status;
    g_error.why = ASK_WHY_MALFORMED;
    return ASK_WHY_MALFORMED;
}

static uint32_t off_in(const uint8_t *body, const uint8_t *p) { return p ? (uint32_t)(p - body) : 0; }

const NcGameRow *nc_view_read(int status, const uint8_t *body, int len) {
    if (!ok_status(status)) { refused(status, body, len); return NULL; }
    uint32_t v;
    NwBlob env;
    if (nw_view_decode(body, len, &v, &env) != NW_OK) { malformed(status); return NULL; }
    memset(&g_error, 0, sizeof g_error);
    g_game_row.version = v;
    g_game_row.env_off = off_in(body, env.p);
    g_game_row.env_len = env.len;
    return &g_game_row;
}

const NcReplay *nc_replay_read(int status, const uint8_t *body, int len) {
    if (!ok_status(status)) { refused(status, body, len); return NULL; }
    NwBlob m, e;
    if (nw_replay_decode(body, len, &m, &e) != NW_OK) { malformed(status); return NULL; }
    memset(&g_error, 0, sizeof g_error);
    g_replay.moves_off = off_in(body, m.p);
    g_replay.moves_len = m.len;
    g_replay.extras_off = off_in(body, e.p);
    g_replay.extras_len = e.len;
    return &g_replay;
}

int nc_list_read(int status, const uint8_t *body, int len, int kind) {
    if (!ok_status(status)) return -refused(status, body, len);
    int n = kind < 0 || kind > 255 ? NW_E_WIRE : nw_list_open(body, len, (uint8_t)kind);
    if (n < 0) return -malformed(status);
    memset(&g_error, 0, sizeof g_error);
    return n;
}

const NcGameRow *nc_game_row(const uint8_t *body, int len, int i) {
    uint32_t v;
    NwBlob env;
    if (nw_get_game(body, len, i, &v, &env) != NW_OK) return NULL;
    g_game_row.version = v;
    g_game_row.env_off = off_in(body, env.p);
    g_game_row.env_len = env.len;
    return &g_game_row;
}

static void chat_copy(NcChatRow *o, const NwChat *c) {
    memset(o, 0, sizeof *o);
    o->id_len = c->id_len;
    memcpy(o->id, c->id, c->id_len);
    o->user_id_len = c->user_id_len;
    memcpy(o->user_id, c->user_id, c->user_id_len);
    o->created_ms = c->created_ms;
    o->is_system = c->is_system;
    o->message_len = c->message_len;
    memcpy(o->message, c->message, c->message_len);
}

const NcChatRow *nc_chat_row(const uint8_t *body, int len, int i) {
    NwChat c;
    if (nw_get_chat(body, len, i, &c) != NW_OK) return NULL;
    chat_copy(&g_chat_row, &c);
    return &g_chat_row;
}

const NcBotRow *nc_bot_row(const uint8_t *body, int len, int i) {
    NwBot b;
    if (nw_get_bot(body, len, i, &b) != NW_OK) return NULL;
    memset(&g_bot_row, 0, sizeof g_bot_row);
    g_bot_row.id_len = b.id_len;
    memcpy(g_bot_row.id, b.id, b.id_len);
    g_bot_row.nickname_len = b.nickname_len;
    memcpy(g_bot_row.nickname, b.nickname, b.nickname_len);
    g_bot_row.key_len = b.key_len;
    memcpy(g_bot_row.key, b.key, b.key_len);
    return &g_bot_row;
}

const NcBoardRow *nc_board_row(const uint8_t *body, int len, int i) {
    NwBoardRow b;
    if (nw_get_board(body, len, i, &b) != NW_OK) return NULL;
    memset(&g_board_row, 0, sizeof g_board_row);
    g_board_row.is_bot = b.is_bot;
    g_board_row.id_len = b.id_len;
    memcpy(g_board_row.id, b.id, b.id_len);
    g_board_row.name_len = b.name_len;
    memcpy(g_board_row.name, b.name, b.name_len);
    g_board_row.rating = b.rating;
    g_board_row.games = b.games;
    return &g_board_row;
}

const NcHistoryRow *nc_history_row(const uint8_t *body, int len, int i) {
    NwHistoryRow h;
    if (nw_get_history(body, len, i, &h) != NW_OK) return NULL;
    memset(&g_hist_row, 0, sizeof g_hist_row);
    g_hist_row.id_len = h.id_len;
    memcpy(g_hist_row.id, h.id, h.id_len);
    g_hist_row.created_ms = h.created_ms;
    g_hist_row.n_players = h.n_players;
    for (int k = 0; k < h.n_players; k++) {
        g_hist_row.players[k].len = h.player_len[k];
        memcpy(g_hist_row.players[k].id, h.player[k], h.player_len[k]);
    }
    g_hist_row.moves_off = off_in(body, h.moves.p);
    g_hist_row.moves_len = h.moves.len;
    g_hist_row.extras_off = off_in(body, h.extras.p);
    g_hist_row.extras_len = h.extras.len;
    return &g_hist_row;
}

const NcHistoryHead *nc_history_head(const uint8_t *body, int len) {
    int has;
    uint32_t rating;
    int n = nw_list_open(body, len, NW_HISTORY);
    if (n < 0 || nw_history_rating(body, len, &has, &rating) != NW_OK) return NULL;
    g_hist_head.has_rating = (uint8_t)has;
    g_hist_head.rating = rating;
    g_hist_head.n = (uint16_t)n;
    return &g_hist_head;
}

// ---- version gates ---------------------------------------------------------------------

// Two ids alike. An empty one may come as NULL (the games list has none), and
// memcmp is not given a NULL even for no bytes.
static int same_id(const char *a, int a_len, const char *b, int b_len) {
    return a_len == b_len && (a_len == 0 || memcmp(a, b, (size_t)a_len) == 0);
}

static gate *gate_find(const char *gid, int gid_len, int make) {
    gate *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < NC_GATES_MAX; i++) {
        gate *g = &g_gates[i];
        if (!g->used) { if (!free_slot) free_slot = g; continue; }
        if (same_id(g->gid, g->gid_len, gid, gid_len)) { g->touched = ++g_gate_clock; return g; }
        if (!oldest || g->touched < oldest->touched) oldest = g;
    }
    if (!make) return NULL;
    gate *g = free_slot ? free_slot : oldest;   // the least recently seen game gives way
    memset(g, 0, sizeof *g);
    g->used = 1;
    g->gid_len = (uint8_t)gid_len;
    memcpy(g->gid, gid, (size_t)gid_len);
    g->touched = ++g_gate_clock;
    return g;
}

void nc_gate_reset(const char *gid, int gid_len) {
    if (!gid || gid_len <= 0 || gid_len > NW_GID_MAX) return;
    gate *g = gate_find(gid, gid_len, 0);
    if (g) memset(g, 0, sizeof *g);
}

// 1 when a push of `version` is newer than any applied on this stream of the game.
static int gate_pass(gate *g, int cast, uint32_t version) {
    uint8_t *has = cast ? &g->has_cast : &g->has_view;
    uint32_t *v = cast ? &g->cast_v : &g->view_v;
    if (*has && version <= *v) return 0;
    *has = 1;
    *v = version;
    return 1;
}

// ---- the stream --------------------------------------------------------------------------

static sub *sub_find(int what, const char *gid, int gid_len) {
    for (int i = 0; i < NC_SUBS_MAX; i++) {
        sub *s = &g_subs[i];
        if (s->state != SUB_FREE && s->what == what && same_id(s->gid, s->gid_len, gid, gid_len))
            return s;
    }
    return NULL;
}

static int sub_args_ok(int what, const char *gid, int gid_len) {
    if (what == NW_SUB_GAMES) return gid_len == 0;
    if (what != NW_SUB_GAME && what != NW_SUB_CHAT) return 0;
    return gid && gid_len > 0 && gid_len <= NW_GID_MAX;
}

int nc_stream_sub(int what, const char *gid, int gid_len) {
    if (!sub_args_ok(what, gid, gid_len)) return NW_E_WIRE;
    sub *s = sub_find(what, gid, gid_len);
    if (!s) {
        for (int i = 0; i < NC_SUBS_MAX && !s; i++)
            if (g_subs[i].state == SUB_FREE) s = &g_subs[i];
        if (!s) return NW_E_CAP;
        memset(s, 0, sizeof *s);
        s->what = (uint8_t)what;
        s->gid_len = (uint8_t)gid_len;
        if (gid_len) memcpy(s->gid, gid, (size_t)gid_len);
    }
    // Asked again: sent again, at once.
    s->state = SUB_WANT;
    s->retry_at = 0;
    s->backoff = NC_RETRY_FIRST_MS;
    // A pending unsubscribe of the same thing is moot.
    for (int i = 0; i < g_n_unsubs; i++)
        if (g_unsubs[i].what == what && same_id(g_unsubs[i].gid, g_unsubs[i].gid_len, gid, gid_len)) {
            g_unsubs[i] = g_unsubs[--g_n_unsubs];
            break;
        }
    return NW_OK;
}

int nc_stream_unsub(int what, const char *gid, int gid_len) {
    if (!sub_args_ok(what, gid, gid_len)) return NW_E_WIRE;
    sub *s = sub_find(what, gid, gid_len);
    if (!s) return NW_OK;
    int told = s->state == SUB_SENT || s->state == SUB_LIVE;
    memset(s, 0, sizeof *s);
    if (told && g_open) {
        if (g_n_unsubs == NC_SUBS_MAX) return NW_E_CAP;
        unsub *u = &g_unsubs[g_n_unsubs++];
        u->what = (uint8_t)what;
        u->gid_len = (uint8_t)gid_len;
        if (gid_len) memcpy(u->gid, gid, (size_t)gid_len);
    }
    return NW_OK;
}

void nc_stream_opened(int64_t now_ms) {
    g_open = 1;
    g_auth_sent_gen = 0;
    g_last_ping = now_ms;
    g_n_unsubs = 0;
    for (int i = 0; i < NC_SUBS_MAX; i++)
        if (g_subs[i].state != SUB_FREE) { g_subs[i].state = SUB_WANT; g_subs[i].retry_at = 0; }
}

int nc_stream_closed(int64_t now_ms) {
    (void)now_ms;
    g_open = 0;
    g_auth_sent_gen = 0;
    g_n_unsubs = 0;
    int wait = g_reconnect_ms ? g_reconnect_ms : NC_RETRY_FIRST_MS;
    g_reconnect_ms = wait * 2 > NC_RETRY_MAX_MS ? NC_RETRY_MAX_MS : wait * 2;
    return wait;
}

const uint8_t *nc_event_payload(void) { return g_payload; }

static const NcEvent *event_none(uint8_t kind) {
    memset(&g_event, 0, sizeof g_event);
    g_payload = NULL;
    g_event.kind = kind;
    return &g_event;
}

const NcEvent *nc_stream_in(const uint8_t *frame, int len, int is_text, int64_t now_ms) {
    const uint8_t *msg = frame;
    int n = len;
    if (is_text) {
        n = nw_text_decode((const char *)frame, len, g_frame, NC_FRAME_MAX);
        if (n < 0) return event_none(NC_EV_BAD);
        msg = g_frame;
    } else if (frame && len > 0 && len <= NC_FRAME_MAX && frame != g_frame) {
        memcpy(g_frame, frame, (size_t)len);
        msg = g_frame;
    } else if (len > NC_FRAME_MAX) {
        return event_none(NC_EV_BAD);
    }
    static NwFrame f;
    if (nw_frame_decode(msg, n, &f) != NW_OK) return event_none(NC_EV_BAD);
    g_reconnect_ms = 0;   // the socket works: a later close starts the schedule over
    event_none(NC_EV_NONE);
    NcEvent *e = &g_event;
    e->gid_len = f.gid_len;
    memcpy(e->gid, f.gid, f.gid_len);
    e->version = f.version;
    switch (f.kind) {
    case NW_WS_ACK: {
        sub *s = NULL;
        for (int i = 0; i < NC_SUBS_MAX && !s; i++)
            if (g_subs[i].state == SUB_SENT && g_subs[i].ref == f.ref) s = &g_subs[i];
        if (!s) return e;   // a subscription since dropped or asked again
        e->what = s->what;
        e->gid_len = s->gid_len;
        memcpy(e->gid, s->gid, s->gid_len);
        if (f.why == 0) {
            s->state = SUB_LIVE;
            s->backoff = NC_RETRY_FIRST_MS;
            if (s->what == NW_SUB_GAME) { e->kind = NC_EV_SEATED; e->seated = f.seated; }
            return e;
        }
        s->state = SUB_REFUSED;
        s->retry_at = now_ms + s->backoff;
        s->backoff = s->backoff * 2 > NC_RETRY_MAX_MS ? NC_RETRY_MAX_MS : s->backoff * 2;
        // A refused token is told again with whatever token the session holds then.
        if (f.why == ASK_WHY_AUTH) g_auth_sent_gen = 0;
        e->kind = NC_EV_REFUSED;
        e->why = f.why;
        return e;
    }
    case NW_WS_VIEW:
    case NW_WS_CAST: {
        if (!f.gid_len) return event_none(NC_EV_BAD);
        int cast = f.kind == NW_WS_CAST;
        gate *g = gate_find(f.gid, f.gid_len, 1);
        // An empty view (the seat is gone) always applies and ends the game's gate.
        if (!cast && f.blob.len == 0) {
            memset(g, 0, sizeof *g);
        } else if (!gate_pass(g, cast, f.version)) {
            return e;   // stale: at or below what was applied
        }
        e->kind = cast ? NC_EV_CAST : NC_EV_VIEW;
        e->what = cast ? NW_SUB_GAME : NW_SUB_GAMES;
        e->payload_len = f.blob.len;
        g_payload = f.blob.p;
        return e;
    }
    case NW_WS_CHAT:
        e->kind = NC_EV_CHAT;
        e->what = NW_SUB_CHAT;
        chat_copy(&e->chat, &f.chat);
        return e;
    case NW_WS_PONG:
        return e;
    default:
        return event_none(NC_EV_BAD);   // a client frame, or one this client does not take
    }
}

static const NcOut *out_frame(const NwFrame *f) {
    static uint8_t raw[NW_ACCESS_MAX + 16];
    int n = nw_frame_encode(f, raw, (int)sizeof raw);
    if (n < 0) return NULL;
    int t = nw_text_encode(raw, n, g_out.text, NC_TEXT_MAX);
    if (t < 0) return NULL;
    g_out.len = (uint16_t)t;
    return &g_out;
}

const NcOut *nc_stream_next(int64_t now_ms) {
    if (!g_open || !g_signed_in) return NULL;
    static NwFrame f;
    memset(&f, 0, sizeof f);
    // The token first, again whenever the session has changed since.
    if (g_auth_sent_gen != g_session_gen) {
        g_auth_sent_gen = g_session_gen;
        f.kind = NW_WS_AUTH;
        f.token_len = g_session.access_len;
        memcpy(f.token, g_session.access, g_session.access_len);
        return out_frame(&f);
    }
    if (g_n_unsubs) {
        unsub *u = &g_unsubs[--g_n_unsubs];
        f.kind = NW_WS_UNSUB;
        f.what = u->what;
        f.gid_len = u->gid_len;
        memcpy(f.gid, u->gid, u->gid_len);
        return out_frame(&f);
    }
    for (int i = 0; i < NC_SUBS_MAX; i++) {
        sub *s = &g_subs[i];
        if (!(s->state == SUB_WANT || (s->state == SUB_REFUSED && s->retry_at <= now_ms))) continue;
        s->state = SUB_SENT;
        s->ref = ++g_next_ref ? g_next_ref : ++g_next_ref;
        f.kind = NW_WS_SUB;
        f.ref = s->ref;
        f.what = s->what;
        f.gid_len = s->gid_len;
        memcpy(f.gid, s->gid, s->gid_len);
        // What this client already holds of the game, so the server sends
        // only a newer view (the catch-up).
        gate *g = s->gid_len ? gate_find(s->gid, s->gid_len, 0) : NULL;
        if (g) f.version = g->view_v > g->cast_v ? g->view_v : g->cast_v;
        return out_frame(&f);
    }
    if (now_ms - g_last_ping >= NC_PING_MS) {
        g_last_ping = now_ms;
        f.kind = NW_WS_PING;
        return out_frame(&f);
    }
    return NULL;
}

int nc_stream_due_ms(int64_t now_ms) {
    if (!g_open) return -1;
    if (g_signed_in && g_auth_sent_gen != g_session_gen) return 0;
    if (g_n_unsubs) return 0;
    int64_t due = g_last_ping + NC_PING_MS;
    for (int i = 0; i < NC_SUBS_MAX; i++) {
        if (g_subs[i].state == SUB_WANT) return 0;
        if (g_subs[i].state == SUB_REFUSED && g_subs[i].retry_at < due) due = g_subs[i].retry_at;
    }
    int64_t d = due - now_ms;
    return d < 0 ? 0 : d > NC_PING_MS ? NC_PING_MS : (int)d;
}
