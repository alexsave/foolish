// lila_wire.h - lila's side of lila-ws's protocol: the text lines on Redis pub/sub, in both
// directions (SCOPE.md 1.1, "Redis protocol"). A line is `<path> <args...>`; `-` is an absent
// value, `+`/`-` a boolean, commas separate a list, and a JSON payload, where there is one, is
// the last argument and is carried opaquely.
#ifndef LWS_LILA_WIRE_H
#define LWS_LILA_WIRE_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>

// ---- lila to lila-ws ----

typedef enum {
    LO_UNKNOWN = 0,
    LO_MLAT,              // mlat <ms>
    LO_TELL_FLAG,         // tell/flag <flag> <json>
    LO_TELL_USERS,        // tell/users <users> <json>
    LO_TELL_SRI,          // tell/sri <sri> <json>
    LO_TELL_SRIS,         // tell/sris <sris> <json>
    LO_TELL_ALL,          // tell/all <json>
    LO_DISCONNECT_USER,   // disconnect/user <user>
    LO_LOBBY_PAIRINGS,    // lobby/pairings <sri:fullId,...>
    LO_TELL_LOBBY,        // tell/lobby <json>
    LO_TELL_LOBBY_ACTIVE, // tell/lobby/active <json>
    LO_TELL_LOBBY_USERS,  // tell/lobby/users <users> <json>
    LO_SET_TROLL,         // mod/troll/set <user> <+/->
    LO_IMPERSONATE,       // mod/impersonate <mod> <user> <+/->
    LO_FOLLOW,            // rel/follow <u1> <u2>
    LO_UNFOLLOW,          // rel/unfollow <u1> <u2>
    LO_TELL_ROOM,         // tell/room <room> <json>
    LO_TELL_ROOM_VERSION, // tell/room/version <room> <v> <troll> <json>
    LO_TELL_ROOM_USER,    // tell/room/user <room> <user> <json>
    LO_TELL_ROOM_USERS,   // tell/room/users <room> <users> <json>
    LO_ROOM_STOP,         // room/stop <room>
    LO_ROOM_PRESENT,      // room/present <reqId> <room> <user>
    LO_ROOM_FILTER_PRESENT, // room/filter-present <reqId> <room> <users>
    LO_TELL_ROOM_CHAT,    // tell/room/chat <room> <v> <troll> <json>
    LO_REQ_RESPONSE,      // req/response <reqId> <body>
    LO_TOUR_GET_WAITING,  // tour/get/waiting <room> <name>
    LO_R_TOUR_STANDING,   // r/tour/standing <tour> <json>
    LO_R_RESYNC_PLAYER,   // r/resync/player <fullId>
    LO_R_GONE,            // r/gone <fullId> <+/->
    LO_R_GONE_IN,         // r/goneIn <fullId> <seconds>
    LO_R_BOT_ONLINE,      // r/bot/online <gameId> <w|b> <+/->
    LO_R_START,           // r/start <users>
    LO_R_FINISH,          // r/finish <gameId> <w|b|-> <users>
    LO_TV_SELECT,         // tv/select <gameId> <speed> <json>
    LO_RACER_STATE,       // racer/state <race> <json>
    LO_API_ONLINE,        // api/online <user> <+/->
    LO_STREAMERS_ONLINE,  // streamers/online <json>
    LO_PONG,              // pong <uptime ms>
    LO_ANNOUNCE_UPDATE,   // announce/update [<json>]
    LO_BOOT,              // boot
    LO_LILA_STOP,         // lila/stop <reqId>
    LO_VERSIONING_READY,  // r/versioning-ready
    LO_R_VER,             // r/ver <gameId> <v> <flags> <type> <json>
} lo_kind;

#define LO_MAX_ARGS 5

typedef struct {
    lo_kind kind;
    int     n;                   // arguments found (the path's count, or fewer: then the line is malformed)
    int     want;                // the path's count
    cb_span arg[LO_MAX_ARGS];    // split on spaces into `want` pieces; the last keeps its spaces
} lo_msg;

// Reads one line. kind LO_UNKNOWN for a path nobody knows; a known path with fewer arguments
// than it takes answers with n < want.
void lo_parse(const char *line, size_t n, lo_msg *out);

// Helpers over the argument conventions.
static inline bool lo_bool(cb_span s) { return s.n == 1 && s.p[0] == '+'; }
static inline bool lo_absent(cb_span s) { return s.n == 1 && s.p[0] == '-'; }
// Walks a comma list ("-" is empty): *it starts at 0; fills out with the next item (NUL-terminated,
// cut at cap-1). false at the end.
bool lo_commas(cb_span list, size_t *it, char *out, size_t cap);
bool lo_int(cb_span s, int *out);
bool lo_i64(cb_span s, long long *out);
// A span as a NUL-terminated string into out (cut at cap-1).
void lo_str(cb_span s, char *out, size_t cap);

// ---- lila-ws to lila ----

// The r-in lane a line goes to: Java's String.hashCode over the line, absolute, modulo 16, with
// the one quirk of Scala's Int.abs (Int.MinValue stays negative, so its lane is 0).
int li_round_lane(const char *msg, size_t n);

void li_tell_sri(cb_buf *b, const char *sri, const char *user, const char *json, size_t n);   // user NULL: "-"
void li_tell_user(cb_buf *b, const char *user, const char *json, size_t n);
void li_notified_batch(cb_buf *b, const char *users_csv);
void li_lags(cb_buf *b, const char *csv);                 // "user:ms,..."
void li_connect_user(cb_buf *b, const char *user);
void li_disconnect_users(cb_buf *b, const char *csv);
void li_boot(cb_buf *b);
void li_ping(cb_buf *b, long long uptime_ms);
void li_connect_sris(cb_buf *b, const char *csv);         // "sri user,sri,..." ("-" when empty)
void li_disconnect_sris(cb_buf *b, const char *csv);
void li_counters(cb_buf *b, int members, int rounds);
void li_chat_say(cb_buf *b, const char *room, const char *author, const char *msg);
void li_chat_say_w(cb_buf *b, const char *room, const char *user, const char *msg);
void li_chat_timeout(cb_buf *b, const char *room, const char *user, const char *suspect, const char *reason, const char *text);
void li_room_versions(cb_buf *b, const char *csv);        // "id:v,..."
void li_r_do(cb_buf *b, const char *full_id, const char *json, size_t n);
// lag, move time and frame lag in centis, or -1 when absent.
void li_r_move(cb_buf *b, const char *full_id, const char *uci, bool blur, int lag_c, int mt_c, int frame_c);
void li_r_berserk(cb_buf *b, const char *game_id, const char *user);
void li_r_hold(cb_buf *b, const char *full_id, const char *ip, int mean, int sd);
void li_r_report(cb_buf *b, const char *full_id, const char *ip, const char *user, const char *name);
void li_r_flag(cb_buf *b, const char *game_id, int color, const char *player_id);   // player_id NULL: "-"
void li_r_bye(cb_buf *b, const char *full_id);
void li_r_ons(cb_buf *b, const char *csv);                // "<gameId>+-,<gameId>,..."
void li_r_latency(cb_buf *b, int ms);
void li_req_response(cb_buf *b, int req_id, const char *value);

// The "-" that stands for an empty list, as a convenience for list builders.
static inline const char *li_csv_or_dash(const cb_buf *csv) { return csv->len ? cb_buf_cstr(csv) : "-"; }

#endif
