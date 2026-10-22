// lila_wire.c - lila_wire.h.
#include "lila_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *path; lo_kind kind; int want; } PATHS[] = {
    { "mlat", LO_MLAT, 1 },
    { "r/ver", LO_R_VER, 5 },
    { "tell/flag", LO_TELL_FLAG, 2 },
    { "tell/users", LO_TELL_USERS, 2 },
    { "tell/sri", LO_TELL_SRI, 2 },
    { "tell/all", LO_TELL_ALL, 1 },
    { "disconnect/user", LO_DISCONNECT_USER, 1 },
    { "lobby/pairings", LO_LOBBY_PAIRINGS, 1 },
    { "tell/lobby", LO_TELL_LOBBY, 1 },
    { "tell/lobby/active", LO_TELL_LOBBY_ACTIVE, 1 },
    { "tell/lobby/users", LO_TELL_LOBBY_USERS, 2 },
    { "mod/troll/set", LO_SET_TROLL, 2 },
    { "mod/impersonate", LO_IMPERSONATE, 3 },
    { "rel/follow", LO_FOLLOW, 2 },
    { "rel/unfollow", LO_UNFOLLOW, 2 },
    { "tell/sris", LO_TELL_SRIS, 2 },
    { "tell/room", LO_TELL_ROOM, 2 },
    { "tell/room/version", LO_TELL_ROOM_VERSION, 4 },
    { "tell/room/user", LO_TELL_ROOM_USER, 3 },
    { "tell/room/users", LO_TELL_ROOM_USERS, 3 },
    { "room/stop", LO_ROOM_STOP, 1 },
    { "room/present", LO_ROOM_PRESENT, 3 },
    { "room/filter-present", LO_ROOM_FILTER_PRESENT, 3 },
    { "tell/room/chat", LO_TELL_ROOM_CHAT, 4 },
    { "req/response", LO_REQ_RESPONSE, 2 },
    { "tour/get/waiting", LO_TOUR_GET_WAITING, 2 },
    { "r/tour/standing", LO_R_TOUR_STANDING, 2 },
    { "r/resync/player", LO_R_RESYNC_PLAYER, 1 },
    { "r/gone", LO_R_GONE, 2 },
    { "r/goneIn", LO_R_GONE_IN, 2 },
    { "r/bot/online", LO_R_BOT_ONLINE, 3 },
    { "r/start", LO_R_START, 1 },
    { "r/finish", LO_R_FINISH, 3 },
    { "tv/select", LO_TV_SELECT, 3 },
    { "racer/state", LO_RACER_STATE, 2 },
    { "api/online", LO_API_ONLINE, 2 },
    { "streamers/online", LO_STREAMERS_ONLINE, 1 },
    { "pong", LO_PONG, 1 },
    { "announce/update", LO_ANNOUNCE_UPDATE, 1 },
    { "boot", LO_BOOT, 0 },
    { "lila/stop", LO_LILA_STOP, 1 },
    { "r/versioning-ready", LO_VERSIONING_READY, 0 },
};

void lo_parse(const char *line, size_t n, lo_msg *out) {
    memset(out, 0, sizeof *out);
    size_t plen = 0;
    while (plen < n && line[plen] != ' ') plen++;
    for (size_t i = 0; i < sizeof PATHS / sizeof PATHS[0]; i++) {
        if (strlen(PATHS[i].path) == plen && memcmp(PATHS[i].path, line, plen) == 0) {
            out->kind = PATHS[i].kind;
            out->want = PATHS[i].want;
            break;
        }
    }
    if (out->kind == LO_UNKNOWN) return;
    size_t at = plen < n ? plen + 1 : n;
    for (int k = 0; k < out->want && at <= n; k++) {
        if (k == out->want - 1) {   // the last argument keeps its spaces
            out->arg[k] = cb_span_of(line + at, n - at);
            out->n = k + 1;
            break;
        }
        size_t e = at;
        while (e < n && line[e] != ' ') e++;
        if (e == n && k < out->want - 1) {   // ran out of pieces
            if (e > at) { out->arg[k] = cb_span_of(line + at, e - at); out->n = k + 1; }
            break;
        }
        out->arg[k] = cb_span_of(line + at, e - at);
        out->n = k + 1;
        at = e + 1;
    }
    // "announce/update" with nothing after it is an empty payload, not a malformed line.
    if (out->kind == LO_ANNOUNCE_UPDATE && out->n == 0) { out->arg[0] = cb_span_of(line + n, 0); out->n = 1; }
}

bool lo_commas(cb_span list, size_t *it, char *out, size_t cap) {
    if (lo_absent(list) || *it >= list.n) return false;
    size_t s = *it, e = s;
    while (e < list.n && list.p[e] != ',') e++;
    size_t len = e - s < cap - 1 ? e - s : cap - 1;
    memcpy(out, list.p + s, len);
    out[len] = 0;
    *it = e + 1;
    return true;
}

bool lo_i64(cb_span s, long long *out) {
    char tmp[32];
    if (s.n == 0 || s.n >= sizeof tmp) return false;
    memcpy(tmp, s.p, s.n);
    tmp[s.n] = 0;
    char *end;
    long long v = strtoll(tmp, &end, 10);
    if (*end) return false;
    *out = v;
    return true;
}
bool lo_int(cb_span s, int *out) {
    long long v;
    if (!lo_i64(s, &v) || v < INT32_MIN || v > INT32_MAX) return false;
    *out = (int)v;
    return true;
}
void lo_str(cb_span s, char *out, size_t cap) {
    size_t len = s.n < cap - 1 ? s.n : cap - 1;
    memcpy(out, s.p, len);
    out[len] = 0;
}

// ---- lila-ws to lila ----

int li_round_lane(const char *msg, size_t n) {
    // Java: h = 31 * h + c over UTF-16 code units, in wrapping 32-bit arithmetic. The lines are
    // ASCII (ids, UCIs, flags); a non-ASCII byte in a chat line would differ from the JVM's
    // UTF-16 unit, and chat goes on a single-lane channel anyway.
    uint32_t h = 0;
    for (size_t i = 0; i < n; i++) h = 31u * h + (uint8_t)msg[i];
    int32_t s = (int32_t)h;
    if (s == INT32_MIN) return 0;
    return (s < 0 ? -s : s) % 16;
}

static void dash_or(cb_buf *b, const char *s) { cb_buf_puts(b, s && *s ? s : "-"); }
static void centis_or_dash(cb_buf *b, int c) { if (c < 0) cb_buf_puts(b, "-"); else cb_buf_printf(b, "%d", c); }

void li_tell_sri(cb_buf *b, const char *sri, const char *user, const char *json, size_t n) {
    cb_buf_puts(b, "tell/sri ");
    cb_buf_puts(b, sri);
    cb_buf_putc(b, ' ');
    dash_or(b, user);
    cb_buf_putc(b, ' ');
    cb_buf_append(b, json, n);
}
void li_tell_user(cb_buf *b, const char *user, const char *json, size_t n) {
    cb_buf_puts(b, "tell/user ");
    cb_buf_puts(b, user);
    cb_buf_putc(b, ' ');
    cb_buf_append(b, json, n);
}
void li_notified_batch(cb_buf *b, const char *csv) { cb_buf_puts(b, "notified/batch "); dash_or(b, csv); }
void li_lags(cb_buf *b, const char *csv) { cb_buf_puts(b, "lags "); dash_or(b, csv); }
void li_connect_user(cb_buf *b, const char *user) { cb_buf_puts(b, "connect/user "); cb_buf_puts(b, user); }
void li_disconnect_users(cb_buf *b, const char *csv) { cb_buf_puts(b, "disconnect/users "); dash_or(b, csv); }
void li_boot(cb_buf *b) { cb_buf_puts(b, "boot"); }
void li_ping(cb_buf *b, long long uptime_ms) { cb_buf_printf(b, "ping %lld", uptime_ms); }
void li_connect_sris(cb_buf *b, const char *csv) { cb_buf_puts(b, "connect/sris "); dash_or(b, csv); }
void li_disconnect_sris(cb_buf *b, const char *csv) { cb_buf_puts(b, "disconnect/sris "); dash_or(b, csv); }
void li_counters(cb_buf *b, int members, int rounds) { cb_buf_printf(b, "counters %d %d", members, rounds); }
void li_chat_say(cb_buf *b, const char *room, const char *author, const char *msg) { cb_buf_printf(b, "chat/say %s %s %s", room, author, msg); }
void li_chat_say_w(cb_buf *b, const char *room, const char *user, const char *msg) { cb_buf_printf(b, "chat/say/w %s %s %s", room, user, msg); }
void li_chat_timeout(cb_buf *b, const char *room, const char *user, const char *suspect, const char *reason, const char *text) {
    cb_buf_printf(b, "chat/timeout %s %s %s %s %s", room, user, suspect, reason, text);
}
void li_room_versions(cb_buf *b, const char *csv) { cb_buf_puts(b, "room/versions "); dash_or(b, csv); }
void li_r_do(cb_buf *b, const char *full_id, const char *json, size_t n) {
    cb_buf_puts(b, "r/do ");
    cb_buf_puts(b, full_id);
    cb_buf_putc(b, ' ');
    cb_buf_append(b, json, n);
}
void li_r_move(cb_buf *b, const char *full_id, const char *uci, bool blur, int lag_c, int mt_c, int frame_c) {
    cb_buf_printf(b, "r/move %s %s %c ", full_id, uci, blur ? '+' : '-');
    centis_or_dash(b, lag_c);
    cb_buf_putc(b, ' ');
    centis_or_dash(b, mt_c);
    cb_buf_putc(b, ' ');
    centis_or_dash(b, frame_c);
}
void li_r_berserk(cb_buf *b, const char *game_id, const char *user) { cb_buf_printf(b, "r/berserk %s %s", game_id, user); }
void li_r_hold(cb_buf *b, const char *full_id, const char *ip, int mean, int sd) { cb_buf_printf(b, "r/hold %s %s %d %d", full_id, ip, mean, sd); }
void li_r_report(cb_buf *b, const char *full_id, const char *ip, const char *user, const char *name) {
    cb_buf_printf(b, "r/report %s %s ", full_id, ip);
    dash_or(b, user);
    cb_buf_putc(b, ' ');
    cb_buf_puts(b, name);
}
void li_r_flag(cb_buf *b, const char *game_id, int color, const char *player_id) {
    cb_buf_printf(b, "r/flag %s %c ", game_id, color == 0 ? 'w' : 'b');
    dash_or(b, player_id);
}
void li_r_bye(cb_buf *b, const char *full_id) { cb_buf_printf(b, "r/bye %s", full_id); }
void li_r_ons(cb_buf *b, const char *csv) { cb_buf_puts(b, "r/ons "); dash_or(b, csv); }
void li_r_latency(cb_buf *b, int ms) { cb_buf_printf(b, "r/latency %d", ms); }
void li_req_response(cb_buf *b, int req_id, const char *value) { cb_buf_printf(b, "req/response %d %s", req_id, value); }
