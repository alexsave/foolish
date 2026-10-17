// netwire.h - the backend's binary surface: every request body, response body
// and push-stream frame between a client and c_base that is not already one of
// the kernel's own formats (the move's table request, the ask, the envelope,
// the cast, the refusal).
//
// ONE CODEC FOR BOTH ENDS. The server (c_base) writes what a client reads and
// reads what a client writes through these same functions, and the client
// library (netclient.h) is the only reader a host reaches, so the layouts below
// are stated here and nowhere else. c_base/DESIGN.md "Binary surface" is the
// design; this is the byte-level contract.
//
// THE LAYOUT. Little-endian. Every body and frame starts [NW_V1][kind]. A str8
// is a u8 length then UTF-8, a str16 a u16 length, a blob a u32 length then
// bytes; a time is u48 epoch ms (le_bytes.h).
//
//   requests (client to server, HTTP bodies)
//     NW_SIGNUP, NW_SIGNIN   str8 name (upper-cased by the host), str8 password
//     NW_REFRESH             str8 refresh token
//     NW_SIGNOUT             u8 scope (NW_SCOPE_*)
//     NW_PASSWORD            str8 password
//     NW_CHAT_SEND           str16 message
//   responses (server to client, HTTP bodies)
//     NW_SESSION   str16 access token, str8 refresh token, u48 expires_at,
//                  str8 user id, str8 username
//     NW_VIEW      u32 version, blob envelope
//     NW_REPLAY    blob moves, blob extras
//     NW_GAMES     u16 n, n x { u32 version, blob envelope }
//     NW_CHATS     u16 n, n x chat
//     NW_BOTS      u16 n, n x { str8 id, str8 nickname, str8 strategy key }
//     NW_BOARD     u16 n, n x { u8 is_bot, str8 id, str8 name, u32 rating, u32 games }
//     NW_HISTORY   u8 has_rating, u32 rating, u16 n,
//                  n x { str8 id, u48 created, u8 n_players, n_players x str8 id,
//                        blob moves, blob extras }
//     chat         str8 id, str8 user id, u48 created, u8 is_system, str16 message
//   stream, client to server
//     NW_WS_AUTH   str16 access token
//     NW_WS_SUB    u16 ref, u8 what (NW_SUB_*), str8 game id, u32 since
//     NW_WS_UNSUB  u8 what, str8 game id
//     NW_WS_PING   nothing
//     NW_WS_SUB_AS str16 access token, then NW_WS_SUB's fields: a subscription
//                  that says who asks, so it never waits on an AUTH frame
//                  (API Gateway runs a connection's frames in no order)
//     NW_WS_BATCH  (either direction) blob: 1..NW_BATCH_MAX entries, each a u32
//                  length then one whole frame of any other kind, read in
//                  order. A client's subscriptions of one moment ride in one
//                  (behind API Gateway: one invocation and one stored write
//                  instead of one each, all at once); the server answers a
//                  batch's replies as one batch, and only a client that sent a
//                  batch is ever sent one.
//   stream, server to client
//     NW_WS_ACK    u16 ref, u8 why (0, or an ASK_WHY_*), u8 seated
//     NW_WS_VIEW   str8 game id, u32 version, blob envelope (empty: the seat is gone)
//     NW_WS_CAST   str8 game id, u32 version, blob cast
//     NW_WS_CHAT   str8 game id, chat
//     NW_WS_PONG   nothing
//     NW_WS_CATCHUP str8 game id, u32 version, blob envelope: a NW_WS_VIEW sent
//                  right after the ACK of a game subscription made with
//                  NW_WS_SUB_AS, the caller's view of what it missed. Its own
//                  kind so a client tells it from a dashboard push.
//
// A refusal of an HTTP request is the kernel's AskRefusal (ask.h), not a body
// of this file. A body is read whole: a byte short or a byte over is NW_E_WIRE.
//
// FRAMES AS TEXT. API Gateway's WebSocket API refuses binary frames from a
// client, so a client sends every stream message as a text frame carrying its
// base64 (RFC 4648, padded); nw_text_encode/nw_text_decode are both ends of it.
#ifndef CNITRO_NETWIRE_H
#define CNITRO_NETWIRE_H

#include "game.h"
#include "roster.h"
#include <stdint.h>

#define NW_V1 1

// Kinds. Append-only: they are on the wire.
#define NW_SIGNUP    1
#define NW_SIGNIN    2
#define NW_REFRESH   3
#define NW_SIGNOUT   4
#define NW_PASSWORD  5
#define NW_CHAT_SEND 6
#define NW_SESSION   20
#define NW_VIEW      21
#define NW_REPLAY    22
#define NW_GAMES     23
#define NW_CHATS     24
#define NW_BOTS      25
#define NW_BOARD     26
#define NW_HISTORY   27
#define NW_WS_AUTH   40
#define NW_WS_SUB    41
#define NW_WS_UNSUB  42
#define NW_WS_PING   43
#define NW_WS_SUB_AS 44
#define NW_WS_ACK    50
#define NW_WS_VIEW   51
#define NW_WS_CAST   52
#define NW_WS_CHAT   53
#define NW_WS_PONG   54
#define NW_WS_CATCHUP 55
#define NW_WS_BATCH  56

#define NW_BATCH_MAX 16   // frames in one NW_WS_BATCH

#define NW_SCOPE_LOCAL  0   // sign out this session
#define NW_SCOPE_GLOBAL 1   // sign out every session of the user

#define NW_SUB_GAMES 1      // the caller's views of every game they sit at
#define NW_SUB_GAME  2      // one game's casts: the caller's seat's, or the spectators'
#define NW_SUB_CHAT  3      // one game's chat (members only)

// Account refusals (ASK_WHY_ACCOUNT's code). Append-only.
#define NW_ACCOUNT_E_BAD_LOGIN     1   // wrong name or password
#define NW_ACCOUNT_E_EXISTS        2   // the name is taken
#define NW_ACCOUNT_E_WEAK_PASSWORD 3   // under 6 bytes
#define NW_ACCOUNT_E_LONG_PASSWORD 4   // over 72 bytes
#define NW_ACCOUNT_E_BAD_NAME      5   // empty, too long, or with '%'
#define NW_ACCOUNT_E_BAD_REFRESH   6   // the refresh token is unknown, or its user is gone
#define NW_ACCOUNT_E_REFRESH_USED  7   // a used or revoked refresh token: the session is over
#define NW_ACCOUNT_E_SAME_PASSWORD 8   // the new password is the old one

#define NW_OK      0
#define NW_E_WIRE  (-401)   // not a body or frame of this file, or a field past its bound
#define NW_E_CAP   (-402)   // an output buffer is too small

#define NW_NAME_MAX     ROSTER_NAME_MAX      // a username or a bot nickname, bytes
#define NW_PASSWORD_MAX 72                   // bcrypt's limit; longer is refused, never cut
#define NW_ACCESS_MAX   2048                 // an access token (a JWT), bytes
#define NW_REFRESH_MAX  64                   // a refresh token (base64url of 32 bytes is 43)
#define NW_ID_MAX       ROSTER_ID_MAX        // a user, bot, message or replay id
#define NW_GID_MAX      ROSTER_GAME_ID_MAX
#define NW_KEY_MAX      ROSTER_BRAIN_MAX     // a bot's strategy key
#define NW_CHAT_MAX     1000                 // a chat message, bytes (the web's own bound)

// A blob: bytes that stay where they are. Encoding reads p; decoding points p
// into the body it read.
typedef struct {
    const uint8_t *p;
    uint32_t       len;
} NwBlob;

typedef struct {
    uint8_t  kind;                                    // NW_SIGNUP .. NW_CHAT_SEND
    uint8_t  scope;                                   // NW_SIGNOUT
    uint8_t  name_len;      char name[NW_NAME_MAX];
    uint8_t  password_len;  char password[NW_PASSWORD_MAX];
    uint8_t  refresh_len;   char refresh[NW_REFRESH_MAX];
    uint16_t message_len;   char message[NW_CHAT_MAX];
} NwRequest;

typedef struct {
    uint16_t access_len;    char access[NW_ACCESS_MAX];
    uint8_t  refresh_len;   char refresh[NW_REFRESH_MAX];
    uint64_t expires_at_ms;
    uint8_t  user_id_len;   char user_id[NW_ID_MAX];
    uint8_t  username_len;  char username[NW_NAME_MAX];
} NwSession;

typedef struct {
    uint8_t  id_len;        char id[NW_ID_MAX];
    uint8_t  user_id_len;   char user_id[NW_ID_MAX];
    uint64_t created_ms;
    uint8_t  is_system;
    uint16_t message_len;   char message[NW_CHAT_MAX];
} NwChat;

typedef struct {
    uint8_t  id_len;        char id[NW_ID_MAX];
    uint8_t  nickname_len;  char nickname[NW_NAME_MAX];
    uint8_t  key_len;       char key[NW_KEY_MAX];
} NwBot;

typedef struct {
    uint8_t  is_bot;
    uint8_t  id_len;        char id[NW_ID_MAX];
    uint8_t  name_len;      char name[NW_NAME_MAX];
    uint32_t rating;
    uint32_t games;
} NwBoardRow;

typedef struct {
    uint8_t  id_len;        char id[NW_ID_MAX];
    uint64_t created_ms;
    uint8_t  n_players;
    uint8_t  player_len[MAX_PLAYERS];
    char     player[MAX_PLAYERS][NW_ID_MAX];
    NwBlob   moves, extras;
} NwHistoryRow;

// One stream frame, either direction; only the fields its kind carries are
// written or read (the rest read zero).
typedef struct {
    uint8_t  kind;                                    // NW_WS_*
    uint16_t ref;                                     // SUB, SUB_AS, ACK
    uint8_t  what;                                    // SUB, SUB_AS, UNSUB
    uint8_t  why;                                     // ACK
    uint8_t  seated;                                  // ACK
    uint8_t  gid_len;       char gid[NW_GID_MAX];     // SUB, SUB_AS, UNSUB, VIEW, CAST, CHAT
    uint32_t version;                                 // SUB, SUB_AS (since), VIEW, CAST
    uint16_t token_len;     char token[NW_ACCESS_MAX];// AUTH, SUB_AS
    NwBlob   blob;                                    // VIEW (envelope), CAST (cast), BATCH (its entries)
    NwChat   chat;                                    // CHAT
} NwFrame;

// ---- requests and single answers ---------------------------------------------------

// Each encoder returns the body's length, NW_E_WIRE for a value that is not one
// (an unknown kind, a field past its bound), or NW_E_CAP. Each decoder zeroes
// `out` first and returns NW_OK or NW_E_WIRE.
int nw_request_encode(const NwRequest *r, uint8_t *out, int cap);
int nw_request_decode(const uint8_t *p, int len, NwRequest *out);
int nw_session_encode(const NwSession *s, uint8_t *out, int cap);
int nw_session_decode(const uint8_t *p, int len, NwSession *out);
int nw_view_encode(uint32_t version, NwBlob envelope, uint8_t *out, int cap);
int nw_view_decode(const uint8_t *p, int len, uint32_t *version, NwBlob *envelope);
int nw_replay_encode(NwBlob moves, NwBlob extras, uint8_t *out, int cap);
int nw_replay_decode(const uint8_t *p, int len, NwBlob *moves, NwBlob *extras);

// ---- lists ------------------------------------------------------------------------
//
// A list is written as its head, then each row appended. The count is not known
// until the rows are, so the writer reserves the head's bytes, appends rows and
// writes the head last at the start. NW_HISTORY's head carries the rating.

#define NW_LIST_HEAD_BYTES    4    // [NW_V1][kind][u16 n]
#define NW_HISTORY_HEAD_BYTES 9    // [NW_V1][NW_HISTORY][u8 has][u32 rating][u16 n]
int nw_list_head(uint8_t kind, int n, uint8_t *out, int cap);
int nw_history_head(int has_rating, uint32_t rating, int n, uint8_t *out, int cap);
int nw_put_game(uint32_t version, NwBlob envelope, uint8_t *out, int cap);
int nw_put_chat(const NwChat *c, uint8_t *out, int cap);
int nw_put_bot(const NwBot *b, uint8_t *out, int cap);
int nw_put_board(const NwBoardRow *r, uint8_t *out, int cap);
int nw_put_history(const NwHistoryRow *r, uint8_t *out, int cap);

// Opens a list body of `kind`: the row count, or NW_E_WIRE (another kind, a
// short head). Then row i (0-based, rows are read in order from the start, and
// the whole body is checked to hold exactly n rows): NW_OK or NW_E_WIRE.
int nw_list_open(const uint8_t *p, int len, uint8_t kind);
int nw_history_rating(const uint8_t *p, int len, int *has_rating, uint32_t *rating);
int nw_get_game(const uint8_t *p, int len, int i, uint32_t *version, NwBlob *envelope);
int nw_get_chat(const uint8_t *p, int len, int i, NwChat *out);
int nw_get_bot(const uint8_t *p, int len, int i, NwBot *out);
int nw_get_board(const uint8_t *p, int len, int i, NwBoardRow *out);
int nw_get_history(const uint8_t *p, int len, int i, NwHistoryRow *out);

// ---- the stream -------------------------------------------------------------------

int nw_frame_encode(const NwFrame *f, uint8_t *out, int cap);
int nw_frame_decode(const uint8_t *p, int len, NwFrame *out);

// A batch's entries (NwFrame.blob of an NW_WS_BATCH). nw_batch_add appends one
// encoded frame to `out` as an entry: its bytes written, NW_E_WIRE for a frame
// that cannot be an entry (shorter than a head, another batch), or NW_E_CAP.
// nw_batch_next reads the entry at *pos and moves *pos past it: 1 with *one
// pointing into `entries`, 0 at the end, NW_E_WIRE for a malformed list (the
// frame decoder has already refused those for a decoded batch).
int nw_batch_add(const uint8_t *frame, int len, uint8_t *out, int cap);
int nw_batch_next(NwBlob entries, uint32_t *pos, NwBlob *one);

// Base64 (RFC 4648, padded) both ways: a text frame's form of a message. The
// encoder answers the text's length (no NUL), the decoder the bytes'.
int nw_text_encode(const uint8_t *p, int len, char *out, int cap);
int nw_text_decode(const char *p, int len, uint8_t *out, int cap);

#endif
