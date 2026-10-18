// netclient.c - the client of the backend's binary surface (netclient.h).
#include "netclient.h"
#include "roster.h"
#include "table.h"
#include "view.h"
#include "evwire.h"
#include "../../../shared/c/mqtt/mqtt.h"
#include <string.h>

// ---- state -------------------------------------------------------------------------

typedef struct {
    uint8_t  what;
    uint8_t  gid_len;
    char     gid[NW_GID_MAX];
    uint8_t  state;        // SUB_WANT, SUB_SENT, SUB_LIVE, SUB_REFUSED
    uint16_t holds;        // the host's nc_stream_sub calls not yet let go
    uint8_t  pinned;       // held by this file (a game just created) until the host lets go
    uint16_t pid;          // the SUBSCRIBE's packet id while SENT
    int64_t  retry_at;
    int32_t  backoff;
    int64_t  quiet_at;     // a LIVE game's: heard nothing of it by then, a push was lost (quiet_check)
    uint8_t  over;         // the game's last board applied was its terminal one: nothing more to hear
} sub;
enum { SUB_FREE = 0, SUB_WANT, SUB_SENT, SUB_LIVE, SUB_REFUSED };

typedef struct {
    uint8_t  used;
    uint8_t  gid_len;
    char     gid[NW_GID_MAX];
    uint8_t  has_view, has_cast, has_answer;
    uint8_t  seat_known, seated;   // whether the user sits at the game, as the last envelope or cast said
    uint32_t view_v;       // the dashboard's: the newest view pushed
    uint32_t cast_v;       // the table's: the newest cast applied
    uint32_t answer_v;     // the table's: the newest view answered and applied
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
static uint8_t  g_open;                 // the host's socket is open
static uint8_t  g_mq;                   // MQ_*: where the MQTT session on it is
static int64_t  g_last_out;             // when a packet last went out: the keep-alive runs from it
static uint16_t g_next_pid;
static int32_t  g_reconnect_ms;
static uint32_t g_conn_seq;             // makes each connection's client id its own
static int64_t  g_forced_at;            // when a refused CONNECT last forced a refresh
static uint32_t g_quiet_mix;            // the quiet jitter's running state
// The broker's bytes: whole packets parsed in place (a pushed message's payload
// is read where it lies), a packet cut across WebSocket messages kept for the next.
static uint8_t  g_mq_in[NC_MQ_IN_MAX];
static int      g_mq_in_len, g_mq_in_off;
enum { MQ_SHUT = 0, MQ_OPENED, MQ_CONNECTING, MQ_LIVE };

// What the last feed changed. A payload is in g_mq_in (a pushed message, where
// it lies) or g_frame (the envelope a refetch answer carried, copied there).
static NcEvent  g_events[NC_EVENTS_MAX];
static uint32_t g_event_at[NC_EVENTS_MAX];
static const uint8_t *g_event_base[NC_EVENTS_MAX];   // g_frame (an answer) or g_mq_in (a push)
static int      g_n_events;
static uint32_t g_session_told;         // the session generation the host was last told
static uint8_t  g_boards;               // the host shows boards and never decodes a cast (nc_set_boards)
static uint8_t  g_frame[NC_FRAME_MAX];
static NwFrame  g_nwf;                  // the frame decoded or being encoded
static NcOut    g_out;

// One row is read at a time (what a reader answers is good until the next
// call), so every kind shares one buffer.
static union {
    NcGameRow    game;
    NcChatRow    chat;
    NcBotRow     bot;
    NcBoardRow   board;
    NcHistoryRow hist;
} g_row;
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

static void pump_reset(void);
static void book_for(const NwSession *s);
static void book_reset(void);

void nc_reset(void) {
    pump_reset();
    book_reset();
    signed_out();
    memset(&g_error, 0, sizeof g_error);
    memset(g_subs, 0, sizeof g_subs);
    memset(g_gates, 0, sizeof g_gates);
    g_n_unsubs = 0;
    g_gate_clock = 0;
    g_open = 0;
    g_mq = MQ_SHUT;
    g_last_out = 0;
    g_next_pid = 0;
    g_reconnect_ms = 0;
    g_forced_at = 0;
    g_mq_in_len = g_mq_in_off = 0;
    g_n_events = 0;
    g_session_told = g_session_gen;
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
    book_for(&s);
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
    book_for(&s);
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

const NcRequest *nc_req_password(const char *current, int current_len, const char *pw, int pw_len, int64_t now_ms) {
    if (!current || !pw || current_len < 0 || pw_len < 0 || current_len > NW_PASSWORD_MAX || pw_len > NW_PASSWORD_MAX) return NULL;
    int r;
    const NcRequest *q = authed(NC_POST, "/v1/auth/password", now_ms, &r);
    if (!q || r) return q;
    NwRequest w;
    memset(&w, 0, sizeof w);
    w.kind = NW_PASSWORD;
    w.password_len = (uint8_t)current_len;
    memcpy(w.password, current, (size_t)current_len);
    w.new_password_len = (uint8_t)pw_len;
    memcpy(w.new_password, pw, (size_t)pw_len);
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
// The bots and the leaderboard are public and the same for everyone: they carry
// no token, so the edge caches one copy of each (c_base DESIGN.md "Edge cache").
// now_ms is kept so no host changes its call.
const NcRequest *nc_req_bots(int64_t now_ms) { (void)now_ms; return get("/v1/bots", NULL, 0, 0, 0); }
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

const NcRequest *nc_req_stream(void) { return begin(NC_GET, "/mqtt"); }

// ---- answers -------------------------------------------------------------------------

int nc_retry_ms(int method, int attempt, int status, const uint8_t *body, int len) {
    AskRefusal r;
    if (method != NC_GET || status != 503 || attempt < 0 || attempt >= NC_BUSY_TRIES) return 0;
    if (!body || ask_refusal_decode(body, len, &r) != ASK_OK || r.why != ASK_WHY_BUSY) return 0;
    return NC_BUSY_FIRST_MS << attempt;
}

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
    g_row.game.version = v;
    g_row.game.env_off = off_in(body, env.p);
    g_row.game.env_len = env.len;
    return &g_row.game;
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
    g_row.game.version = v;
    g_row.game.env_off = off_in(body, env.p);
    g_row.game.env_len = env.len;
    return &g_row.game;
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
    chat_copy(&g_row.chat, &c);
    return &g_row.chat;
}

const NcBotRow *nc_bot_row(const uint8_t *body, int len, int i) {
    NwBot b;
    if (nw_get_bot(body, len, i, &b) != NW_OK) return NULL;
    memset(&g_row.bot, 0, sizeof g_row.bot);
    g_row.bot.id_len = b.id_len;
    memcpy(g_row.bot.id, b.id, b.id_len);
    g_row.bot.nickname_len = b.nickname_len;
    memcpy(g_row.bot.nickname, b.nickname, b.nickname_len);
    g_row.bot.key_len = b.key_len;
    memcpy(g_row.bot.key, b.key, b.key_len);
    return &g_row.bot;
}

const NcBoardRow *nc_board_row(const uint8_t *body, int len, int i) {
    NwBoardRow b;
    if (nw_get_board(body, len, i, &b) != NW_OK) return NULL;
    memset(&g_row.board, 0, sizeof g_row.board);
    g_row.board.is_bot = b.is_bot;
    g_row.board.id_len = b.id_len;
    memcpy(g_row.board.id, b.id, b.id_len);
    g_row.board.name_len = b.name_len;
    memcpy(g_row.board.name, b.name, b.name_len);
    g_row.board.rating = b.rating;
    g_row.board.games = b.games;
    return &g_row.board;
}

const NcHistoryRow *nc_history_row(const uint8_t *body, int len, int i) {
    NwHistoryRow h;
    if (nw_get_history(body, len, i, &h) != NW_OK) return NULL;
    memset(&g_row.hist, 0, sizeof g_row.hist);
    g_row.hist.id_len = h.id_len;
    memcpy(g_row.hist.id, h.id, h.id_len);
    g_row.hist.created_ms = h.created_ms;
    g_row.hist.n_players = h.n_players;
    g_row.hist.seat = h.seat;
    g_row.hist.rank = h.rank;
    g_row.hist.fool = h.fool;
    g_row.hist.duration_s = h.duration_s;
    return &g_row.hist;
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

// ---- the pump -------------------------------------------------------------------------

enum { SLOT_FREE = 0, SLOT_QUEUED, SLOT_SENT };

typedef struct {
    uint8_t  state;
    uint8_t  attempts;      // sends that never arrived
    uint8_t  reauthed;      // sent again once after a 401
    uint8_t  busy;          // sent again after a 503 BUSY (nc_retry_ms)
    uint8_t  own;           // this file's own refetch: no ticket for the host, its view an event
    uint32_t min_version;   // own: not sent once something this new was applied (0: always)
    uint32_t ticket;
    uint32_t sent_id;
    int64_t  not_before;
    // The intent, its strings and bytes in g_arena at `at`: name, password, bytes.
    uint8_t  kind, flag;
    uint8_t  gid_len;       char gid[NW_GID_MAX];
    int64_t  intent;
    uint16_t at;
    uint8_t  name_len, password_len;
    uint16_t bytes_len;
} slot;

// Two pools in one array. The host's intents take [0, NC_PUMP_MAX) and only
// those: what the host is refused is what the host itself has waiting. This
// file's own refetches (a reconcile, a catch-up, a board's read) take
// [NC_PUMP_MAX, NC_SLOTS), so background work never refuses a host intent,
// however fast games go. They are one per game and carry no arena bytes;
// NC_OWN_MAX is NC_SUBS_MAX so a reconnect's catch-up of every game the host
// holds a stream of fits at once. A cap on own work inside one shared pool was
// the other way: it still lets own work shrink what the host has, by an amount
// that depends on timing, where this pool gives the host a fixed NC_PUMP_MAX.
#define NC_OWN_MAX NC_SUBS_MAX
#define NC_SLOTS   (NC_PUMP_MAX + NC_OWN_MAX)
static slot     g_slots[NC_SLOTS];
// What the waiting intents carry beyond a game id: most carry nothing, a move a
// few bytes, a chat line or an ask up to NC_BODY_MAX. Shared rather than a full
// NcIntent per slot, which was most of the pump's memory.
#define NC_ARENA_BYTES 2560
static uint8_t  g_arena[NC_ARENA_BYTES];
static uint32_t g_ticket;
static uint32_t g_req_id;
static uint32_t g_refresh_id;           // the refresh in flight, 0 none
static int64_t  g_refresh_not_before;
static uint8_t  g_refresh_fails;
static NcDone   g_done;                 // nc_on_response's
static NcDone   g_dropped[NC_PUMP_MAX]; // finished with no answer, for nc_next_done
static int      g_n_dropped;
static NcDone   g_dropped_out;

// The book of games and the stream, below.
static gate *gate_find(const char *gid, int gid_len, int make);
static uint32_t any_top(const gate *g);
static int answer_pass(gate *g, uint32_t version);
static int envelope_names(const uint8_t *p, int len, char *gid, uint8_t *gid_len, uint32_t *version);
static void events_begin(void);
static NcEvent *event_add(uint8_t kind);
static int events_end(void);
static sub *sub_make(int what, const char *gid, int gid_len);
static sub *sub_find(int what, const char *gid, int gid_len);
static int same_id(const char *a, int a_len, const char *b, int b_len);
static void seat_learn(gate *g, const uint8_t *env, uint32_t len);
static int stream_wants_token(int64_t now_ms);
static void heard(const char *gid, int gid_len, int over, int64_t now_ms);
static int envelope_over(const uint8_t *env, uint32_t len);
static void quiet_check(int64_t now_ms);

static void pump_reset(void) {
    memset(g_slots, 0, sizeof g_slots);
    g_refresh_id = 0;
    g_refresh_not_before = 0;
    g_refresh_fails = 0;
    g_n_dropped = 0;
}

static int needs_user(uint8_t kind) {
    return kind != NC_DO_SIGN_UP && kind != NC_DO_SIGN_IN && kind != NC_DO_LEADERBOARD;
}

// A read: sent again when it never arrived.
static int idempotent(uint8_t kind) {
    return kind == NC_DO_GAMES || kind == NC_DO_OPEN_GAME || kind == NC_DO_CHAT_HISTORY || kind == NC_DO_BOTS ||
           kind == NC_DO_LEADERBOARD || kind == NC_DO_HISTORY || kind == NC_DO_REPLAY;
}

// The request an intent asks for now: the refresh first when the token is due.
static const NcRequest *build(const NcIntent *in, int64_t now) {
    const char *g = in->gid;
    int gl = in->gid_len;
    switch (in->kind) {
    case NC_DO_SIGN_UP: return nc_req_signup(in->name, in->name_len, in->password, in->password_len);
    case NC_DO_SIGN_IN: return nc_req_signin(in->name, in->name_len, in->password, in->password_len);
    case NC_DO_SIGN_OUT: return nc_req_signout(in->flag != 0, now);
    case NC_DO_PASSWORD:
        return in->bytes_len > NW_PASSWORD_MAX ? NULL
                                               : nc_req_password((const char *)in->bytes, in->bytes_len, in->password, in->password_len, now);
    case NC_DO_DELETE_ACCOUNT: return nc_req_delete_account(now);
    case NC_DO_CREATE: return nc_req_create(now);
    case NC_DO_PLAY: return nc_req_action(g, gl, in->bytes, in->bytes_len, in->intent, now);
    case NC_DO_ASK: return nc_req_meta(in->bytes, in->bytes_len, now);
    case NC_DO_GAMES: return nc_req_games(now);
    case NC_DO_OPEN_GAME: return nc_req_game(g, gl, now);
    case NC_DO_CHAT_HISTORY: return nc_req_chats(g, gl, now);
    case NC_DO_SEND_CHAT: return nc_req_chat_send(g, gl, (const char *)in->bytes, in->bytes_len, now);
    case NC_DO_BOTS: return nc_req_bots(now);
    case NC_DO_LEADERBOARD: return nc_req_board();
    case NC_DO_HISTORY: return nc_req_history(now);
    case NC_DO_REPLAY: return nc_req_replay(g, gl, now);
    default: return NULL;
    }
}

static void done_begin(NcDone *d, const slot *sl) {
    memset(d, 0, sizeof *d);
    d->ticket = sl->ticket;
    d->kind = sl->kind;
    d->gid_len = sl->gid_len;
    memcpy(d->gid, sl->gid, sl->gid_len);
}

static int slot_extra(const slot *sl) { return sl->name_len + sl->password_len + sl->bytes_len; }

// The intent a slot holds, whole again.
static void slot_intent(const slot *sl, NcIntent *in) {
    memset(in, 0, sizeof *in);
    in->kind = sl->kind;
    in->flag = sl->flag;
    in->gid_len = sl->gid_len;
    memcpy(in->gid, sl->gid, sl->gid_len);
    in->intent = sl->intent;
    const uint8_t *p = g_arena + sl->at;
    in->name_len = sl->name_len;
    memcpy(in->name, p, sl->name_len);
    in->password_len = sl->password_len;
    memcpy(in->password, p + sl->name_len, sl->password_len);
    in->bytes_len = sl->bytes_len;
    memcpy(in->bytes, p + sl->name_len + sl->password_len, sl->bytes_len);
}

// The lowest offset in g_arena where `need` bytes overlap no waiting slot's, or -1.
static int arena_find(int need) {
    if (need == 0) return 0;
    for (int c = -1; c < NC_SLOTS; c++) {
        int at = c < 0 ? 0 : g_slots[c].state != SLOT_FREE && slot_extra(&g_slots[c]) ? g_slots[c].at + slot_extra(&g_slots[c]) : -1;
        if (at < 0 || at + need > NC_ARENA_BYTES) continue;
        int clear = 1;
        for (int i = 0; i < NC_SLOTS && clear; i++) {
            const slot *o = &g_slots[i];
            int n = slot_extra(o);
            if (o->state != SLOT_FREE && n && at < o->at + n && o->at < at + need) clear = 0;
        }
        if (clear) return at;
    }
    return -1;
}

static void done_error(NcDone *d) {
    d->why = g_error.why;
    d->code = g_error.code;
    d->detail = g_error.detail;
    d->status = g_error.status;
}

// Finishes a slot with no answer of its own (nc_next_done); this file's own
// refetch is simply let go.
static void drop(slot *sl, uint8_t why, int status) {
    if (!sl->own && g_n_dropped < NC_PUMP_MAX) {
        NcDone *d = &g_dropped[g_n_dropped++];
        done_begin(d, sl);
        d->why = why;
        d->status = status;
    }
    memset(sl, 0, sizeof *sl);
}

static void drop_user_slots(uint8_t why, int status) {
    for (int i = 0; i < NC_SLOTS; i++)
        if (g_slots[i].state != SLOT_FREE && needs_user(g_slots[i].kind)) drop(&g_slots[i], why, status);
}

// The session is over (signed out, deleted, its refresh refused): forget it and
// what it was subscribed to, and finish whatever waited for it.
static void session_over(void) {
    signed_out();
    memset(g_subs, 0, sizeof g_subs);
    g_n_unsubs = 0;
    memset(g_gates, 0, sizeof g_gates);
    g_refresh_id = 0;
    drop_user_slots(ASK_WHY_AUTH, 0);
}

// The book (versions applied, subscriptions, this file's own refetches) is one
// user's: another user's session starts it over.
static uint8_t g_book_user_len;
static char    g_book_user[NW_ID_MAX];
// A book that was nobody's yet (a page that holds its streams before its kept
// session is restored, as the web's do) is the first user's: what it holds is
// theirs, not another user's to forget.
static void book_reset(void) { g_book_user_len = 0; }

static void book_for(const NwSession *s) {
    if (same_id(g_book_user, g_book_user_len, s->user_id, s->user_id_len)) return;
    int had_user = g_book_user_len != 0;
    g_book_user_len = s->user_id_len;
    memcpy(g_book_user, s->user_id, s->user_id_len);
    if (!had_user) return;
    memset(g_gates, 0, sizeof g_gates);
    memset(g_subs, 0, sizeof g_subs);
    g_n_unsubs = 0;
    for (int i = NC_PUMP_MAX; i < NC_SLOTS; i++) memset(&g_slots[i], 0, sizeof g_slots[i]);
}

static int32_t backoff_ms(int attempts) {
    int64_t ms = NC_RETRY_FIRST_MS;
    for (int i = 1; i < attempts && ms < NC_RETRY_MAX_MS; i++) ms *= 2;
    return (int32_t)(ms < NC_RETRY_MAX_MS ? ms : NC_RETRY_MAX_MS);
}

// A free slot of the host's pool, or of this file's own when `own`.
static slot *slot_take(const NcIntent *in, int own) {
    int need = in->name_len + in->password_len + in->bytes_len;
    int at = arena_find(need);
    if (at < 0) return NULL;
    for (int i = own ? NC_PUMP_MAX : 0; i < (own ? NC_SLOTS : NC_PUMP_MAX); i++) {
        slot *sl = &g_slots[i];
        if (sl->state != SLOT_FREE) continue;
        memset(sl, 0, sizeof *sl);
        sl->kind = in->kind;
        sl->flag = in->flag;
        sl->gid_len = in->gid_len;
        memcpy(sl->gid, in->gid, in->gid_len);
        sl->intent = in->intent;
        sl->at = (uint16_t)at;
        sl->name_len = in->name_len;
        sl->password_len = in->password_len;
        sl->bytes_len = in->bytes_len;
        memcpy(g_arena + at, in->name, in->name_len);
        memcpy(g_arena + at + in->name_len, in->password, in->password_len);
        memcpy(g_arena + at + in->name_len + in->password_len, in->bytes, in->bytes_len);
        sl->state = SLOT_QUEUED;
        sl->own = (uint8_t)(own != 0);
        if (++g_ticket == 0) g_ticket = 1;
        sl->ticket = g_ticket;
        return sl;
    }
    return NULL;
}

uint32_t nc_do(const NcIntent *in, int64_t now_ms) {
    memset(&g_error, 0, sizeof g_error);
    if (!in) { g_error.why = ASK_WHY_MALFORMED; return 0; }
    if (needs_user(in->kind) && !g_signed_in) { g_error.why = ASK_WHY_AUTH; return 0; }
    // Whether the wire can carry it, now rather than when it is sent. The trial
    // build must not disturb the request nc_next_request last handed out.
    NcRequest kept = g_req;
    int ok = build(in, now_ms) != NULL;
    g_req = kept;
    if (!ok) { g_error.why = ASK_WHY_MALFORMED; return 0; }
    slot *sl = slot_take(in, 0);
    // The host's NC_PUMP_MAX (or NC_ARENA_BYTES) already waiting: busy for a
    // moment, as a throttled server is (status 0: nothing was sent). The host
    // has its pool to itself, so this is only ever its own intents piling up.
    if (!sl) { g_error.why = ASK_WHY_BUSY; return 0; }
    return sl->ticket;
}

// This file's own read of a game, sent at `not_before`, unless something at
// least `min_version` new has been applied by then. One per game at a time.
static int refetch_queue(const char *gid, int gid_len, int64_t not_before, uint32_t min_version) {
    if (!g_signed_in || gid_len <= 0 || gid_len > NW_GID_MAX) return 0;
    for (int i = NC_PUMP_MAX; i < NC_SLOTS; i++) {
        slot *sl = &g_slots[i];
        if (sl->state != SLOT_FREE && same_id(sl->gid, sl->gid_len, gid, gid_len)) {
            if (sl->state == SLOT_QUEUED && not_before < sl->not_before) sl->not_before = not_before;
            if (min_version > sl->min_version) sl->min_version = min_version;
            return 1;
        }
    }
    NcIntent in;
    memset(&in, 0, sizeof in);
    in.kind = NC_DO_OPEN_GAME;
    in.gid_len = (uint8_t)gid_len;
    memcpy(in.gid, gid, (size_t)gid_len);
    slot *sl = slot_take(&in, 1);
    if (!sl) return 0;   // full: the next push that finds a gap asks again
    sl->not_before = not_before;
    sl->min_version = min_version;
    return 1;
}

// An own refetch that something newer has made moot.
static int caught_up(const slot *sl) {
    if (!sl->own || !sl->min_version) return 0;
    gate *g = gate_find(sl->gid, sl->gid_len, 0);
    return g && (g->has_view || g->has_cast || g->has_answer) && any_top(g) >= sl->min_version;
}

// Lets go of every own refetch still waiting to be sent that is moot. The one
// place a caught-up refetch is let go: every gate rises inside a feed and every
// refetch is queued inside one, so the end of each feed is the moment it can
// first be moot (and the slot is free at once, not when its time comes).
static void own_settle(void) {
    for (int i = NC_PUMP_MAX; i < NC_SLOTS; i++)
        if (g_slots[i].state == SLOT_QUEUED && caught_up(&g_slots[i])) memset(&g_slots[i], 0, sizeof g_slots[i]);
}

// The host let go of game `gid`'s stream: its own refetch still waiting to be
// sent has nobody to tell. One already sent is read as usual.
static void own_forget(const char *gid, int gid_len) {
    for (int i = NC_PUMP_MAX; i < NC_SLOTS; i++)
        if (g_slots[i].state == SLOT_QUEUED && same_id(g_slots[i].gid, g_slots[i].gid_len, gid, gid_len))
            memset(&g_slots[i], 0, sizeof g_slots[i]);
}

const NcRequest *nc_next_request(int64_t now_ms) {
    quiet_check(now_ms);
    // The stream's CONNECT carries the token: a socket waiting to connect with
    // one about to expire asks for the refresh itself, whatever else waits.
    if (stream_wants_token(now_ms) && !g_refresh_id && g_refresh_not_before <= now_ms) {
        const NcRequest *q = nc_req_refresh();
        if (q) {
            if (++g_req_id == 0) g_req_id = 1;
            g_req.id = g_req_id;
            g_refresh_id = g_req_id;
            return &g_req;
        }
    }
    for (int pass = 0; pass < NC_SLOTS; pass++) {
        slot *pick = NULL;
        for (int i = 0; i < NC_SLOTS; i++) {
            slot *sl = &g_slots[i];
            if (sl->state != SLOT_QUEUED || sl->not_before > now_ms) continue;
            if (needs_user(sl->kind) && (g_refresh_id || g_refresh_not_before > now_ms)) continue;
            if (!pick || sl->ticket - pick->ticket > 0x7fffffffu) pick = sl;   // the oldest ticket first
        }
        if (!pick) return NULL;
        if (needs_user(pick->kind) && !g_signed_in) { drop(pick, ASK_WHY_AUTH, 0); continue; }
        NcIntent in;
        slot_intent(pick, &in);
        const NcRequest *q = build(&in, now_ms);
        if (!q) { drop(pick, ASK_WHY_MALFORMED, 0); continue; }
        if (++g_req_id == 0) g_req_id = 1;
        g_req.id = g_req_id;
        if (q->is_refresh) {
            g_refresh_id = g_req_id;
            return &g_req;
        }
        pick->state = SLOT_SENT;
        pick->sent_id = g_req_id;
        return &g_req;
    }
    return NULL;
}

static void refresh_answered(int status, const uint8_t *body, int len, int64_t now_ms) {
    g_refresh_id = 0;
    int why = nc_session_read(status, body, len);
    if (why == 0) { g_refresh_fails = 0; g_refresh_not_before = 0; return; }
    if (!g_signed_in) { session_over(); return; }   // the token was refused: the session is over
    // It never arrived, or the server failed: try again later, then give up on what waits.
    if (++g_refresh_fails >= NC_PUMP_TRIES) {
        g_refresh_fails = 0;
        g_refresh_not_before = 0;
        drop_user_slots(g_error.why, status);
        return;
    }
    g_refresh_not_before = now_ms + backoff_ms(g_refresh_fails);
}

// An answered envelope of game `gid` through the book: applied, or stale.
// `own`: this file's refetch, so it becomes an event (its payload copied to g_frame).
static int answer_view(NcDone *d, const char *gid, int gid_len, uint32_t version,
                       const uint8_t *body, uint32_t off, uint32_t len, int own, int64_t now_ms) {
    gate *g = gate_find(gid, gid_len, 1);
    if (!answer_pass(g, version)) { d->stale = 1; heard(gid, gid_len, -1, now_ms); return 0; }
    seat_learn(g, body + off, len);
    heard(gid, gid_len, envelope_over(body + off, len), now_ms);
    d->version = version;
    if (!own) { d->a_off = off; d->a_len = len; return 1; }
    if (len > NC_FRAME_MAX) return 0;
    NcEvent *e = event_add(NC_EV_VIEW);
    if (!e) return 0;
    g_event_base[g_n_events - 1] = g_frame;
    memcpy(g_frame, body + off, len);
    e->what = NW_SUB_GAME;
    e->gid_len = (uint8_t)gid_len;
    memcpy(e->gid, gid, (size_t)gid_len);
    e->version = version;
    e->payload_len = len;
    return 1;
}

static void read_answer(NcDone *d, const NcIntent *in, int own, int status, const uint8_t *body, int len, int64_t now_ms) {
    switch (d->kind) {
    case NC_DO_SIGN_UP:
    case NC_DO_SIGN_IN:
        nc_session_read(status, body, len);
        break;
    case NC_DO_SIGN_OUT:
        nc_status_read(status, body, len);
        session_over();   // signed out here whatever the server answered
        break;
    case NC_DO_DELETE_ACCOUNT:
        if (nc_status_read(status, body, len) == 0) session_over();
        break;
    case NC_DO_CREATE:
    case NC_DO_ASK: {
        if (nc_status_read(status, body, len) != 0) break;
        char gid[NW_GID_MAX];
        uint8_t gl = 0;
        uint32_t version = 0;
        int named = envelope_names(body, len, gid, &gl, &version);
        TableAsk ask;
        int asked = d->kind == NC_DO_ASK && ask_decode(in->bytes, in->bytes_len, &ask) == ASK_OK;
        if (asked && ask.gid_len && ask.gid_len <= NW_GID_MAX) {
            memcpy(gid, ask.gid, ask.gid_len);
            gl = ask.gid_len;
        }
        if (!gl) break;
        d->gid_len = gl;
        memcpy(d->gid, gid, gl);
        // The answer is the asker's board only when it moved the asker: a create,
        // a join, their own leave. Every other ask's change arrives as its cast,
        // which animates, so its answer is not a board for the table to take.
        int own_exit = asked && ask.kind == ASK_EXIT && ask.bot_len == 0 &&
                       (ask.player_len == 0 || same_id(ask.player, ask.player_len, g_session.user_id, g_session.user_id_len));
        int moved = d->kind == NC_DO_CREATE || (asked && ask.kind == ASK_JOIN) || own_exit;
        if (!moved || !named) break;
        // The answer says whether the asker now sits there (the envelope's
        // seat), which picks the casts taken from now on: the seat's or the
        // spectators', both of which a game's subscription hears.
        answer_view(d, gid, gl, version, body, 0, (uint32_t)len, 0, now_ms);
        // A create's game is heard from now on.
        if (d->kind == NC_DO_CREATE && !g_boards) {
            sub *s = sub_make(NW_SUB_GAME, gid, gl);
            if (s) s->pinned = 1;
        }
        break;
    }
    case NC_DO_PLAY: {
        const NcAction *a = nc_action_read(status, body, len);
        if (!a) break;
        d->action_status = a->status;
        d->reject_code = a->reject_code;
        d->version = a->version;
        // Refused over a newer game than the one applied here: its pushes are late
        // or lost. Give them a moment, then take the game whole.
        if (a->status != TABLE_STATUS_APPLIED) {
            gate *g = gate_find(in->gid, in->gid_len, 0);
            if (!g || any_top(g) < a->version)
                refetch_queue(in->gid, in->gid_len, now_ms + NC_RECONCILE_MS, a->version);
        }
        break;
    }
    case NC_DO_OPEN_GAME: {
        const NcGameRow *r = nc_view_read(status, body, len);
        if (r) answer_view(d, in->gid, in->gid_len, r->version, body, r->env_off, r->env_len, own, now_ms);
        break;
    }
    case NC_DO_REPLAY: {
        const NcReplay *r = nc_replay_read(status, body, len);
        if (r) { d->a_off = r->moves_off; d->a_len = r->moves_len; d->b_off = r->extras_off; d->b_len = r->extras_len; }
        break;
    }
    case NC_DO_GAMES: case NC_DO_CHAT_HISTORY: case NC_DO_BOTS: case NC_DO_LEADERBOARD: case NC_DO_HISTORY: {
        uint8_t kind = d->kind == NC_DO_GAMES ? NW_GAMES : d->kind == NC_DO_CHAT_HISTORY ? NW_CHATS
                     : d->kind == NC_DO_BOTS ? NW_BOTS : d->kind == NC_DO_LEADERBOARD ? NW_BOARD : NW_HISTORY;
        int n = nc_list_read(status, body, len, kind);
        if (n >= 0) {
            d->n_rows = n;
            d->list_kind = kind;
            if (kind == NW_HISTORY) {
                const NcHistoryHead *h = nc_history_head(body, len);
                if (h) { d->has_rating = h->has_rating; d->rating = h->rating; }
            }
        }
        break;
    }
    default:   // PASSWORD, SEND_CHAT: accepted or refused
        nc_status_read(status, body, len);
        break;
    }
    done_error(d);
}

static const NcDone *answered(uint32_t id, int status, const uint8_t *body, int len, int64_t now_ms) {
    if (!id) return NULL;
    if (len < 0 || (len > 0 && !body)) len = 0;
    if (id == g_refresh_id) { refresh_answered(status, body, len, now_ms); return NULL; }
    slot *sl = NULL;
    for (int i = 0; i < NC_SLOTS; i++)
        if (g_slots[i].state == SLOT_SENT && g_slots[i].sent_id == id) { sl = &g_slots[i]; break; }
    if (!sl) return NULL;   // an answer for nothing waiting (it was dropped meanwhile)
    if (status == 0 && idempotent(sl->kind) && sl->attempts + 1 < NC_PUMP_TRIES) {
        // It never arrived: a read goes again, later.
        sl->attempts++;
        sl->state = SLOT_QUEUED;
        sl->not_before = now_ms + backoff_ms(sl->attempts);
        return NULL;
    }
    int wait = idempotent(sl->kind) ? nc_retry_ms(NC_GET, sl->busy, status, body, len) : 0;
    if (wait > 0) {
        // The server's store was throttled: a read goes again, a little later.
        sl->busy++;
        sl->state = SLOT_QUEUED;
        sl->not_before = now_ms + wait;
        return NULL;
    }
    if (status == 401 && needs_user(sl->kind) && g_signed_in && !sl->reauthed) {
        // The token was not taken: refresh it, then send this once more.
        sl->reauthed = 1;
        sl->state = SLOT_QUEUED;
        g_session.expires_at_ms = 0;
        return NULL;
    }
    slot done_slot = *sl;
    NcIntent in;
    slot_intent(sl, &in);   // before the slot (and its arena bytes) are let go
    memset(sl, 0, sizeof *sl);
    done_begin(&g_done, &done_slot);
    if (status == 0) {
        memset(&g_error, 0, sizeof g_error);
        g_error.why = ASK_WHY_INTERNAL;
        done_error(&g_done);
    } else {
        read_answer(&g_done, &in, done_slot.own, status, body, len, now_ms);
    }
    return done_slot.own ? NULL : &g_done;
}

const NcDone *nc_on_response(uint32_t id, int status, const uint8_t *body, int len, int64_t now_ms) {
    events_begin();
    const NcDone *d = answered(id, status, body, len, now_ms);
    events_end();
    return d;
}

const NcDone *nc_next_done(void) {
    if (g_n_dropped == 0) return NULL;
    g_dropped_out = g_dropped[0];
    memmove(g_dropped, g_dropped + 1, (size_t)(g_n_dropped - 1) * sizeof g_dropped[0]);
    g_n_dropped--;
    return &g_dropped_out;
}

int nc_pump_due_ms(int64_t now_ms) {
    if (g_n_dropped) return 0;
    int64_t due = -1;
    if (stream_wants_token(now_ms) && !g_refresh_id)
        due = g_refresh_not_before > now_ms ? g_refresh_not_before - now_ms : 0;
    for (int i = 0; i < NC_SLOTS; i++) {
        const slot *sl = &g_slots[i];
        if (sl->state != SLOT_QUEUED) continue;
        int64_t at = sl->not_before;
        if (needs_user(sl->kind)) {
            if (g_refresh_id) continue;   // the refresh's answer wakes it
            if (g_refresh_not_before > at) at = g_refresh_not_before;
        }
        int64_t ms = at > now_ms ? at - now_ms : 0;
        if (due < 0 || ms < due) due = ms;
    }
    return due > INT32_MAX ? INT32_MAX : (int)due;
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

// ---- the stream --------------------------------------------------------------------------
//
// MQTT 3.1.1 over the host's one WebSocket (shared/c/mqtt): this file writes
// every packet and reads every one the broker sends; the host carries bytes.
// CONNECT carries the access token as its password and a client id of
// "<user>-<this connection>"; a subscription is a SUBSCRIBE of its topics:
//   NW_SUB_GAMES          u/<user>/v
//   NW_SUB_CHAT   <game>  u/<user>/m/<game>
//   NW_SUB_GAME   <game>  u/<user>/c/<game> and s/<game>, both: which casts are
//                         taken is the book's (the seat's when the user sits there)
// Every push's payload is the server's nw1 message (netwire.h). A game
// subscription answered (SUBACK) takes the game whole once (the catch-up, an
// HTTP read) unless something newer arrives first, so nothing pushed between
// the host's read and the subscription going live is lost. A refusal has no
// code on the wire (the broker ends the connection), so a socket closed before
// its CONNACK refreshes the token before the next CONNECT (at most once every
// NC_FORCE_REFRESH_MS).

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
    if (!gid || gid_len <= 0 || gid_len > NW_GID_MAX) return 0;
    // A game id becomes a topic level: only bytes a level can hold.
    for (int i = 0; i < gid_len; i++) {
        char c = gid[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static sub *sub_make(int what, const char *gid, int gid_len) {
    sub *s = sub_find(what, gid, gid_len);
    if (s) return s;
    for (int i = 0; i < NC_SUBS_MAX; i++) {
        if (g_subs[i].state != SUB_FREE) continue;
        s = &g_subs[i];
        memset(s, 0, sizeof *s);
        s->what = (uint8_t)what;
        s->gid_len = (uint8_t)gid_len;
        if (gid_len) memcpy(s->gid, gid, (size_t)gid_len);
        s->state = SUB_WANT;
        s->backoff = NC_RETRY_FIRST_MS;
        // A pending unsubscribe of the same thing is moot.
        for (int j = 0; j < g_n_unsubs; j++)
            if (g_unsubs[j].what == what && same_id(g_unsubs[j].gid, g_unsubs[j].gid_len, gid, gid_len)) {
                g_unsubs[j] = g_unsubs[--g_n_unsubs];
                break;
            }
        return s;
    }
    return NULL;
}

int nc_stream_sub(int what, const char *gid, int gid_len) {
    if (!sub_args_ok(what, gid, gid_len)) return NW_E_WIRE;
    sub *s = sub_make(what, gid, gid_len);
    if (!s) return NW_E_CAP;
    s->holds++;
    return NW_OK;
}

int nc_stream_unsub(int what, const char *gid, int gid_len) {
    if (!sub_args_ok(what, gid, gid_len)) return NW_E_WIRE;
    sub *s = sub_find(what, gid, gid_len);
    if (!s) return NW_OK;
    if (s->holds > 1) { s->holds--; return NW_OK; }
    // The last hold (a pin the host never held is let go with it).
    int told = s->state == SUB_SENT || s->state == SUB_LIVE;
    memset(s, 0, sizeof *s);
    if (what == NW_SUB_GAME) own_forget(gid, gid_len);
    if (told && g_mq == MQ_LIVE) {
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
    g_mq = MQ_OPENED;
    g_last_out = now_ms;
    g_n_unsubs = 0;
    g_mq_in_len = g_mq_in_off = 0;
    for (int i = 0; i < NC_SUBS_MAX; i++)
        if (g_subs[i].state != SUB_FREE) { g_subs[i].state = SUB_WANT; g_subs[i].retry_at = 0; }
}

int nc_stream_closed(int64_t now_ms) {
    // Closed with a CONNECT out and no CONNACK: the broker's only way to say
    // "not you". The token may be what it refused; refresh it before the next
    // CONNECT, but not more often than NC_FORCE_REFRESH_MS (a broker that is
    // down would otherwise cost a refresh, and its writes, per reconnect).
    if (g_mq == MQ_CONNECTING && g_signed_in && (!g_forced_at || now_ms - g_forced_at >= NC_FORCE_REFRESH_MS)) {
        g_forced_at = now_ms;
        g_session.expires_at_ms = 0;
    }
    g_open = 0;
    g_mq = MQ_SHUT;
    g_n_unsubs = 0;
    g_mq_in_len = g_mq_in_off = 0;
    int wait = g_reconnect_ms ? g_reconnect_ms : NC_RETRY_FIRST_MS;
    g_reconnect_ms = wait * 2 > NC_RETRY_MAX_MS ? NC_RETRY_MAX_MS : wait * 2;
    return wait;
}

// A socket open and waiting to CONNECT whose token is due.
static int stream_wants_token(int64_t now_ms) {
    return g_open && g_mq == MQ_OPENED && g_signed_in &&
           (int64_t)g_session.expires_at_ms - NC_REFRESH_EARLY_MS <= now_ms;
}

// ---- what changed ------------------------------------------------------------------

static void events_begin(void) { g_n_events = 0; }

static NcEvent *event_add(uint8_t kind) {
    if (g_n_events == NC_EVENTS_MAX) return NULL;
    NcEvent *e = &g_events[g_n_events];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    g_event_at[g_n_events] = 0;
    g_event_base[g_n_events] = g_mq_in;
    g_n_events++;
    return e;
}

// The feed's end: own refetches it made moot are let go, and a session that
// changed during it is an event of its own.
static int events_end(void) {
    own_settle();
    if (g_session_told != g_session_gen) {
        g_session_told = g_session_gen;
        event_add(NC_EV_SESSION);
    }
    return g_n_events;
}

void nc_set_boards(int on) { g_boards = on != 0; }

const NcEvent *nc_event(int i) { return i >= 0 && i < g_n_events ? &g_events[i] : NULL; }

const uint8_t *nc_event_payload(int i) {
    if (i < 0 || i >= g_n_events || !g_events[i].payload_len) return NULL;
    return g_event_base[i] + g_event_at[i];
}

static uint32_t table_top(const gate *g) {
    uint32_t v = g->has_cast ? g->cast_v : 0;
    return g->has_answer && g->answer_v > v ? g->answer_v : v;
}

static uint32_t any_top(const gate *g) {
    uint32_t v = table_top(g);
    return g->has_view && g->view_v > v ? g->view_v : v;
}

// 1 when an answered view of `version` applies (see netclient.h "what changed").
static int answer_pass(gate *g, uint32_t version) {
    int any = g->has_view || g->has_cast || g->has_answer;
    if (any) {
        uint32_t top = any_top(g);
        if (version < top) return 0;
        // At the top itself only when nothing the table took is there already
        // (a dashboard push the table may not have shown is not).
        if (version == top && ((g->has_cast && g->cast_v == top) || (g->has_answer && g->answer_v == top)))
            return 0;
    }
    g->has_answer = 1;
    if (!any || version > g->answer_v) g->answer_v = version;
    return 1;
}

static int refetch_queue(const char *gid, int gid_len, int64_t not_before, uint32_t min_version);

// The game's id and version an envelope names (its roster trailer carries the id).
static int envelope_names(const uint8_t *p, int len, char *gid, uint8_t *gid_len, uint32_t *version) {
    EnvHeader h;
    if (!p || !env_header_read(p, len, &h) || h.trailer_at >= len) return 0;
    Roster r;   // on the stack: a static would cost every module that links this file
    char id[ROSTER_GAME_ID_MAX + 1];
    int n = 0, status = 0, passing = 0, used = 0;
    uint32_t ai = 0;
    if (roster_trailer_read(&r, id, &n, &status, &ai, &passing, p + h.trailer_at, len - h.trailer_at, &used) != ROSTER_OK ||
        n <= 0 || n > NW_GID_MAX)
        return 0;
    memcpy(gid, id, (size_t)n);
    *gid_len = (uint8_t)n;
    *version = h.version;
    return 1;
}

// ---- the stream: what the broker sends -------------------------------------------------

// What the user sits at, as an envelope says (its seat; an empty one: the seat is gone).
static void seat_learn(gate *g, const uint8_t *env, uint32_t len) {
    EnvHeader h;
    if (!g) return;
    if (!env || !len) { g->seat_known = 1; g->seated = 0; return; }
    if (!env_header_read(env, (int)len, &h)) return;
    g->seat_known = 1;
    g->seated = h.seat >= 0;
}

// ---- the quiet game ------------------------------------------------------------------
//
// The stream is at most once (MQTT QoS 0), and a lost cast shows only when a
// later one skips its version. The LAST cast of a quiet table has no later one:
// a player whose turn it now is would wait on a board that says it is not, and
// nobody else moves. So a live game subscription that hears nothing of its
// game for NC_QUIET_MS (plus a jitter, so a table's clients do not read in
// step) takes the game whole once, as its SUBACK's catch-up does, and is let
// go as caught up when something newer arrives first. A game whose terminal
// board was applied has nothing more to hear and is not read.

// Whether the `len`-byte envelope at env is a board whose game is over.
static int envelope_over(const uint8_t *env, uint32_t len) {
    EnvHeader h;
    return env && env_header_read(env, (int)len, &h) && h.state_len > 0 && env[h.state_at] == GAME_STATUS_GAME_OVER;
}

// Whether the cast at p (evwire.h: its header, then the as3 push) ends on a
// board whose game is over (the sequence's final board, whose first byte is
// the status, view.c state_put).
static int cast_over(const uint8_t *p, uint32_t len) {
    uint32_t v;
    int at = evwire_cast_header_get(p, (int)len, &v);
    const unsigned char *fin = NULL;
    int fin_len = 0;
    return at > 0 && evwire_read(p + at, (int)len - at, NULL, &fin, &fin_len, NULL, NULL) >= 0 && fin_len > 0 &&
           fin[0] == GAME_STATUS_GAME_OVER;
}

// The jitter of one wait, below NC_QUIET_JITTER_MS: from the clock, the user
// and the game, so clients that heard the same push at the same moment differ.
static int32_t quiet_jitter(const char *gid, int gid_len, int64_t now_ms) {
    uint32_t x = g_quiet_mix ^ (uint32_t)now_ms ^ (uint32_t)((uint64_t)now_ms >> 32) * 2654435761u;
    for (int i = 0; i < g_session.user_id_len; i++) x = (x ^ (uint8_t)g_session.user_id[i]) * 16777619u;
    for (int i = 0; i < gid_len; i++) x = (x ^ (uint8_t)gid[i]) * 16777619u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    g_quiet_mix = x;
    return (int32_t)(x % NC_QUIET_JITTER_MS);
}

static void quiet_arm(sub *s, int64_t now_ms) {
    s->quiet_at = now_ms + NC_QUIET_MS + quiet_jitter(s->gid, s->gid_len, now_ms);
}

// A live game subscription that waits on its quiet deadline.
static int quiet_watched(const sub *s) {
    return g_mq == MQ_LIVE && s->what == NW_SUB_GAME && s->state == SUB_LIVE && !s->over;
}

// Something of game `gid` was heard: its quiet wait starts over. `over`: a
// board was applied, and is (1) or is not (0) the game's terminal one; -1:
// nothing was applied (a stale cast or answer, a cast a read stands in for).
static void heard(const char *gid, int gid_len, int over, int64_t now_ms) {
    sub *s = sub_find(NW_SUB_GAME, gid, gid_len);
    if (!s) return;
    if (over >= 0) s->over = (uint8_t)over;
    quiet_arm(s, now_ms);
}

// Every quiet game whose wait is over is read, and waits again.
static void quiet_check(int64_t now_ms) {
    for (int i = 0; i < NC_SUBS_MAX; i++) {
        sub *s = &g_subs[i];
        if (!quiet_watched(s) || s->quiet_at > now_ms) continue;
        gate *g = gate_find(s->gid, s->gid_len, 0);
        refetch_queue(s->gid, s->gid_len, now_ms, g ? any_top(g) + 1 : 1);
        quiet_arm(s, now_ms);
    }
}

// Which of this user's topics a PUBLISH came on.
enum { T_NONE = 0, T_VIEWS, T_SEAT, T_WATCH, T_CHAT };

// The topic's family, and its game into gid/gid_len (none for T_VIEWS). T_NONE
// for any topic this client never subscribes to: another user's, a level it
// does not know.
static int topic_of(const uint8_t *t, int n, char *gid, uint8_t *gid_len) {
    *gid_len = 0;
    const char *u = g_session.user_id;
    int ul = g_session.user_id_len;
    const uint8_t *rest = NULL;
    int rn = 0, fam = T_NONE;
    if (n > 2 && t[0] == 's' && t[1] == '/') {
        fam = T_WATCH;
        rest = t + 2;
        rn = n - 2;
    } else if (ul && n > 2 + ul + 1 && t[0] == 'u' && t[1] == '/' && memcmp(t + 2, u, (size_t)ul) == 0 && t[2 + ul] == '/') {
        const uint8_t *k = t + 3 + ul;
        int kn = n - 3 - ul;
        if (kn == 1 && k[0] == 'v') return T_VIEWS;
        if (kn > 2 && k[1] == '/' && (k[0] == 'c' || k[0] == 'm')) {
            fam = k[0] == 'c' ? T_SEAT : T_CHAT;
            rest = k + 2;
            rn = kn - 2;
        }
    }
    if (fam == T_NONE || rn <= 0 || rn > NW_GID_MAX || !sub_args_ok(NW_SUB_GAME, (const char *)rest, rn)) return T_NONE;
    memcpy(gid, rest, (size_t)rn);
    *gid_len = (uint8_t)rn;
    return fam;
}

static void push_event(NcEvent *e, const NwFrame *f, int what) {
    e->what = (uint8_t)what;
    e->gid_len = f->gid_len;
    memcpy(e->gid, f->gid, f->gid_len);
    e->version = f->version;
    e->payload_len = f->blob.len;
    g_event_at[g_n_events - 1] = f->blob.len ? (uint32_t)(f->blob.p - g_mq_in) : 0;
}

// One pushed message, which came on a topic of family `fam` (its game agreeing).
static void take_push(const NwFrame *f, int fam, int64_t now_ms) {
    switch (f->kind) {
    case NW_WS_VIEW: {
        if (fam != T_VIEWS || !f->gid_len) { event_add(NC_EV_BAD); return; }
        gate *g = gate_find(f->gid, f->gid_len, 1);
        if (f->blob.len == 0) {
            // An empty view (the seat is gone) always applies and ends the game's gate.
            memset(g, 0, sizeof *g);
            g->used = 1;
            g->gid_len = f->gid_len;
            memcpy(g->gid, f->gid, f->gid_len);
            seat_learn(g, NULL, 0);
        } else {
            seat_learn(g, f->blob.p, f->blob.len);   // a view is only ever of a seat the user holds
            // A game held on its own stream is the casts' and the answers' to
            // move; a dashboard view of it would snap past what they animate.
            if (!g_boards && sub_find(NW_SUB_GAME, f->gid, f->gid_len)) return;
            if (g->has_view && f->version <= g->view_v) return;   // stale
            g->has_view = 1;
            g->view_v = f->version;
        }
        // Only a host of boards gets here with the game held (above).
        heard(f->gid, f->gid_len, f->blob.len ? envelope_over(f->blob.p, f->blob.len) : -1, now_ms);
        NcEvent *e = event_add(NC_EV_VIEW);
        if (e) push_event(e, f, NW_SUB_GAMES);
        return;
    }
    case NW_WS_CAST: {
        if ((fam != T_SEAT && fam != T_WATCH) || !f->gid_len) { event_add(NC_EV_BAD); return; }
        gate *g = gate_find(f->gid, f->gid_len, 1);
        heard(f->gid, f->gid_len, -1, now_ms);   // whatever becomes of it, the game's pushes arrive
        if (fam == T_SEAT) {
            // Only a seated user is sent a seat's casts.
            g->seat_known = 1;
            g->seated = 1;
        } else if (!g->seat_known) {
            // A spectators' cast for a game whose seat the book does not know
            // yet: a seated user hears the same version as their own cast, and
            // a spectators' board is not theirs. Take the game whole instead.
            refetch_queue(f->gid, f->gid_len, now_ms, f->version);
            return;
        } else if (g->seated) {
            return;   // the seat's own cast carries this move
        }
        if (g_boards) {
            // A host that shows boards never decodes a cast: the game moved, so
            // take it whole (caught_up lets the fetch go when its view at this
            // version is here by the time it would be sent).
            refetch_queue(f->gid, f->gid_len, now_ms, f->version);
            return;
        }
        int table = g->has_cast || g->has_answer;
        uint32_t top = table_top(g);
        if (table && f->version <= top) return;   // stale
        if (table && f->version > top + 1) {
            // A version was skipped: this cast would land on a board it does not
            // follow. Drop it and take the game whole instead.
            refetch_queue(f->gid, f->gid_len, now_ms, f->version);
            return;
        }
        g->has_cast = 1;
        g->cast_v = f->version;
        heard(f->gid, f->gid_len, cast_over(f->blob.p, f->blob.len), now_ms);
        NcEvent *e = event_add(NC_EV_CAST);
        if (e) push_event(e, f, NW_SUB_GAME);
        return;
    }
    case NW_WS_CHAT: {
        if (fam != T_CHAT) { event_add(NC_EV_BAD); return; }
        NcEvent *e = event_add(NC_EV_CHAT);
        if (!e) return;
        e->what = NW_SUB_CHAT;
        e->gid_len = f->gid_len;
        memcpy(e->gid, f->gid, f->gid_len);
        chat_copy(&e->chat, &f->chat);
        return;
    }
    default:
        event_add(NC_EV_BAD);   // not a push the server sends
        return;
    }
}

static void take_suback(const MqttPacket *p, int64_t now_ms) {
    sub *s = NULL;
    for (int i = 0; i < NC_SUBS_MAX && !s; i++)
        if (g_subs[i].state == SUB_SENT && g_subs[i].pid == p->packet_id) s = &g_subs[i];
    if (!s) return;   // a subscription since dropped or asked again
    int refused = 0;
    for (int k = 0; k < p->n_items; k++) refused |= p->items[k] == MQTT_SUBACK_FAILURE;
    NcEvent *e = event_add(NC_EV_NONE);
    if (!e) return;
    e->what = s->what;
    e->gid_len = s->gid_len;
    memcpy(e->gid, s->gid, s->gid_len);
    if (refused) {
        s->state = SUB_REFUSED;
        s->retry_at = now_ms + s->backoff;
        s->backoff = s->backoff * 2 > NC_RETRY_MAX_MS ? NC_RETRY_MAX_MS : s->backoff * 2;
        e->kind = NC_EV_REFUSED;
        e->why = ASK_WHY_FORBIDDEN;
        return;
    }
    s->state = SUB_LIVE;
    s->backoff = NC_RETRY_FIRST_MS;
    if (s->what != NW_SUB_GAME) return;
    // Live from here on: take the game whole once, so a commit between the
    // host's own read and this moment is not lost; anything newer than what is
    // held, arriving first, makes it moot.
    gate *g = gate_find(s->gid, s->gid_len, 0);
    e->kind = NC_EV_SEATED;
    e->seated = g && g->seat_known && g->seated;
    refetch_queue(s->gid, s->gid_len, now_ms, g ? any_top(g) + 1 : 1);
    quiet_arm(s, now_ms);
}

// One whole packet from the broker.
static void take_packet(const MqttPacket *p, int64_t now_ms) {
    switch (p->type) {
    case MQTT_CONNACK:
        if (g_mq != MQ_CONNECTING) { event_add(NC_EV_BAD); return; }
        if (p->return_code != MQTT_RC_ACCEPTED) return;   // the broker closes it; nc_stream_closed decides
        g_mq = MQ_LIVE;
        g_reconnect_ms = 0;   // the socket works: a later close starts the schedule over
        for (int i = 0; i < NC_SUBS_MAX; i++)
            if (g_subs[i].state != SUB_FREE) { g_subs[i].state = SUB_WANT; g_subs[i].retry_at = 0; }
        return;
    case MQTT_SUBACK:
        take_suback(p, now_ms);
        return;
    case MQTT_PUBLISH: {
        if (g_mq != MQ_LIVE) { event_add(NC_EV_BAD); return; }
        char gid[NW_GID_MAX];
        uint8_t gl = 0;
        int fam = topic_of(p->topic, p->topic_len, gid, &gl);
        NwFrame *f = &g_nwf;
        if (fam == T_NONE || nw_frame_decode(p->payload, p->payload_len, f) != NW_OK) { event_add(NC_EV_BAD); return; }
        // The message names its game: it must be the topic's.
        if (fam != T_VIEWS && !same_id(f->gid, f->gid_len, gid, gl)) { event_add(NC_EV_BAD); return; }
        take_push(f, fam, now_ms);
        return;
    }
    case MQTT_UNSUBACK:
    case MQTT_PINGRESP:
    case MQTT_PUBACK:
        return;
    default:
        event_add(NC_EV_BAD);
        return;
    }
}

int nc_stream_in(const uint8_t *frame, int len, int is_text, int64_t now_ms) {
    events_begin();
    // MQTT over WebSocket is binary (3.1.1, 6.0): a text frame is not the broker's.
    if (is_text || len < 0 || (len > 0 && !frame)) { event_add(NC_EV_BAD); return events_end(); }
    // What the last feed parsed (its events' payloads) is let go now.
    if (g_mq_in_off) {
        memmove(g_mq_in, g_mq_in + g_mq_in_off, (size_t)(g_mq_in_len - g_mq_in_off));
        g_mq_in_len -= g_mq_in_off;
        g_mq_in_off = 0;
    }
    if (len > NC_MQ_IN_MAX - g_mq_in_len) {
        g_mq_in_len = 0;   // past anything this client takes: the stream is lost; the host reconnects
        event_add(NC_EV_BAD);
        return events_end();
    }
    if (len) memcpy(g_mq_in + g_mq_in_len, frame, (size_t)len);
    g_mq_in_len += len;
    while (g_mq_in_off < g_mq_in_len && g_n_events < NC_EVENTS_MAX - 1) {
        MqttPacket p;
        int k = mqtt_parse(g_mq_in + g_mq_in_off, g_mq_in_len - g_mq_in_off, NC_MQ_IN_MAX, MQTT_FROM_SERVER, &p);
        if (k == 0) break;   // the rest comes in a later message
        if (k < 0) {
            g_mq_in_off = g_mq_in_len;   // not the protocol: nothing after it can be read either
            event_add(NC_EV_BAD);
            break;
        }
        g_mq_in_off += k;
        take_packet(&p, now_ms);
    }
    return events_end();
}

// ---- the stream: what this client sends -------------------------------------------------

static uint16_t next_pid(void) {
    if (++g_next_pid == 0) g_next_pid = 1;
    return g_next_pid;
}

// Appends n bytes at *at in out (cap): 0 when they do not fit.
static int put_bytes(char *out, int cap, int *at, const char *p, int n) {
    if (n < 0 || *at + n > cap) return 0;
    memcpy(out + *at, p, (size_t)n);
    *at += n;
    return 1;
}

// A subscription's topics (one, or a game's two) into t[0..), their text in
// buf. The count, or 0 when they do not fit (never: ids are short).
static int topics_of(int what, const char *gid, int gid_len, MqttTopic *t, char buf[2][NC_TOPIC_MAX]) {
    const char *u = g_session.user_id;
    int ul = g_session.user_id_len, at = 0;
    if (what == NW_SUB_GAMES) {
        int ok = put_bytes(buf[0], NC_TOPIC_MAX, &at, "u/", 2) && put_bytes(buf[0], NC_TOPIC_MAX, &at, u, ul) &&
                 put_bytes(buf[0], NC_TOPIC_MAX, &at, "/v", 2);
        t[0] = (MqttTopic){ buf[0], at, 0 };
        return ok;
    }
    int ok = put_bytes(buf[0], NC_TOPIC_MAX, &at, "u/", 2) && put_bytes(buf[0], NC_TOPIC_MAX, &at, u, ul) &&
             put_bytes(buf[0], NC_TOPIC_MAX, &at, what == NW_SUB_CHAT ? "/m/" : "/c/", 3) &&
             put_bytes(buf[0], NC_TOPIC_MAX, &at, gid, gid_len);
    if (!ok) return 0;
    t[0] = (MqttTopic){ buf[0], at, 0 };
    if (what != NW_SUB_GAME) return 1;
    at = 0;
    if (!put_bytes(buf[1], NC_TOPIC_MAX, &at, "s/", 2) || !put_bytes(buf[1], NC_TOPIC_MAX, &at, gid, gid_len)) return 0;
    t[1] = (MqttTopic){ buf[1], at, 0 };
    return 2;
}

// Due: wanted, refused and its wait over, or sent and never answered.
static int sub_due(const sub *s, int64_t now_ms) {
    return s->state == SUB_WANT || ((s->state == SUB_REFUSED || s->state == SUB_SENT) && s->retry_at <= now_ms);
}

// The client id of this connection: "<user>-<8 base-36 digits>", the digits
// from the clock and a counter, so two tabs of one user never share one (a
// broker drops the older connection of a client id that connects again).
static int client_id(char *out, int cap, int64_t now_ms) {
    static const char D[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    int at = 0;
    if (!put_bytes(out, cap, &at, g_session.user_id, g_session.user_id_len) || !put_bytes(out, cap, &at, "-", 1)) return -1;
    uint64_t x = (uint64_t)now_ms * 2654435761u ^ ((uint64_t)++g_conn_seq << 40);
    for (int i = 0; i < 8; i++) {
        if (at >= cap) return -1;
        out[at++] = D[x % 36];
        x /= 36;
    }
    return at;
}

// Everything the client has to say at this moment, as one WebSocket message
// (MQTT allows several packets in one): the CONNECT once the socket is open
// and the token fresh; once connected, the unsubscriptions, the subscriptions
// due (one SUBSCRIBE each, so each SUBACK names its own), and a PINGREQ when
// nothing has gone out for NC_PING_MS. What does not fit NC_OUT_MAX waits for
// the next call.
const NcOut *nc_stream_next(int64_t now_ms) {
    if (!g_open || !g_signed_in) return NULL;
    int at = 0, n;
    uint8_t *o = g_out.bytes;
    if (g_mq == MQ_OPENED) {
        if (stream_wants_token(now_ms)) return NULL;   // the pump refreshes it first
        char id[NW_ID_MAX + 16];
        int il = client_id(id, (int)sizeof id, now_ms);
        n = il < 0 ? -1
                   : mqtt_write_connect(o, NC_OUT_MAX, id, il, NC_KEEPALIVE_S, "token", 5, (const uint8_t *)g_session.access,
                                        g_session.access_len);
        if (n < 0) return NULL;
        g_mq = MQ_CONNECTING;
        g_last_out = now_ms;
        g_out.len = (uint16_t)n;
        return &g_out;
    }
    if (g_mq != MQ_LIVE) return NULL;
    MqttTopic t[2];
    char buf[2][NC_TOPIC_MAX];
    while (g_n_unsubs) {
        unsub *u = &g_unsubs[g_n_unsubs - 1];
        int k = topics_of(u->what, u->gid, u->gid_len, t, buf);
        n = k ? mqtt_write_unsubscribe(o + at, NC_OUT_MAX - at, next_pid(), t, k) : 0;
        if (n < 0) break;   // the next message
        at += n;
        g_n_unsubs--;
    }
    for (int i = 0; i < NC_SUBS_MAX; i++) {
        sub *s = &g_subs[i];
        if (!sub_due(s, now_ms)) continue;
        int k = topics_of(s->what, s->gid, s->gid_len, t, buf);
        uint16_t pid = next_pid();
        n = k ? mqtt_write_subscribe(o + at, NC_OUT_MAX - at, pid, t, k) : -1;
        if (n < 0) break;
        at += n;
        s->state = SUB_SENT;
        s->pid = pid;
        s->retry_at = now_ms + NC_ACK_WAIT_MS;
    }
    if (!at && now_ms - g_last_out >= NC_PING_MS) at = mqtt_write_bare(o, NC_OUT_MAX, MQTT_PINGREQ);
    if (at <= 0) return NULL;
    g_last_out = now_ms;
    g_out.len = (uint16_t)at;
    return &g_out;
}

int nc_stream_due_ms(int64_t now_ms) {
    if (!g_open || !g_signed_in) return -1;   // nobody to connect as: nothing to send
    if (g_mq == MQ_OPENED) return stream_wants_token(now_ms) ? -1 : 0;   // the pump's refresh wakes it
    if (g_mq != MQ_LIVE) return -1;                                       // the CONNACK wakes it
    if (g_n_unsubs) return 0;
    int64_t due = g_last_out + NC_PING_MS;
    for (int i = 0; i < NC_SUBS_MAX; i++) {
        if (g_subs[i].state == SUB_WANT) return 0;
        if ((g_subs[i].state == SUB_REFUSED || g_subs[i].state == SUB_SENT) && g_subs[i].retry_at < due)
            due = g_subs[i].retry_at;
        if (quiet_watched(&g_subs[i]) && g_subs[i].quiet_at < due) due = g_subs[i].quiet_at;   // quiet_check's
    }
    int64_t d = due - now_ms;
    return d < 0 ? 0 : d > NC_PING_MS ? NC_PING_MS : (int)d;
}

int nc_due_ms(int64_t now_ms) {
    int p = nc_pump_due_ms(now_ms), q = nc_stream_due_ms(now_ms);
    if (p < 0) return q;
    if (q < 0) return p;
    return p < q ? p : q;
}

const void *nc_row(int kind, const uint8_t *body, int len, int i) {
    switch (kind) {
    case NW_GAMES: return nc_game_row(body, len, i);
    case NW_CHATS: return nc_chat_row(body, len, i);
    case NW_BOTS: return nc_bot_row(body, len, i);
    case NW_BOARD: return nc_board_row(body, len, i);
    case NW_HISTORY: return nc_history_row(body, len, i);
    default: return NULL;
    }
}
