// ask.h - everything a client asks a server that is not a move, and the reply
// to an ask the server refuses.
//
// A move crosses as the action request (table.h table_request_decode). Every
// other request - the lobby edits, the bot nudge - used to cross as a JSON
// object each host spelled for itself ({ type, game_id, bot_id, ... }), and a
// refusal came back as { error: "<prose>" }. Both are bytes now, and both
// layouts are this file's:
//
//   TableAsk     what is asked. A host fills the struct through the generated
//                writer (tools/structgen/specs/view_layout.args) or, where it
//                links the kernel, directly; ask_encode writes the request body
//                and ask_decode reads it back, so no host knows a byte of it.
//   AskRefusal   why an ask (or a move, or any other request) got no answer: a
//                reason, and the kernel's own code behind it. No prose: nothing
//                on a client ever showed the sentence, and the sentence a person
//                does read is the server's log line.
//
// The struct is the crossing with the host; the WIRE is what ask_encode writes,
// so a field added here does not change a byte an older client sends.
//
//   request:  u8 ASK_FORMAT, u8 kind, u8 gid_len, gid, then by kind
//     ASK_JOIN, ASK_START, ASK_CONTINUE, ASK_BUMP   nothing
//     ASK_ADD_BOT             u8 bot_len, bot        (0: any bot not seated)
//     ASK_EXIT                u8 bot_len, bot, u8 player_len, player
//                             (a bot to remove, else a human, else the asker)
//     ASK_REARRANGE_HAND      u8 n, n x u8 hand index
//     ASK_REARRANGE_PLAYERS   u8 n, n x { u8 len, id }
//     ASK_UPDATE_NAME         u16 LE title_len, title
//     ASK_SET_RULES           u8 passing             (1 the classic game, 0 podkidnoy)
//   refusal:  u8 ASK_REFUSAL_FORMAT, u8 why, i32 LE code, i32 LE detail
//
// A body is read whole: a byte past the ask's end is a refusal, like a byte short.
#ifndef CNITRO_ASK_H
#define CNITRO_ASK_H

#include "game.h"
#include "roster.h"
#include <stdint.h>

#define ASK_FORMAT 1

// Append-only numbering: these are on the wire.
#define ASK_JOIN              1
#define ASK_START             2
#define ASK_ADD_BOT           3
#define ASK_EXIT              4
#define ASK_CONTINUE          5
#define ASK_REARRANGE_HAND    6
#define ASK_REARRANGE_PLAYERS 7
#define ASK_UPDATE_NAME       8
#define ASK_SET_RULES         9
// The bot-loop nudge: commits nothing, answers with the asker's envelope (the
// spectator's when not seated), and the host wakes the bots when the table has
// bot work. It needs no seat, because a spectator's client sends it too.
#define ASK_BUMP              10

#define ASK_OK       0
#define ASK_E_WIRE (-301)   // a body that is not an ask, or a struct that is not one
#define ASK_E_CAP  (-302)   // an output buffer is too small

// Raw input, so the title's rule stays table_retitle's (50 characters as the web
// counts them, then trimmed) and not a writer's range check: twice the roster's
// own budget, as roster.h's RosterSpec has it.
#define ASK_TITLE_MAX (2 * ROSTER_TITLE_MAX)
// The longest body ask_encode writes (a rename), with room to spare.
#define ASK_BYTES_MAX 512

typedef struct {
    uint8_t len;
    char    id[ROSTER_ID_MAX];
} AskId;

typedef struct {
    uint8_t  kind;                        // ASK_*
    uint8_t  passing;                     // ASK_SET_RULES
    uint8_t  gid_len;    char gid[ROSTER_GAME_ID_MAX];
    uint8_t  bot_len;    char bot[ROSTER_ID_MAX];       // ASK_ADD_BOT, ASK_EXIT
    uint8_t  player_len; char player[ROSTER_ID_MAX];    // ASK_EXIT
    uint8_t  n_idx;      uint8_t idx[MAX_HAND_SIZE];    // ASK_REARRANGE_HAND
    uint8_t  n_order;    AskId order[MAX_PLAYERS];      // ASK_REARRANGE_PLAYERS
    uint16_t title_len;  char title[ASK_TITLE_MAX];     // ASK_UPDATE_NAME
} TableAsk;

// The request body for `a`: its length, ASK_E_WIRE for a struct that is not an
// ask (an unknown kind, an empty game id, a count past its array), or ASK_E_CAP.
// Only the fields the kind carries are written.
int ask_encode(const TableAsk *a, uint8_t *out, int cap);

// Reads a request body into `out`, zeroed first, so a field the kind does not
// carry reads empty. ASK_OK, or ASK_E_WIRE.
int ask_decode(const uint8_t *p, int len, TableAsk *out);

// ---- the refusal ---------------------------------------------------------------

#define ASK_REFUSAL_FORMAT 1
#define ASK_REFUSAL_BYTES  10

// Why there is no answer. Append-only numbering.
#define ASK_WHY_AUTH      1   // no bearer token, or one that does not verify
#define ASK_WHY_MALFORMED 2   // the body is not a request this endpoint reads
#define ASK_WHY_REFUSED   3   // the kernel refused: `code` a TABLE_E_*, `detail` the ROSTER_E_* behind a TABLE_E_ROSTER
#define ASK_WHY_NOT_FOUND 4   // the game id names no game
#define ASK_WHY_NO_BOT    5   // add-bot: no bot is free to seat, or the one named is not
#define ASK_WHY_INTERNAL  6   // the server failed; its log says how
#define ASK_WHY_METHOD    7   // an HTTP method the endpoint does not take
#define ASK_WHY_ACCOUNT   8   // an account refusal (netwire.h): `code` an NW_ACCOUNT_E_*
#define ASK_WHY_FORBIDDEN 9   // a read or a subscription the caller may not make (not a member)

typedef struct {
    uint8_t why;      // ASK_WHY_*
    int32_t code;
    int32_t detail;
} AskRefusal;

// ASK_REFUSAL_BYTES, ASK_E_WIRE for a reason that is not one, or ASK_E_CAP.
int ask_refusal_encode(const AskRefusal *r, uint8_t *out, int cap);
// ASK_OK, or ASK_E_WIRE for a body that is not exactly a refusal.
int ask_refusal_decode(const uint8_t *p, int len, AskRefusal *out);

#endif
