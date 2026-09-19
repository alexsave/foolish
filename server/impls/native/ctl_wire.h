// ctl_wire.h - the native server's CONTROL-PLANE wire, in packed bytes.
//
// The game STATE has always been packed bytes (view.c state_put) and a MOVE
// has always been a packed awire frame (awire.c). Only the control plane -
// sign in, the lobby verbs, and every server answer that was not a view - was
// still JSON, scraped on both sides by a hand-rolled `json_str` that was "not
// a parser" by its own admission: one in foolish_server.c, a second copy in
// foolish_hammer.c, and a third pair of `grep -o` one-liners in the shell
// drivers. Three scrapers of one format is three chances to disagree about it.
//
// This is that control plane as ONE codec both sides link, in the same shape
// the rest of the repo uses (c/src/awire.c, c/src/msg_wire.c, sdk/ts/wire/):
// explicit little-endian bytes, a length-prefixed shape, documented offsets,
// and encode/decode that run the same checks so a host can never emit a frame
// it would itself refuse.
//
// FRAME
//   [0]      version   (CTL_WIRE_VERSION)
//   [1]      kind      (CTL_* below)
//   [2..3]   payload length, uint16 LE
//   [4..]    payload, kind-specific
//
// A frame is refused WHOLE - never clamped onto a shorter string, never
// half-read - on a wrong version, an unknown kind, a payload length that does
// not match the bytes actually present, a field that would run past the
// payload, or a string longer than the caller's buffer. That is the same
// posture awire_decode takes against a hostile card byte: a malformed wire is
// malformed, not "the nearest legal thing".
//
// STRINGS are length-prefixed, never NUL-terminated on the wire:
//   [u8 len][len bytes]
// They are 8-bit-clean going out and going in; the decoder NUL-terminates into
// the caller's buffer, so nothing downstream has to know the difference.
//
// ENDIANNESS is a property of the wire, not of the host: every multi-byte
// field is written and read byte at a time, little-endian, exactly as
// msg_wire.c's rd16/wr16 do. Every target today is little-endian; that is
// precisely the kind of accident that rots.
#ifndef FOOLISH_CTL_WIRE_H
#define FOOLISH_CTL_WIRE_H

#include <stdbool.h>
#include <stdint.h>

#define CTL_WIRE_VERSION 1

// The head is fixed-width, so the payload always starts here.
#define CTL_HEAD_LEN 4

// No control frame this server speaks is anywhere near this big (the largest
// is a session: 4 + 13 + 24 + 96 = 137 bytes), so one cap covers every caller's
// scratch buffer. Same "size it once, with real margin" discipline
// VIEW_CACHE_CAP takes in registry.h.
#define CTL_FRAME_MAX 256

// Field caps, mirroring the server's own field widths (User.user_id is
// ID_LEN+1, User.username is 24, a signed token is ~70 base64url chars in a
// 96-byte field). A decode that would not fit is refused, not truncated.
#define CTL_ID_CAP        16
#define CTL_NAME_CAP      24
#define CTL_TOKEN_CAP     128
#define CTL_STRATEGY_CAP  24

// ---------------------------------------------------------------------------
// Kinds. Requests are 0x01.., responses 0x40.., and the error frame is 0x7f -
// so a frame says what it is on its own, without the reader having to remember
// which endpoint it asked.
// ---------------------------------------------------------------------------
#define CTL_AUTH      0x01   // -> POST /auth/signup | /auth/signin
#define CTL_META      0x02   // -> POST /meta

#define CTL_SESSION   0x40   // <- /auth/*   [str user_id][str username][str token]
#define CTL_GAME      0x41   // <- /create   [str game_id]
#define CTL_LOBBY     0x42   // <- /meta     [str game_id][u8 status]
#define CTL_APPLIED   0x43   // <- /action   [u8 ok][u8 status]
#define CTL_STATUS    0x44   // <- /status   [i8 status]
#define CTL_STATS     0x45   // <- /stats    the fixed counter block below
#define CTL_HEALTH    0x46   // <- /health, OPTIONS   (empty payload)
#define CTL_ERROR     0x7f   // <- any       [u8 reason]

// The /meta verb, a byte where it used to be a JSON "type" string. An
// UNRECOGNIZED verb is a well-formed frame carrying a verb this server has no
// branch for - exactly what an unrecognized "type" string used to be, and
// answered the same way (nothing happens, the lobby answers anyway). The
// decoder therefore does NOT reject it; only the server's switch ignores it.
#define CTL_META_JOIN      1
#define CTL_META_START     2
#define CTL_META_ADD_BOT   3
#define CTL_META_CONTINUE  4

// Error reasons: one per refusal the JSON bodies used to spell out
// ({"error":"auth"} and friends). A byte carries exactly as much as the string
// did - these were never prose for a human, they were a tag for a client, and
// every client already branches on the HTTP status code first.
#define CTL_ERR_RATE_LIMITED   1   // 429, /auth/signup + /create
#define CTL_ERR_USERNAME       2   // 400, the roster refuses this display name
#define CTL_ERR_FULL           3   // 400, no user slot / no game slot left
#define CTL_ERR_AUTH           4   // 401, no such token
#define CTL_ERR_NO_GAME        5   // 404, no such game
#define CTL_ERR_NOT_PLAYING    6   // 400, this game is not dealt
#define CTL_ERR_NOT_SEATED     7   // 400, this token holds no seat here
#define CTL_ERR_BAD_SEAT       8   // 400, a seat below VIEW_SPECTATOR
#define CTL_ERR_UNAUTHORIZED   9   // 401, a seat view asked for without a token
#define CTL_ERR_NOT_YOUR_SEAT 10   // 403, that seat's hand is not yours to read
#define CTL_ERR_ROUTE         11   // 404, no such endpoint
#define CTL_ERR_WS_AUTH       12   // 401, the /ws upgrade failed its auth/seat check
#define CTL_ERR_WS_KEY        13   // 400, the /ws upgrade carried no usable key

// ---------------------------------------------------------------------------
// Decoded payloads
// ---------------------------------------------------------------------------

typedef struct {
    char user_id[CTL_ID_CAP];
    char username[CTL_NAME_CAP];
    char token[CTL_TOKEN_CAP];
} CtlSession;

typedef struct {
    int  verb;                          // CTL_META_*, or any other byte the sender chose
    char game_id[CTL_ID_CAP];
    char strategy[CTL_STRATEGY_CAP];    // "" unless the verb carried one (add-bot)
} CtlMeta;

// GET /stats' counters, in the order h_stats' JSON listed them. Fixed-width,
// so the payload is exactly 6*4 + 4*8 = 56 bytes and has no length fields of
// its own - same reasoning serialize_user takes for a fixed-width User row.
typedef struct {
    int32_t  live_connections;
    int32_t  max_connections;
    int32_t  games;              // slot high-water, not the live count
    int32_t  games_live;         // games - free_slots
    int32_t  free_slots;
    int32_t  users;
    uint64_t games_reclaimed;
    uint64_t moves_applied;
    uint64_t bot_decisions;
    uint64_t octogen_decisions;
} CtlStats;

#define CTL_STATS_PAYLOAD_LEN 56

// ---------------------------------------------------------------------------
// Encoders. Each writes ONE whole frame into buf[0..cap) and returns the bytes
// written, or -1 if it would not fit (or a field is too long to encode). A
// partial frame is never left behind: the length is checked before the first
// byte goes down.
// ---------------------------------------------------------------------------
int ctl_enc_auth(const char *username, unsigned char *buf, int cap);
int ctl_enc_meta(int verb, const char *game_id, const char *strategy, unsigned char *buf, int cap);

int ctl_enc_session(const char *user_id, const char *username, const char *token,
                    unsigned char *buf, int cap);
int ctl_enc_game(const char *game_id, unsigned char *buf, int cap);
int ctl_enc_lobby(const char *game_id, int status, unsigned char *buf, int cap);
int ctl_enc_applied(bool ok, int status, unsigned char *buf, int cap);
int ctl_enc_status(int status, unsigned char *buf, int cap);
int ctl_enc_stats(const CtlStats *st, unsigned char *buf, int cap);
int ctl_enc_health(unsigned char *buf, int cap);
int ctl_enc_error(int reason, unsigned char *buf, int cap);

// ---------------------------------------------------------------------------
// Decoders. Each reads ONE whole frame from buf[0..len) and returns 1 on
// success, 0 on refusal. On a refusal the out-params are untouched, so a
// caller that pre-zeroed them sees exactly the "absent" it would have seen
// from a missing JSON key.
// ---------------------------------------------------------------------------
int ctl_dec_auth(const unsigned char *buf, int len, char *username_out, int cap);
int ctl_dec_meta(const unsigned char *buf, int len, CtlMeta *out);

int ctl_dec_session(const unsigned char *buf, int len, CtlSession *out);
int ctl_dec_game(const unsigned char *buf, int len, char *game_id_out, int cap);
int ctl_dec_lobby(const unsigned char *buf, int len, char *game_id_out, int cap, int *status_out);
int ctl_dec_applied(const unsigned char *buf, int len, bool *ok_out, int *status_out);
int ctl_dec_status(const unsigned char *buf, int len, int *status_out);
int ctl_dec_stats(const unsigned char *buf, int len, CtlStats *out);
int ctl_dec_error(const unsigned char *buf, int len, int *reason_out);

// The kind byte of a well-formed frame, or -1 if buf[0..len) is not one (wrong
// version, short head, or a payload length that disagrees with `len`). Lets a
// reader tell "the server refused me" from "the server answered" without
// knowing which it expected.
int ctl_kind(const unsigned char *buf, int len);

#endif
