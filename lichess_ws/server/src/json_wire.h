// json_wire.h - the browser's side of lila-ws's protocol: the JSON text frames a client sends,
// read into a struct, and the frames the gateway sends, written. JSON exists here because it is
// lichess's wire format (SCOPE.md 1.1); no other file of the gateway reads or writes a JSON byte,
// except lila_wire.c which carries JSON payloads opaquely between the socket and Redis.
#ifndef LWS_JSON_WIRE_H
#define LWS_JSON_WIRE_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CO_PING = 1,        // "p", "null", {"t":"p"[,"l":lag]}
    CO_WATCH,           // startWatching: ids[] (up to 16)
    CO_TV_CHANNELS,     // startWatchingTvChannels
    CO_MOVELAT,
    CO_NOTIFIED,
    CO_FOLLOWING,       // following_onlines (d: subscribe, default true)
    CO_MSGTYPE,         // msgType: d the destination user
    CO_USER_FORWARD,    // msgSend, msgRead: the whole object forwarded as tell/user
    CO_SITE_FORWARD,    // anything else on a site socket: tell/sri
    CO_IGNORE,          // opening, anaDests, wrongHole, the eval cache, storm, racer: nothing to do
    CO_LOBBY_IDLE,      // idle: d bool, forwarded
    CO_LOBBY_JOIN,      // join
    CO_LOBBY_FORWARD,   // cancel, joinSeek, cancelSeek, poolIn, poolOut, hookIn, hookOut
    CO_STUDY_FORWARD,   // the study vocabulary (not served, counted as unhandled)
    CO_ROUND_FORWARD,   // resign, abort, draw-yes, ...: the whole object as r/do
    CO_ROUND_MOVE,      // move or drop: uci, blur, lag, move time, ack
    CO_ROUND_HOLD,      // hold: mean, sd
    CO_ROUND_BERSERK,   // berserk: ack
    CO_ROUND_REPORT,    // rep: d.n
    CO_ROUND_FLAG,      // flag: d "white" | "black"
    CO_ROUND_BYE,       // bye2
    CO_CHAT_SAY,        // talk: d the text
    CO_CHAT_TIMEOUT,    // timeout: d.userId, d.reason, d.text
    CO_CHALLENGE_PING,  // ping
    CO_UNEXPECTED,      // not a JSON object, or a type nobody knows: the socket is closed (1011)
} co_kind;

#define CO_WATCH_MAX 16
#define CO_TEXT_MAX 8192

typedef struct {
    co_kind  kind;
    char     t[24];
    bool     has_lag;  int lag_ms;                     // ping "l"
    int      n_watch;  char watch[CO_WATCH_MAX][9];    // startWatching
    bool     flag;                                     // following_onlines d, idle d
    char     uci[8];   bool blur;                      // move: "e2e4", "e7e8q", "N@f3"
    bool     has_lag_ms;                               // move d.l (ms) -> lag_ms above
    bool     has_mt;   int mt_centis;                  // move d.s (base 36 centis)
    bool     has_ack;  int ack;                        // move / berserk d.a
    int      mean, sd;                                 // hold
    int      color;                                    // flag: 0 white, 1 black
    char     user[32];                                 // msgType d, timeout d.userId
    char     reason[32];                               // timeout d.reason
    char     text[CO_TEXT_MAX];                        // talk d, rep d.n, timeout d.text
    cb_span  raw;                                      // the whole message (forwards)
} co_msg;

// Parses one text frame. Always fills out->kind (CO_UNEXPECTED for what cannot be read).
void co_parse(const char *s, size_t n, co_msg *out);

// ---- what the gateway sends ----

// {"t":"<t>","d":<raw json>} into b.
void ci_msg_raw(cb_buf *b, const char *t, const char *json, size_t n);
// {"t":"<t>","d":"<escaped string>"}
void ci_msg_str(cb_buf *b, const char *t, const char *s);
// {"t":"<t>","d":<int>} and {"t":"<t>","d":<bool>} and {"t":"<t>"}
void ci_msg_int(cb_buf *b, const char *t, int64_t v);
void ci_msg_bool(cb_buf *b, const char *t, bool v);
void ci_msg_bare(cb_buf *b, const char *t);
// {"t":"<t>","v":<version>,"d":<raw>} (a round event in full)
void ci_versioned(cb_buf *b, const char *t, int version, const char *json, size_t n);
// {"v":<version>} (the skip form)
void ci_skip(cb_buf *b, int version);
// The full form with `,"dests":{...}` removed (what the mover gets).
void ci_strip_dests(cb_buf *b, const char *full, size_t n);
// {"v":<version>,<the payload without its opening brace> (a room event in full)
void ci_room_versioned(cb_buf *b, int version, const char *json, size_t n);
// {"t":"n","d":<members>,"r":<rounds>}
void ci_lobby_pong(cb_buf *b, int members, int rounds);
// {"t":"fen","d":{"id":..,"lm":..,"fen":"<fen> <w|b>"[,"wc":..,"bc":..]}}
void ci_fen(cb_buf *b, const char *game_id, const char *uci, const char *fen, char turn, bool has_clock, int wc, int bc);
// {"t":"finish","d":{"id":..,"win":"w"|"b"|null}}
void ci_finish(cb_buf *b, const char *game_id, int winner /* -1 none, 0 white, 1 black */);
// {"t":"redirect","d":{"id":<fullId>,"url":"/<fullId>"}}
void ci_redirect(cb_buf *b, const char *full_id);
// {"t":"crowd","d":{"white":bool,"black":bool,"watchers":{}|{"nb":N}|{"users":[names]}}}
void ci_round_crowd(cb_buf *b, bool white, bool black, int nb, const char *const *names, int n_names);
// {"t":"batch","d":[<payloads joined by commas>]}
void ci_batch_begin(cb_buf *b);
void ci_batch_sep(cb_buf *b);
void ci_batch_end(cb_buf *b);

// Reads uci, fen and the clocks out of a move event's JSON the way lila-ws does (the regexes in
// Fens.scala): "uci":"..", "fen":"..", and, when present, "white":<int> ... "black":<int>.
// Returns true when uci and fen were found; has_clock says whether both clocks were.
bool co_move_position(const char *json, size_t n, char uci[8], char fen[96], bool *has_clock, int *wc, int *bc);

#endif
