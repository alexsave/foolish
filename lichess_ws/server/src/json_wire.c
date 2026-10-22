// json_wire.c - json_wire.h. The reader is c_base's tolerant path reader (util/json.h): a
// client message is under 200 bytes and names one of a dozen types, so a handful of path
// lookups is the whole parse; the pings, which are most of the traffic, never reach it.
#include "json_wire.h"

#include "util/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_sq(const char *p) { return p[0] >= 'a' && p[0] <= 'h' && p[1] >= '1' && p[1] <= '8'; }

static bool uci_ok(const char *u) {
    size_t n = strlen(u);
    if (n != 4 && n != 5) return false;
    if (!is_sq(u) || !is_sq(u + 2)) return false;
    return n == 4 || strchr("qrbnk", u[4]) != NULL;
}

static int role_letter(const char *name) {
    static const struct { const char *n; char c; } R[] = {
        { "pawn", 'P' }, { "knight", 'N' }, { "bishop", 'B' }, { "rook", 'R' }, { "queen", 'Q' }, { "king", 'K' },
    };
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++)
        if (strcmp(R[i].n, name) == 0) return R[i].c;
    return 0;
}

static bool get_int(const char *s, size_t n, const char *path, int *out) {
    int64_t v;
    if (!cb_json_get_i64(s, n, path, &v) || v < INT32_MIN || v > INT32_MAX) return false;
    *out = (int)v;
    return true;
}

static bool in_list(const char *t, const char *const *list) {
    for (; *list; list++)
        if (strcmp(t, *list) == 0) return true;
    return false;
}

static const char *const ROUND_FORWARDS[] = { "blindfold-yes", "blindfold-no", "moretime", "rematch-yes", "rematch-no", "takeback-yes",
    "takeback-no", "draw-yes", "draw-no", "draw-claim", "resign", "resign-force", "draw-force", "abort", "outoftime", NULL };
static const char *const LOBBY_FORWARDS[] = { "cancel", "joinSeek", "cancelSeek", "poolIn", "poolOut", "hookIn", "hookOut", NULL };
static const char *const STUDY_FORWARDS[] = { "anaMove", "anaDrop", "like", "setPath", "deleteNode", "promote", "forceVariation", "setRole",
    "kick", "leave", "shapes", "addChapter", "setChapter", "editChapter", "descStudy", "descChapter", "deleteChapter", "clearAnnotations",
    "sortChapters", "editStudy", "setTag", "setComment", "deleteComment", "setGamebook", "toggleGlyph", "explorerGame", "requestAnalysis",
    "invite", "relaySync", "setTopics", "clearVariations", NULL };
static const char *const IGNORED[] = { "opening", "anaDests", "wrongHole", "evalGet", "evalPut", "evalGetMulti", "sk1", "racerScore",
    "racerJoin", "racerStart", NULL };

void co_parse(const char *s, size_t n, co_msg *m) {
    memset(m, 0, sizeof *m);
    m->raw = cb_span_of(s, n);
    m->kind = CO_UNEXPECTED;
    if ((n == 1 && s[0] == 'p') || (n == 4 && memcmp(s, "null", 4) == 0) || (n == 9 && memcmp(s, "{\"t\":\"p\"}", 9) == 0)) {
        m->kind = CO_PING;
        return;
    }
    if (cb_json_find(s, n, "", NULL) != CB_JSON_OBJ) return;
    if (!cb_json_get_str(s, n, "t", m->t, sizeof m->t)) return;
    const char *t = m->t;
    if (strcmp(t, "p") == 0) {
        m->kind = CO_PING;
        m->has_lag = get_int(s, n, "l", &m->lag_ms);
    } else if (strcmp(t, "startWatching") == 0) {
        char d[512];
        if (!cb_json_get_str(s, n, "d", d, sizeof d)) { m->kind = CO_IGNORE; return; }   // old apps send empty lists
        char *save = NULL;
        for (char *tok = strtok_r(d, " ", &save); tok && m->n_watch < CO_WATCH_MAX; tok = strtok_r(NULL, " ", &save)) {
            if (strlen(tok) != 8) continue;
            bool dup = false;
            for (int i = 0; i < m->n_watch; i++) if (strcmp(m->watch[i], tok) == 0) dup = true;
            if (!dup) cb_strlcpy(m->watch[m->n_watch++], tok, 9);
        }
        m->kind = CO_WATCH;
    } else if (strcmp(t, "startWatchingTvChannels") == 0) m->kind = CO_TV_CHANNELS;
    else if (strcmp(t, "moveLat") == 0) m->kind = CO_MOVELAT;
    else if (strcmp(t, "notified") == 0) m->kind = CO_NOTIFIED;
    else if (strcmp(t, "following_onlines") == 0) {
        m->flag = true;
        cb_json_get_bool(s, n, "d", &m->flag);
        m->kind = CO_FOLLOWING;
    } else if (strcmp(t, "msgType") == 0) {
        if (cb_json_get_str(s, n, "d", m->user, sizeof m->user)) m->kind = CO_MSGTYPE;
    } else if (strcmp(t, "msgSend") == 0 || strcmp(t, "msgRead") == 0) m->kind = CO_USER_FORWARD;
    else if (strcmp(t, "idle") == 0) {
        if (cb_json_get_bool(s, n, "d", &m->flag)) m->kind = CO_LOBBY_IDLE;
    } else if (strcmp(t, "join") == 0) m->kind = CO_LOBBY_JOIN;
    else if (in_list(t, LOBBY_FORWARDS)) m->kind = CO_LOBBY_FORWARD;
    else if (in_list(t, STUDY_FORWARDS)) m->kind = CO_STUDY_FORWARD;
    else if (strcmp(t, "move") == 0 || strcmp(t, "drop") == 0) {
        if (cb_json_find(s, n, "d", NULL) != CB_JSON_OBJ) return;
        if (t[0] == 'm') {
            if (!cb_json_get_str(s, n, "d.u", m->uci, sizeof m->uci)) {
                char from[4], to[4], prom[4] = "";
                if (!cb_json_get_str(s, n, "d.from", from, sizeof from) || !cb_json_get_str(s, n, "d.to", to, sizeof to)) return;
                cb_json_get_str(s, n, "d.promotion", prom, sizeof prom);
                snprintf(m->uci, sizeof m->uci, "%s%s%s", from, to, prom);
            }
            if (!uci_ok(m->uci)) return;
        } else {
            char role[16], pos[4];
            if (!cb_json_get_str(s, n, "d.role", role, sizeof role) || !cb_json_get_str(s, n, "d.pos", pos, sizeof pos)) return;
            int r = role_letter(role);
            if (!r || strlen(pos) != 2 || !is_sq(pos)) return;
            snprintf(m->uci, sizeof m->uci, "%c@%s", r, pos);
        }
        int b;
        m->blur = get_int(s, n, "d.b", &b) && b == 1;
        m->has_ack = get_int(s, n, "d.a", &m->ack);
        m->has_lag_ms = get_int(s, n, "d.l", &m->lag_ms);
        char mt[16];
        if (cb_json_get_str(s, n, "d.s", mt, sizeof mt) && mt[0]) {
            char *end;
            long v = strtol(mt, &end, 36);
            if (*end == 0 && v >= INT32_MIN && v <= INT32_MAX) { m->has_mt = true; m->mt_centis = (int)v; }
        }
        m->kind = CO_ROUND_MOVE;
    } else if (strcmp(t, "hold") == 0) {
        if (get_int(s, n, "d.mean", &m->mean) && get_int(s, n, "d.sd", &m->sd)) m->kind = CO_ROUND_HOLD;
    } else if (strcmp(t, "berserk") == 0) {
        m->has_ack = get_int(s, n, "d.a", &m->ack);
        m->kind = CO_ROUND_BERSERK;
    } else if (strcmp(t, "rep") == 0) {
        if (cb_json_get_str(s, n, "d.n", m->text, sizeof m->text)) m->kind = CO_ROUND_REPORT;
    } else if (strcmp(t, "flag") == 0) {
        char c[8];
        if (cb_json_get_str(s, n, "d", c, sizeof c)) {
            if (strcmp(c, "white") == 0) { m->color = 0; m->kind = CO_ROUND_FLAG; }
            else if (strcmp(c, "black") == 0) { m->color = 1; m->kind = CO_ROUND_FLAG; }
        }
    } else if (strcmp(t, "bye2") == 0) m->kind = CO_ROUND_BYE;
    else if (in_list(t, ROUND_FORWARDS)) m->kind = CO_ROUND_FORWARD;
    else if (strcmp(t, "talk") == 0) {
        if (cb_json_get_str(s, n, "d", m->text, sizeof m->text)) m->kind = CO_CHAT_SAY;
    } else if (strcmp(t, "timeout") == 0) {
        if (cb_json_get_str(s, n, "d.userId", m->user, sizeof m->user) && cb_json_get_str(s, n, "d.reason", m->reason, sizeof m->reason) &&
            cb_json_get_str(s, n, "d.text", m->text, sizeof m->text))
            m->kind = CO_CHAT_TIMEOUT;
    } else if (strcmp(t, "ping") == 0) m->kind = CO_CHALLENGE_PING;
    else if (in_list(t, IGNORED)) m->kind = CO_IGNORE;
}

// ---- writing ----

static void head(cb_buf *b, const char *t) {
    cb_buf_puts(b, "{\"t\":\"");
    cb_buf_puts(b, t);
    cb_buf_putc(b, '"');
}

void ci_msg_raw(cb_buf *b, const char *t, const char *json, size_t n) {
    head(b, t);
    cb_buf_puts(b, ",\"d\":");
    cb_buf_append(b, json, n);
    cb_buf_putc(b, '}');
}
void ci_msg_str(cb_buf *b, const char *t, const char *s) {
    head(b, t);
    cb_buf_puts(b, ",\"d\":\"");
    cb_json_escape(b, s, strlen(s));
    cb_buf_puts(b, "\"}");
}
void ci_msg_int(cb_buf *b, const char *t, int64_t v) { head(b, t); cb_buf_printf(b, ",\"d\":%lld}", (long long)v); }
void ci_msg_bool(cb_buf *b, const char *t, bool v) { head(b, t); cb_buf_puts(b, v ? ",\"d\":true}" : ",\"d\":false}"); }
void ci_msg_bare(cb_buf *b, const char *t) { head(b, t); cb_buf_putc(b, '}'); }

void ci_versioned(cb_buf *b, const char *t, int version, const char *json, size_t n) {
    head(b, t);
    cb_buf_printf(b, ",\"v\":%d,\"d\":", version);
    cb_buf_append(b, json, n);
    cb_buf_putc(b, '}');
}
void ci_skip(cb_buf *b, int version) { cb_buf_printf(b, "{\"v\":%d}", version); }

void ci_strip_dests(cb_buf *b, const char *full, size_t n) {
    static const char KEY[] = ",\"dests\":{";
    const size_t klen = sizeof KEY - 1;
    size_t i = 0;
    while (i < n) {
        const char *at = NULL;
        for (size_t j = i; j + klen <= n; j++)
            if (full[j] == ',' && memcmp(full + j, KEY, klen) == 0) { at = full + j; break; }
        if (!at) break;
        size_t start = (size_t)(at - full), body = start + klen, end = body;
        while (end < n && full[end] != '}') end++;
        if (end >= n || end == body) {   // no closing brace, or an empty object: the pattern needs one char inside
            cb_buf_append(b, full + i, body - i);
            i = body;
            continue;
        }
        cb_buf_append(b, full + i, start - i);
        i = end + 1;
    }
    cb_buf_append(b, full + i, n - i);
}

void ci_room_versioned(cb_buf *b, int version, const char *json, size_t n) {
    cb_buf_printf(b, "{\"v\":%d,", version);
    if (n > 0) cb_buf_append(b, json + 1, n - 1);
}

void ci_lobby_pong(cb_buf *b, int members, int rounds) { cb_buf_printf(b, "{\"t\":\"n\",\"d\":%d,\"r\":%d}", members, rounds); }

void ci_fen(cb_buf *b, const char *game_id, const char *uci, const char *fen, char turn, bool has_clock, int wc, int bc) {
    cb_buf_puts(b, "{\"t\":\"fen\",\"d\":{\"id\":\"");
    cb_json_escape(b, game_id, strlen(game_id));
    cb_buf_puts(b, "\",\"lm\":\"");
    cb_json_escape(b, uci, strlen(uci));
    cb_buf_puts(b, "\",\"fen\":\"");
    cb_json_escape(b, fen, strlen(fen));
    cb_buf_putc(b, ' ');
    cb_buf_putc(b, turn);
    cb_buf_putc(b, '"');
    if (has_clock) cb_buf_printf(b, ",\"wc\":%d,\"bc\":%d", wc, bc);
    cb_buf_puts(b, "}}");
}

void ci_finish(cb_buf *b, const char *game_id, int winner) {
    cb_buf_puts(b, "{\"t\":\"finish\",\"d\":{\"id\":\"");
    cb_json_escape(b, game_id, strlen(game_id));
    cb_buf_puts(b, winner < 0 ? "\",\"win\":null}}" : winner == 0 ? "\",\"win\":\"w\"}}" : "\",\"win\":\"b\"}}");
}

void ci_redirect(cb_buf *b, const char *full_id) {
    cb_buf_puts(b, "{\"t\":\"redirect\",\"d\":{\"id\":\"");
    cb_json_escape(b, full_id, strlen(full_id));
    cb_buf_puts(b, "\",\"url\":\"/");
    cb_json_escape(b, full_id, strlen(full_id));
    cb_buf_puts(b, "\"}}");
}

void ci_round_crowd(cb_buf *b, bool white, bool black, int nb, const char *const *names, int n_names) {
    cb_buf_printf(b, "{\"t\":\"crowd\",\"d\":{\"white\":%s,\"black\":%s,\"watchers\":", white ? "true" : "false", black ? "true" : "false");
    if (nb == 0) cb_buf_puts(b, "{}");
    else if (nb > 10 || !names) cb_buf_printf(b, "{\"nb\":%d}", nb);
    else {
        cb_buf_puts(b, "{\"users\":[");
        for (int i = 0; i < n_names; i++) {
            if (i) cb_buf_putc(b, ',');
            cb_buf_putc(b, '"');
            cb_json_escape(b, names[i], strlen(names[i]));
            cb_buf_putc(b, '"');
        }
        cb_buf_puts(b, "]}");
    }
    cb_buf_puts(b, "}}");
}

void ci_batch_begin(cb_buf *b) { cb_buf_puts(b, "{\"t\":\"batch\",\"d\":["); }
void ci_batch_sep(cb_buf *b) { cb_buf_putc(b, ','); }
void ci_batch_end(cb_buf *b) { cb_buf_puts(b, "]}"); }

static const char *find_key(const char *p, size_t n, const char *key, size_t klen) {
    for (size_t i = 0; i + klen <= n; i++)
        if (memcmp(p + i, key, klen) == 0) return p + i + klen;
    return NULL;
}

static bool take_quoted(const char *p, const char *end, char *out, size_t cap) {
    size_t i = 0;
    while (p < end && *p != '"') {
        if (i + 1 >= cap) return false;
        out[i++] = *p++;
    }
    if (p >= end || i == 0) return false;
    out[i] = 0;
    return true;
}

bool co_move_position(const char *json, size_t n, char uci[8], char fen[96], bool *has_clock, int *wc, int *bc) {
    const char *end = json + n;
    const char *u = find_key(json, n, "uci\":\"", 6);
    if (!u || !take_quoted(u, end, uci, 8)) return false;
    const char *f = find_key(u, (size_t)(end - u), "fen\":\"", 6);
    if (!f || !take_quoted(f, end, fen, 96)) return false;
    *has_clock = false;
    const char *w = f;
    for (;;) {   // "white":<digits>: the first such after the fen, as the regex would backtrack to
        w = find_key(w, (size_t)(end - w), "white\":", 7);
        if (!w) return true;
        if (w < end && isdigit((unsigned char)*w)) break;
    }
    const char *k = w;
    for (;;) {
        k = find_key(k, (size_t)(end - k), "black\":", 7);
        if (!k) return true;
        if (k < end && isdigit((unsigned char)*k)) break;
    }
    *wc = atoi(w);
    *bc = atoi(k);
    *has_clock = true;
    return true;
}
