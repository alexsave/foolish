// netclient.h - the client of the backend's binary surface, for every host.
//
// A host keeps the sockets and nothing else: it asks this file for a request,
// sends it (URLSession, fetch), and hands back the status and the bytes; it
// opens the stream this file names, passes every frame in, sends every frame
// this file asks it to, and calls it on a timer; it persists the session as an
// opaque blob. What a request says, which token it carries and when that token
// is refreshed, what an answer means, which pushes are stale, when a refused
// subscription is retried and when to ping are all decided here, once, for the
// iOS app and the web alike. The bytes are netwire.h's.
//
// ONE CLIENT PER PROCESS. The state (the session, the subscriptions, the
// version gates) is one set of statics, as the kernel's client slot is: an app
// or a page talks to one backend as one user. Every answer is a struct the host
// reads through the readers structgen generates from this header; a pointer it
// is handed stays good until the next call into this file.
//
// SPANS. A blob in an answer (an envelope, a replay's moves) is an offset and a
// length into the body the host passed in, so nothing is copied and the host
// slices its own bytes. A stream event's payload is in this file's own buffer
// (a text frame is decoded into it), read through nc_event_payload.
//
// TIME. Every call that can act on time takes now_ms (epoch ms) from the host.
#ifndef CNITRO_NETCLIENT_H
#define CNITRO_NETCLIENT_H

#include "ask.h"
#include "netwire.h"
#include "table.h"   // TABLE_STATUS_*, what NcAction.status says
#include <stdint.h>

#define NC_GET  1
#define NC_POST 2

#define NC_PATH_MAX  160
#define NC_AUTH_MAX  (7 + NW_ACCESS_MAX)      // "Bearer " and the token
#define NC_BODY_MAX  1100                     // a chat message is the largest body
#define NC_TEXT_MAX  2800                     // a stream frame as base64: the AUTH frame is the largest
#define NC_FRAME_MAX 32768                    // a stream message, decoded
#define NC_SESSION_BLOB_MAX 2400              // what nc_session_save writes, at most

// Refresh an access token this long before it expires.
#define NC_REFRESH_EARLY_MS 60000
// The stream: a ping this often, a refused subscription retried after
// NC_RETRY_FIRST_MS doubling to NC_RETRY_MAX_MS, and a closed socket reopened
// on the same schedule (reset once a frame arrives).
#define NC_PING_MS        25000
#define NC_RETRY_FIRST_MS 500
#define NC_RETRY_MAX_MS   10000

#define NC_SUBS_MAX  16
#define NC_GATES_MAX 128

// What an event asks the host to do.
#define NC_EV_NONE    0   // nothing (a pong, a stale push, an accepted subscription)
#define NC_EV_VIEW    1   // apply the envelope in the payload; an empty one: the seat is gone
#define NC_EV_CAST    2   // apply the cast in the payload
#define NC_EV_CHAT    3   // append ev->chat to game ev->gid
#define NC_EV_REFUSED 4   // a subscription was refused (ev->why); it is retried by itself
#define NC_EV_SEATED  5   // a game subscription was accepted; ev->seated says which stream it hears
#define NC_EV_BAD     6   // a frame that is not this protocol (the host may log it)

typedef struct {
    uint8_t  method;                    // NC_GET, NC_POST
    uint8_t  is_refresh;                // the session's refresh, asked for first: send it, hand the
                                        // answer to nc_session_read, then build the request again
    uint8_t  path_len;    char path[NC_PATH_MAX];      // after the backend's base URL
    uint16_t auth_len;    char auth[NC_AUTH_MAX];      // the Authorization value, empty for none
    uint16_t body_len;    uint8_t body[NC_BODY_MAX];   // empty: send no body
} NcRequest;

typedef struct {
    uint8_t signed_in;
    uint8_t user_id_len;   char user_id[NW_ID_MAX];
    uint8_t username_len;  char username[NW_NAME_MAX];
} NcSessionInfo;

// Why the last answer read was not one (ask.h AskRefusal); why 0: it was.
typedef struct {
    uint8_t why;           // 0, or ASK_WHY_*
    int32_t code;          // NW_ACCOUNT_E_* for ASK_WHY_ACCOUNT, TABLE_E_* for ASK_WHY_REFUSED
    int32_t detail;
    int32_t status;        // the HTTP status
} NcError;

typedef struct {
    uint32_t version;
    uint32_t env_off, env_len;         // the envelope, in the body passed in
} NcGameRow;

typedef struct {
    uint8_t  id_len;        char id[NW_ID_MAX];
    uint8_t  user_id_len;   char user_id[NW_ID_MAX];
    uint64_t created_ms;
    uint8_t  is_system;
    uint16_t message_len;   char message[NW_CHAT_MAX];
} NcChatRow;

typedef struct {
    uint8_t id_len;         char id[NW_ID_MAX];
    uint8_t nickname_len;   char nickname[NW_NAME_MAX];
    uint8_t key_len;        char key[NW_KEY_MAX];
} NcBotRow;

typedef struct {
    uint8_t  is_bot;
    uint8_t  id_len;        char id[NW_ID_MAX];
    uint8_t  name_len;      char name[NW_NAME_MAX];
    uint32_t rating;
    uint32_t games;
} NcBoardRow;

typedef struct {
    uint8_t len;
    char    id[NW_ID_MAX];
} NcId;

typedef struct {
    uint8_t  id_len;        char id[NW_ID_MAX];
    uint64_t created_ms;
    uint8_t  n_players;     NcId players[MAX_PLAYERS];
    uint32_t moves_off, moves_len;     // in the body passed in
    uint32_t extras_off, extras_len;
} NcHistoryRow;

typedef struct {
    uint8_t  has_rating;
    uint32_t rating;
    uint16_t n;
} NcHistoryHead;

typedef struct {
    uint32_t moves_off, moves_len;
    uint32_t extras_off, extras_len;
} NcReplay;

typedef struct {
    uint8_t  status;       // TABLE_STATUS_*
    uint8_t  reject_code;
    uint32_t version;
} NcAction;

typedef struct {
    uint8_t  kind;         // NC_EV_*
    uint8_t  what;         // NW_SUB_* the push or refusal belongs to
    uint8_t  why;          // NC_EV_REFUSED: ASK_WHY_*
    uint8_t  seated;       // NC_EV_SEATED: 1 the caller's own seat's casts, 0 the spectators'
    uint8_t  gid_len;      char gid[NW_GID_MAX];
    uint32_t version;
    uint32_t payload_len;  // NC_EV_VIEW, NC_EV_CAST: nc_event_payload()
    NcChatRow chat;        // NC_EV_CHAT
} NcEvent;

typedef struct {
    uint16_t len;
    char     text[NC_TEXT_MAX];   // send as a TEXT frame
} NcOut;

// ---- the session ------------------------------------------------------------------

// Forget everything: the session, the subscriptions, the gates (tests, and a
// host's sign-out after the server has answered).
void nc_reset(void);
// The session as a blob to persist, and back. save: its length, 0 when signed
// out, NW_E_CAP. load: NW_OK, or NW_E_WIRE (the client is then signed out).
int nc_session_save(uint8_t *out, int cap);
int nc_session_load(const uint8_t *p, int len);
const NcSessionInfo *nc_session_info(void);
// An answer to signup, signin or refresh: NW_OK and the session is stored, or
// the refusal's why (nc_error says more). A refused refresh signs the client out.
int nc_session_read(int status, const uint8_t *body, int len);

// The last refusal read.
const NcError *nc_error(void);

// ---- requests ----------------------------------------------------------------------
//
// Each answers the request to send, or NULL: a value the wire cannot carry, or
// (for a request that needs the user) nobody signed in. A request that needs
// the user comes back as the refresh instead when the token is due
// (NcRequest.is_refresh). The name is the username as typed, upper-cased by
// the host: case mapping is Unicode, and the platforms already agree on it.
const NcRequest *nc_req_signup(const char *name, int name_len, const char *pw, int pw_len);
const NcRequest *nc_req_signin(const char *name, int name_len, const char *pw, int pw_len);
const NcRequest *nc_req_refresh(void);
const NcRequest *nc_req_signout(int global, int64_t now_ms);
const NcRequest *nc_req_password(const char *pw, int pw_len, int64_t now_ms);
const NcRequest *nc_req_delete_account(int64_t now_ms);
const NcRequest *nc_req_create(int64_t now_ms);
// A move: the kernel's action wire for game `gid`, composed against `intent`
// (the table request, table.h); intent < 0 sends none.
const NcRequest *nc_req_action(const char *gid, int gid_len, const uint8_t *wire, int wire_len, int64_t intent, int64_t now_ms);
// An ask: the body ask_encode wrote.
const NcRequest *nc_req_meta(const uint8_t *ask, int ask_len, int64_t now_ms);
const NcRequest *nc_req_games(int64_t now_ms);
const NcRequest *nc_req_game(const char *gid, int gid_len, int64_t now_ms);
const NcRequest *nc_req_chats(const char *gid, int gid_len, int64_t now_ms);
const NcRequest *nc_req_chat_send(const char *gid, int gid_len, const char *msg, int msg_len, int64_t now_ms);
const NcRequest *nc_req_bots(int64_t now_ms);
const NcRequest *nc_req_board(void);
const NcRequest *nc_req_history(int64_t now_ms);
// The newest replay of a game the caller played in.
const NcRequest *nc_req_replay(const char *gid, int gid_len, int64_t now_ms);
// The stream's path (GET, upgraded to a WebSocket by the host).
const NcRequest *nc_req_stream(void);

// Every builder above through one door, for a host that crosses by number (the
// wasm module): `a` and `b` are the strings or bytes each takes in order (the
// name and the password, the game id and the wire or the message, the ask, the
// new password), `n` the number (sign out: global; action: the intent).
#define NC_REQ_SIGNUP    1
#define NC_REQ_SIGNIN    2
#define NC_REQ_REFRESH   3
#define NC_REQ_SIGNOUT   4
#define NC_REQ_PASSWORD  5
#define NC_REQ_DELETE    6
#define NC_REQ_CREATE    7
#define NC_REQ_ACTION    8
#define NC_REQ_META      9
#define NC_REQ_GAMES     10
#define NC_REQ_GAME      11
#define NC_REQ_CHATS     12
#define NC_REQ_CHAT_SEND 13
#define NC_REQ_BOTS      14
#define NC_REQ_BOARD     15
#define NC_REQ_HISTORY   16
#define NC_REQ_REPLAY    17
#define NC_REQ_STREAM    18
const NcRequest *nc_req(int which, const char *a, int a_len, const char *b, int b_len, int64_t n, int64_t now_ms);

// ---- answers -----------------------------------------------------------------------
//
// Each takes the HTTP status and the body as received. On a refusal the answer
// is NULL (or the why, for the ints) and nc_error says why.

// A request answered with no body of its own (signout, password, delete,
// chat send) or with the caller's envelope (create, meta): 0 when it was
// accepted, else the why. A host signs itself out with nc_reset after a signout
// or a delete, whatever the server answered.
int nc_status_read(int status, const uint8_t *body, int len);
const NcAction *nc_action_read(int status, const uint8_t *body, int len);
const NcGameRow *nc_view_read(int status, const uint8_t *body, int len);
const NcReplay *nc_replay_read(int status, const uint8_t *body, int len);
// A list (NW_GAMES, NW_CHATS, NW_BOTS, NW_BOARD, NW_HISTORY): its row count,
// or -why. Then each row by index, from the same body.
int nc_list_read(int status, const uint8_t *body, int len, int kind);
const NcGameRow *nc_game_row(const uint8_t *body, int len, int i);
const NcChatRow *nc_chat_row(const uint8_t *body, int len, int i);
const NcBotRow *nc_bot_row(const uint8_t *body, int len, int i);
const NcBoardRow *nc_board_row(const uint8_t *body, int len, int i);
const NcHistoryRow *nc_history_row(const uint8_t *body, int len, int i);
const NcHistoryHead *nc_history_head(const uint8_t *body, int len);
// Row i of a list of `kind` through one door: the struct above for that kind.
const void *nc_row(int kind, const uint8_t *body, int len, int i);

// ---- the stream --------------------------------------------------------------------

// The subscriptions the host wants; each is (re)sent whenever the socket opens.
// A second nc_stream_sub of the same (what, gid) asks again (a spectator who
// took a seat hears their own seat's casts from then on). NW_OK, or NW_E_CAP /
// NW_E_WIRE.
int nc_stream_sub(int what, const char *gid, int gid_len);
int nc_stream_unsub(int what, const char *gid, int gid_len);
// The socket opened; it closed (the answer is how long to wait before
// reopening it, in ms).
void nc_stream_opened(int64_t now_ms);
int nc_stream_closed(int64_t now_ms);
// One frame as it arrived (is_text: a text frame, base64 of the message).
const NcEvent *nc_stream_in(const uint8_t *frame, int len, int is_text, int64_t now_ms);
const uint8_t *nc_event_payload(void);
// The next frame to send, or NULL for none now; call until NULL. Then
// nc_stream_due_ms says how long until there is one (a retry, a ping), -1 for
// none while the socket is shut.
const NcOut *nc_stream_next(int64_t now_ms);
int nc_stream_due_ms(int64_t now_ms);
// Forget the newest versions seen for a game, so the next push or fetched view
// of it applies whatever its version (the host's resync).
void nc_gate_reset(const char *gid, int gid_len);

#endif
