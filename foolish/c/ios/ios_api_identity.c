// ios_api_identity.c - THE ROSTER, AND THE SEAT AND NAME QUESTIONS ASKED OF IT
// (see ios/include/ios_api.h).
//
// Split out of ios_api.c by domain, unchanged. One packed-roster reader, the
// gates a surface asks before it draws a Join button, and Rule F - the fool's
// penalty, which is a roster question too (the rematch key is taken over the
// canonical rotation of the names).
//
// Nothing here decides anything: every entry reads the roster through the one
// reader a seal uses (fio_read_joins, shared with ios_api_msg.c through
// ios_internal.h) and hands msg_wire.c's rule its arguments.

#include "ios_api.h"
#include "ios_internal.h"

#include "game.h"
#include "msg_wire.h"

#include <string.h>

// The packed-roster reader, shared with the seal in ios_api_msg.c so a seal
// cannot write a roster these gates would read differently. The layout and the
// bounding discipline are stated once, in ios_internal.h.
int fio_read_joins(const uint8_t *b, int len, MsgEnvelope *e) {
    if (!b || len < 1) return FIO_EPARSE;
    const int n = b[0];
    if (n > MSG_MAX_JOINS) return FIO_EPARSE;
    int p = 1;
    for (int i = 0; i < n; i++) {
        if (p + 2 > len) return FIO_EPARSE;
        const int seat = b[p];
        const int name_len = b[p + 1];
        if (name_len > MSG_MAX_NAME) return FIO_EPARSE;
        if (p + 2 + name_len > len) return FIO_EPARSE;
        MsgJoin *jn = &e->joins[i];
        jn->seat = (uint8_t)seat;
        jn->name_len = (uint8_t)name_len;
        if (name_len > 0) memcpy(jn->name, b + p + 2, (size_t)name_len);
        p += 2 + name_len;
    }
    if (p != len) return FIO_EPARSE;
    e->n_joins = (uint8_t)n;
    return FIO_EOK;
}

// ---------- Rule F: the fool's penalty ------------------------------------

// A packed roster as a bare array, the shape msg_roster_key wants. Shares
// fio_read_joins so the entries below cannot read a roster differently from the
// way a seal writes one.
static int fio_joins_of(const uint8_t *joins, int joins_len, MsgJoin *out, int *n_out) {
    static MsgEnvelope tmp;   // static: MsgEnvelope is large, and this is a
    msg_envelope_init(&tmp);  // single-threaded actor (see the file header)
    const int rc = fio_read_joins(joins, joins_len, &tmp);
    if (rc != FIO_EOK) return rc;
    if (tmp.n_joins < 2 || tmp.n_joins > MSG_MAX_JOINS) return FIO_EBADARG;
    for (int i = 0; i < tmp.n_joins; i++) out[i] = tmp.joins[i];
    *n_out = tmp.n_joins;
    return FIO_EOK;
}

int fio_msg_carry(const uint8_t *joins_packed, int joins_len, int fool_seat,
                  uint32_t *key_out, int *fool_index_out) {
    if (!joins_packed || !key_out || !fool_index_out) return FIO_EBADARG;
    MsgJoin joins[MSG_MAX_JOINS];
    int n = 0;
    const int rc = fio_joins_of(joins_packed, joins_len, joins, &n);
    if (rc != FIO_EOK) return rc;
    if (fool_seat < 0 || fool_seat >= n) return FIO_EBADARG;

    uint32_t key = 0;
    int rot = 0;
    if (msg_roster_key(joins, n, &key, &rot) != MSG_EOK) return FIO_EBADARG;
    *key_out = key;
    // Back out of the seating into the canonical rotation the key was taken
    // over: canonical[k] == seated[(k + rot) % n], so seat s is index s - rot.
    *fool_index_out = ((fool_seat - rot) % n + n) % n;
    return FIO_EOK;
}

// ---------- the chain layer's gates (msg_wire.h) ---------------------------
//
// Nothing here decides anything: each entry reads the packed roster through the
// one reader a seal uses and hands the rule its arguments.

int fio_nickname_verdict(int n_chars, int n_bytes) {
    return msg_nickname_verdict(n_chars, n_bytes);
}

int fio_name_max_bytes(void) { return MSG_MAX_NAME; }
int fio_name_max_chars(void) { return MSG_MAX_NAME_CHARS; }

int fio_seat_resolve(int cached_seat, int sender_is_local, int n_players,
                     int last_actor_seat, int chat_is_dm) {
    return msg_seat_resolve(cached_seat, sender_is_local, n_players,
                            last_actor_seat, chat_is_dm);
}

// The roster every gate below takes, read once. A blob that does not read is
// an EMPTY roster rather than an error: these gates all fail open, and a caller
// that cannot read its own bubble has already lost the argument elsewhere.
//
// NOT fio_joins_of, which is the rematch key's reader and demands two or more
// players. A lobby with ONE join is the ordinary case here - it is the bubble
// the creator sends - and a gate that refused it would put the Join button
// back on every fresh invite.
static int fio_gate_joins(const uint8_t *joins, int joins_len, MsgJoin *out) {
    static MsgEnvelope tmp;   // static for the same reason fio_joins_of is
    msg_envelope_init(&tmp);
    if (!joins || fio_read_joins(joins, joins_len, &tmp) != FIO_EOK) return 0;
    for (int i = 0; i < tmp.n_joins; i++) out[i] = tmp.joins[i];
    return tmp.n_joins;
}

int fio_roster_name_taken(const uint8_t *joins, int joins_len,
                          const uint8_t *name, int name_len) {
    MsgJoin js[MSG_MAX_JOINS];
    const int n = fio_gate_joins(joins, joins_len, js);
    return msg_name_taken(js, n, (const char *)name, name_len);
}

int fio_seat_claimed_by_name(const uint8_t *joins, int joins_len,
                             const uint8_t *name, int name_len) {
    MsgJoin js[MSG_MAX_JOINS];
    const int n = fio_gate_joins(joins, joins_len, js);
    return msg_seat_claimed_by_name(js, n, (const char *)name, name_len);
}

int fio_seat_cache_disowned(const uint8_t *joins, int joins_len, int cached_seat,
                            const uint8_t *name, int name_len) {
    MsgJoin js[MSG_MAX_JOINS];
    const int n = fio_gate_joins(joins, joins_len, js);
    return msg_seat_cache_disowned(js, n, cached_seat, (const char *)name, name_len);
}

int fio_seat_resolve_on_board(const uint8_t *joins, int joins_len,
                              int cached_seat, int sender_is_local, int n_players,
                              int last_actor_seat, int chat_is_dm,
                              const uint8_t *name, int name_len) {
    MsgJoin js[MSG_MAX_JOINS];
    const int n = fio_gate_joins(joins, joins_len, js);
    return msg_seat_resolve_on_board(js, n, cached_seat, sender_is_local, n_players,
                                     last_actor_seat, chat_is_dm,
                                     (const char *)name, name_len);
}

int fio_seat_resolve_in_lobby(const uint8_t *joins, int joins_len,
                              int cached_seat, int sender_is_local, int n_players,
                              int last_actor_seat, int chat_is_dm,
                              const uint8_t *name, int name_len) {
    MsgJoin js[MSG_MAX_JOINS];
    const int n = fio_gate_joins(joins, joins_len, js);
    return msg_seat_resolve_in_lobby(js, n, cached_seat, sender_is_local, n_players,
                                     last_actor_seat, chat_is_dm,
                                     (const char *)name, name_len);
}

int fio_msg_set_carry(uint32_t key, int fool_index) {
    FioSession *s = fio_session();
    if (key == 0 || fool_index < 0 || fool_index >= MSG_MAX_JOINS) {
        s->msg_carry_key = 0;
        s->msg_carry_fool = MSG_NO_FOOL;
        return FIO_EOK;
    }
    s->msg_carry_key = key;
    s->msg_carry_fool = (uint8_t)fool_index;
    return FIO_EOK;
}

int fio_msg_penalty_fool_seat(const uint8_t *joins_packed, int joins_len,
                              uint32_t carry_key, int carry_fool) {
    if (!joins_packed) return -1;
    MsgJoin joins[MSG_MAX_JOINS];
    int n = 0;
    if (fio_joins_of(joins_packed, joins_len, joins, &n) != FIO_EOK) return -1;
    const uint8_t fool = (carry_fool < 0 || carry_fool > 0xFF)
                       ? (uint8_t)MSG_NO_FOOL : (uint8_t)carry_fool;
    return msg_rematch_fool_seat(joins, n, carry_key, fool);
}

int fio_msg_start_rematch(const uint8_t *joins_packed, int joins_len, uint32_t carry_key,
                          int carry_fool, int *opening_out) {
    if (!joins_packed || !opening_out) return FIO_EBADARG;
    MsgJoin joins[MSG_MAX_JOINS];
    int n = 0;
    const int rc = fio_joins_of(joins_packed, joins_len, joins, &n);
    if (rc != FIO_EOK) return rc;

    const uint8_t fool = (carry_fool < 0 || carry_fool > 0xFF)
                       ? (uint8_t)MSG_NO_FOOL : (uint8_t)carry_fool;
    const int opening = msg_rematch_opening(joins, n, carry_key, fool);

    // Pin BEFORE the deal and set the resident term AFTER it: fio_new_game
    // (which fio_reseat_game runs) consumes the pin and then clears both, so
    // this order is the one that survives it.
    if (opening >= 0) game_open_at_seat(opening);
    const int drc = fio_reseat_game(n);
    if (drc != FIO_EOK) { game_open_at_seat(-1); return drc; }
    fio_session()->msg_opening =
        (opening >= 0) ? (uint8_t)opening : (uint8_t)MSG_NO_OPENING;

    *opening_out = opening;
    return FIO_EOK;
}
