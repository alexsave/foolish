// netclient.h - the client of the backend's binary surface, for every host.
//
// A host keeps the sockets and nothing else. It says what the user wants (an
// NcIntent: sign in, open the dashboard, play this move) and gets a ticket; it
// runs ONE transport loop - send every request nc_next_request names, hand each
// answer to nc_on_response, wake when nc_pump_due_ms says - and resolves a
// ticket when an NcDone names it; it opens the stream this file names, passes
// every frame in, sends every frame this file asks it to, and calls it on a
// timer; it persists the session as an opaque blob. What a request says, which token it carries and when that token
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
#define NC_GATES_MAX 64

// What an event asks the host to do.
#define NC_EV_NONE    0   // nothing (a pong, a stale push, an accepted subscription)
#define NC_EV_VIEW    1   // apply the envelope in the payload; an empty one: the seat is gone
#define NC_EV_CAST    2   // apply the cast in the payload
#define NC_EV_CHAT    3   // append ev->chat to game ev->gid
#define NC_EV_REFUSED 4   // a subscription was refused (ev->why); it is retried by itself
#define NC_EV_SEATED  5   // a game subscription was accepted; ev->seated says which stream it hears
#define NC_EV_BAD     6   // a frame that is not this protocol (the host may log it)
#define NC_EV_SESSION 7   // the session changed: keep nc_session_save's blob, show nc_session_info
#define NC_EVENTS_MAX 4   // what one feed can change at most

typedef struct {
    uint32_t id;                        // nc_next_request's: hand the answer back with it
    uint8_t  method;                    // NC_GET, NC_POST
    uint8_t  is_refresh;                // the builders below only: the session's refresh, asked for
                                        // first; send it, hand the answer to nc_session_read, then
                                        // build the request again (the pump does all of this itself)
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
    uint8_t  what;         // NW_SUB_* the push or refusal belongs to; a VIEW of NW_SUB_GAME is
                           // this client's own refetch of the game (the table takes it whole)
    uint8_t  why;          // NC_EV_REFUSED: ASK_WHY_*
    uint8_t  seated;       // NC_EV_SEATED: 1 the caller's own seat's casts, 0 the spectators'
    uint8_t  gid_len;      char gid[NW_GID_MAX];
    uint32_t version;
    uint32_t payload_len;  // NC_EV_VIEW, NC_EV_CAST: nc_event_payload(i)
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

// ---- answers -----------------------------------------------------------------------
//
// Each takes the HTTP status and the body as received. On a refusal the answer
// is NULL (or the why, for the ints) and nc_error says why.

// A read the server was too busy to answer (503, ASK_WHY_BUSY: its store was
// throttled): how long to wait before sending it again, or 0 when the answer
// stands. `method` is the request's (NcRequest.method), `attempt` how many
// times this request was sent again already (0 after the first send). Only a
// GET is sent again (a POST may have been applied before the throttle), at most
// NC_BUSY_TRIES times, after NC_BUSY_FIRST_MS doubling. The host waits, builds
// the request again and sends it; the answer it finally reads says the rest.
#define NC_BUSY_TRIES    3
#define NC_BUSY_FIRST_MS 250
int nc_retry_ms(int method, int attempt, int status, const uint8_t *body, int len);

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

// ---- the pump ----------------------------------------------------------------------
//
// What the user wants, as the host says it. Every endpoint, token, refresh,
// retry and reading of an answer is this file's: the host submits an intent,
// sends what nc_next_request names, and hands every answer to nc_on_response.
//
// THE LOOP, whole:
//   ticket = nc_do(&intent, now)                 // 0: refused at once (nc_error)
//   while ((r = nc_next_request(now))) send r, then later:
//       done = nc_on_response(r->id, status, body, now)   // status 0: it never arrived
//       if (done) resolve done->ticket with (done, body)
//   while ((done = nc_next_done())) resolve done->ticket   // finished with no answer of its own
//   wake again in nc_pump_due_ms(now) ms (-1: nothing waits)
//
// A request that needs the user waits for a refresh this file sends first when
// the token is within NC_REFRESH_EARLY_MS of expiring, and is sent again after
// one when it is answered 401. A read that never arrives (status 0) is sent
// again after NC_RETRY_FIRST_MS doubling to NC_RETRY_MAX_MS, NC_PUMP_TRIES
// times in all; a write is not (it may have landed). Signing out, deleting the
// account, or a refused refresh ends the session here and finishes every
// ticket still waiting for the user with ASK_WHY_AUTH.
#define NC_DO_SIGN_UP        1   // name (as typed: upper-cased by the host), password
#define NC_DO_SIGN_IN        2   // name, password
#define NC_DO_SIGN_OUT       3   // flag: 1 every session of the user, 0 this one
#define NC_DO_PASSWORD       4   // password
#define NC_DO_DELETE_ACCOUNT 5
#define NC_DO_CREATE         6   // -> the creator's envelope (a_off, a_len)
#define NC_DO_PLAY           7   // gid, bytes: the kernel's action wire, intent: the version (-1 none)
#define NC_DO_ASK            8   // bytes: the kernel's ask -> the asker's envelope
#define NC_DO_GAMES          9   // -> a list: every view of the user
#define NC_DO_OPEN_GAME      10  // gid -> the view (version, a_off, a_len); NOT_FOUND for none
#define NC_DO_CHAT_HISTORY   11  // gid -> a list
#define NC_DO_SEND_CHAT      12  // gid, bytes: the message
#define NC_DO_BOTS           13  // -> a list
#define NC_DO_LEADERBOARD    14  // -> a list
#define NC_DO_HISTORY        15  // -> a list, with the user's rating
#define NC_DO_REPLAY         16  // gid -> moves (a_off, a_len) and extras (b_off, b_len)

#define NC_PUMP_MAX   8          // tickets waiting at once
#define NC_PUMP_TRIES 4

typedef struct {
    uint8_t  kind;                                     // NC_DO_*
    uint8_t  flag;                                     // SIGN_OUT
    uint8_t  gid_len;       char gid[NW_GID_MAX];
    uint8_t  name_len;      char name[NW_NAME_MAX];
    uint8_t  password_len;  char password[NW_PASSWORD_MAX];
    uint16_t bytes_len;     uint8_t bytes[NC_BODY_MAX];
    int64_t  intent;                                   // PLAY
} NcIntent;

// A ticket finished. Spans are into the body handed to the nc_on_response that
// returned this (none for nc_next_done's).
typedef struct {
    uint32_t ticket;
    uint8_t  kind;          // the intent's NC_DO_*
    uint8_t  why;           // 0: done; else ASK_WHY_* (code, detail, status as NcError's)
    int32_t  code, detail, status;
    uint8_t  action_status; // PLAY: TABLE_STATUS_*
    uint8_t  reject_code;   // PLAY
    uint32_t version;       // PLAY, OPEN_GAME
    uint32_t a_off, a_len;  // an envelope (CREATE, ASK, OPEN_GAME), a replay's moves
    uint32_t b_off, b_len;  // a replay's extras
    uint8_t  stale;         // OPEN_GAME, CREATE, ASK: answered, but older than what was applied (no envelope)
    uint8_t  gid_len;       char gid[NW_GID_MAX];   // the game it was about (CREATE: the new one)
    int32_t  n_rows;        // a list: read row i with nc_row(list_kind, body, len, i)
    uint8_t  list_kind;     // NW_GAMES, NW_CHATS, NW_BOTS, NW_BOARD, NW_HISTORY
    uint8_t  has_rating;    // HISTORY
    uint32_t rating;
} NcDone;

uint32_t nc_do(const NcIntent *in, int64_t now_ms);
const NcRequest *nc_next_request(int64_t now_ms);
const NcDone *nc_on_response(uint32_t id, int status, const uint8_t *body, int len, int64_t now_ms);
const NcDone *nc_next_done(void);
int nc_pump_due_ms(int64_t now_ms);

// ---- the stream --------------------------------------------------------------------

// The subscriptions the host holds; each is (re)sent whenever the socket opens.
// They are counted: the last nc_stream_unsub of a (what, gid) lets it go. Which
// stream a game subscription hears (the seat's or the spectators') is the
// server's to say, and this file asks again when a join or a leave changes it.
// NW_OK, or NW_E_CAP / NW_E_WIRE.
int nc_stream_sub(int what, const char *gid, int gid_len);
int nc_stream_unsub(int what, const char *gid, int gid_len);
// The socket opened; it closed (the answer is how long to wait before
// reopening it, in ms).
void nc_stream_opened(int64_t now_ms);
int nc_stream_closed(int64_t now_ms);
// One frame as it arrived (is_text: a text frame, base64 of the message): how
// many events it made, read with nc_event.
int nc_stream_in(const uint8_t *frame, int len, int is_text, int64_t now_ms);
// The next frame to send, or NULL for none now; call until NULL. Then
// nc_stream_due_ms says how long until there is one (a retry, a ping), -1 for
// none while the socket is shut.
const NcOut *nc_stream_next(int64_t now_ms);
int nc_stream_due_ms(int64_t now_ms);
// Forget the newest versions seen for a game, so the next push or fetched view
// of it applies whatever its version.
void nc_gate_reset(const char *gid, int gid_len);

// ---- what changed ------------------------------------------------------------------
//
// ONE STATE, TWO FEEDS. The stream (nc_stream_in) and the answers
// (nc_on_response) feed the same book of games, so every version rule is here:
//   - a cast applies when it is newer than the table's (casts and answered
//     views); one that skips a version is dropped and the game refetched;
//   - a view pushed for the dashboard applies when newer than the last one,
//     and is not handed out at all for a game held on its own stream (casts
//     animate that one; an empty view, the seat gone, always is);
//   - an answered view (OPEN_GAME, CREATE, ASK, and this file's own refetches)
//     applies unless something newer was applied, or an answer or cast at its
//     very version was (a ticket's NcDone says stale, with no envelope);
//   - a move refused over a newer version refetches the game when no push has
//     caught up NC_RECONCILE_MS later;
//   - a game subscription accepted again (the socket reopened, a seat changed)
//     or accepted after a refusal refetches the game;
//   - a join, a leave or a create moves the game's subscription itself.
// After each feed the host reads the events it made, nc_event(0) up to the
// count, as often as it likes until the next feed; then it sends what
// nc_next_request and nc_stream_next have for it.
#define NC_RECONCILE_MS 1000
const NcEvent *nc_event(int i);
const uint8_t *nc_event_payload(int i);
// When to call again (nc_next_request, nc_next_done, nc_stream_next): the
// sooner of nc_pump_due_ms and nc_stream_due_ms, -1 when neither waits.
int nc_due_ms(int64_t now_ms);

#endif
