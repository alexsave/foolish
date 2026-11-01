// replay_log_oracle.h - THE SESSION LOG'S REPLAY, AS THE KERNEL CUT IT BEFORE THE MOVE LIST:
// table_replay_code and table_replay_extras over the stored session log (the u48-stamped
// records TableCommit.logs holds), verbatim from table.c as they were, kept only as the oracle
// the move list's cut is held to (tests/move_list_test.c, foolish/server/test/unit_store.c's
// schema-9 upgrade). Nothing ships it. The board is read off a copy, never changed.
#ifndef REPLAY_LOG_ORACLE_H
#define REPLAY_LOG_ORACLE_H

#include "../src/table.h"
#include "../src/replay.h"
#include "../src/replay_extras.h"
#include "../wasm/wire.h"
#include "../../../shared/c/le_bytes.h"
#include <string.h>

static int orl_record_at(const uint8_t *log, int len, int at) {
    if (at + 10 > len) return 0;
    if (log[at + 6] > LOG_DRAW) return TABLE_E_WIRE;
    const int n = 10 + 2 * log[at + 9];
    return at + n > len ? 0 : n;
}
#define log_record_at orl_record_at

static bool orl_info_type(int type) {
    return type == LOG_ATTACK || type == LOG_COVER || type == LOG_PASS || type == LOG_PICKUP;
}
#define info_type orl_info_type

// Where the log's last GAME_START session begins (0 when it has none).
static int orl_session_start(const uint8_t *log, int len) {
    int start = 0, k;
    for (int at = 0; (k = log_record_at(log, len, at)) > 0; at += k)
        if (log[at + 6] == LOG_GAME_START) start = at;
    return start;
}

// A whole session log: every record complete and of a known type. Returns 0 or TABLE_E_WIRE.
static int orl_log_whole(const uint8_t *log, int len) {
    int at = 0, k;
    while ((k = log_record_at(log, len, at)) > 0) at += k;
    return k < 0 || at != len ? TABLE_E_WIRE : 0;
}

// A card byte as the TS readers on both sides of the gate took it: the hidden
// card and (for a target) no card kept, anything else past the last card clamped.
static int orl_gate_card(int b, int target) {
    if (b == 0xFE || (target && b == 0xFF)) return b;
    return b > 51 ? 51 : b;
}

// THE ROUND-TRIP GATE (server/api/common/replay/encode.ts checkInfoActionsMatch):
// the ATTACK, COVER, PASS and PICKUP records of the log's last GAME_START
// session, against the decoded code's, in order - same count, same type, the
// same seat (one the table has), the same card pairs.
static bool orl_replay_verify(int n_seats, const uint8_t *log, int len, const uint8_t *dec, int dec_len) {
    if (dec_len < REPLAY_DEC_HDR) return false;
    const uint32_t n_dec = le_get_u32(dec + 16);
    int d = REPLAY_DEC_HDR, at = orl_session_start(log, len), k = 0;
    uint32_t read = 0;
    for (;;) {
        // The next info record on each side.
        while ((k = log_record_at(log, len, at)) > 0 && !info_type(log[at + 6])) at += k;
        const uint8_t *dr = 0;
        while (read < n_dec) {
            if (d + 4 > dec_len || d + 4 + 2 * dec[d + 3] > dec_len) return false;
            const uint8_t *r = dec + d;
            d += 4 + 2 * r[3];
            read++;
            if (info_type(r[0])) { dr = r; break; }
        }
        if (k <= 0 || !dr) return k <= 0 && !dr;
        const uint8_t *lr = log + at + 6;
        if (lr[0] != dr[0] || lr[1] >= n_seats || lr[1] != dr[1] || lr[3] != dr[3]) return false;
        for (int j = 0; j < lr[3]; j++) {
            if (orl_gate_card(lr[4 + 2 * j], 0) != orl_gate_card(dr[4 + 2 * j], 0)) return false;
            if (orl_gate_card(lr[5 + 2 * j], 1) != orl_gate_card(dr[5 + 2 * j], 1)) return false;
        }
        at += k;
    }
}

// The stored records onto a copy of the loaded board, as table.c log_read read them.
static Game orl_game;
static void orl_log_read(const Table *t, const uint8_t *log, int len) {
    memcpy(&orl_game, t->g, sizeof orl_game);
    Game *g = &orl_game;
    int n = 0, k;
    for (int at = 0; n < MAX_LOGS && (k = log_record_at(log, len, at)) > 0; at += k) {
        const uint8_t *r = log + at + 6;
        GameLog *l = &g->logs[n++];
        l->log_type = (int8_t)r[0];
        l->player_idx = (int8_t)r[1];
        l->defender_index = (int8_t)r[2];
        l->num_pairs = (int8_t)(r[3] < MAX_LOG_PAIRS ? r[3] : MAX_LOG_PAIRS);
        for (int j = 0; j < l->num_pairs; j++) {
            l->pairs[j].primary = card_from_wire_pair(r[4 + 2 * j]);
            l->pairs[j].target = card_from_wire_pair(r[5 + 2 * j]);
        }
    }
    g->num_logs = n;
}

// table_replay_code over the session log, as it was.
static int oracle_replay_code(const Table *t, const uint8_t *seed, int seed_len, const uint8_t *log, int log_len,
                              uint8_t *out, int cap, uint8_t *scratch, int scratch_cap) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    if (log_len < 0 || (log_len > 0 && !log) || orl_log_whole(log, log_len) != 0) return TABLE_E_WIRE;
    orl_log_read(t, log, log_len);
    const int n = replay_encode_v6_from_game(&orl_game, seed, seed_len, 1 << 30, out, cap);
    if (n < 0) return n;
    const int dn = replay_decode(out, n, scratch, scratch_cap);
    if (dn < 0) return dn;
    return orl_replay_verify(t->g->num_players, log, log_len, scratch, dn) ? n : TABLE_E_REPLAY_VERIFY;
}

// The session's move times, one at a time (replay_extras.h ReplayExtrasGap): the
// GAME_START and info records of the last session, in order. A cursor, so the
// encoder's in-order walks cost one pass each.
typedef struct {
    const uint8_t *log;
    int len, start;
    int at, index;        // the record `index` is at, after the one at `start`
    double prev;
} TimesCursor;

static bool orl_timed_type(int type) { return type == LOG_GAME_START || info_type(type); }

// The next timed record at or after `at`, or -1.
static int timed_next(const TimesCursor *c, int at) {
    int k;
    while ((k = log_record_at(c->log, c->len, at)) > 0) {
        if (orl_timed_type(c->log[at + 6])) return at;
        at += k;
    }
    return -1;
}

// Exactly the JavaScript arithmetic (Date.parse(created_at) / 1000, then a
// difference): an IEEE division, which no build can turn into a reciprocal
// multiply now that none compiles with -ffast-math.
static double record_seconds(const uint8_t *rec) {
    return (double)(int64_t)le_get_u48(rec) / 1000;
}

static double times_gap(void *ctx, int i) {
    TimesCursor *c = (TimesCursor *)ctx;
    if (i == 0 || i != c->index + 1) {   // restart from the first gap
        c->at = timed_next(c, c->start);
        c->prev = record_seconds(c->log + c->at);
        c->index = -1;
        for (int j = 0; j < i; j++) times_gap(ctx, j);
    }
    c->at = timed_next(c, c->at + log_record_at(c->log, c->len, c->at));
    volatile double t = record_seconds(c->log + c->at);
    volatile double g = t - c->prev;
    c->prev = t;
    c->index = i;
    return g > 0 ? g : 0;
}

// table_replay_extras over the session log, as it was.
static int oracle_replay_extras(const Table *t, const uint8_t *log, int log_len, uint8_t *out, int cap) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    if (log_len < 0 || (log_len > 0 && !log) || orl_log_whole(log, log_len) != 0) return TABLE_E_WIRE;
    const uint8_t *names[MAX_PLAYERS];
    int lens[MAX_PLAYERS];
    for (int s = 0; s < t->r.n; s++) { names[s] = (const uint8_t *)t->r.seats[s].name; lens[s] = t->r.seats[s].name_len; }
    TimesCursor c = { log, log_len, orl_session_start(log, log_len), 0, -1, 0 };
    int n_times = 0;
    for (int at = timed_next(&c, c.start); at >= 0; at = timed_next(&c, at + log_record_at(log, log_len, at))) n_times++;
    if (n_times - 1 > 0xffff) return TABLE_E_WIRE;
    const int first = timed_next(&c, c.start);
    const int n = replay_extras_encode_parts(names, lens, t->r.n, n_times > 0,
                                             n_times > 0 ? record_seconds(log + first) : 0,
                                             n_times > 0 ? n_times - 1 : 0, times_gap, &c, out, cap);
    return n == -REPLAY_EXTRAS_ECAP ? TABLE_E_CAP : n;
}


#undef log_record_at
#undef info_type
#endif
