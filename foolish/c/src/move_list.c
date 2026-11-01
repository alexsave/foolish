// move_list.c - the session's move list (move_list.h): its one writer and its
// one reader.
#include "move_list.h"
#include "game.h"
#include <stddef.h>
#include <string.h>

enum { K_MARK = 0, K_ATTACK = 1, K_COVER = 2, K_PASS = 3, K_PICKUP = 4, K_GOOD = 5, K_TICK = 6 };
enum { M_START = 0, M_ROUND_END = 1, M_CUT = 2 };

#define MS_LIMIT ((int64_t)1 << 48)

static uint64_t zigzag(int64_t d) { return d < 0 ? ((uint64_t)(-(d + 1)) << 1) | 1u : (uint64_t)d << 1; }
static int64_t unzigzag(uint64_t z) { return (z & 1u) ? -(int64_t)(z >> 1) - 1 : (int64_t)(z >> 1); }

static bool timed_kind(int kind) {
    return kind == MOVE_LIST_START || kind == LOG_ATTACK || kind == LOG_COVER || kind == LOG_PASS
        || kind == LOG_PICKUP;
}

int move_list_open(MoveListCursor *c, const uint8_t *list, int len) {
    memset(c, 0, sizeof(*c));
    c->prev = -1;
    if (len < 0 || (len > 0 && !list)) return MOVE_LIST_E_PARSE;
    c->p = list;
    c->len = len;
    if (len == 0) return 0;
    if (list[0] == MOVE_LIST_BROKEN) {
        if (len != 1) return MOVE_LIST_E_PARSE;
        c->at = 1;
        return 1;
    }
    if (list[0] != MOVE_LIST_FORMAT) return MOVE_LIST_E_PARSE;
    c->at = 1;
    return 0;
}

// The TICK at c->at (its head already read as h): the time moved. 0 or E_PARSE.
static int read_tick(MoveListCursor *c, uint8_t h) {
    uint64_t z = h & 15u;
    if (h & 16u) {
        uint64_t rest = 0;
        int shift = 0;
        for (;;) {
            if (c->at >= c->len || shift > 49) return MOVE_LIST_E_PARSE;
            const uint8_t b = c->p[c->at++];
            rest |= (uint64_t)(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80)) {
                if (b == 0) return MOVE_LIST_E_PARSE;   // minimal: no empty last byte
                break;
            }
        }
        z |= rest << 4;
    }
    if (z == 0 || z >= ((uint64_t)1 << 50)) return MOVE_LIST_E_PARSE;
    const int64_t ms = c->ms + unzigzag(z);
    if (ms < 0 || ms >= MS_LIMIT) return MOVE_LIST_E_PARSE;
    c->ms = ms;
    return 0;
}

int move_list_next(MoveListCursor *c, MoveListItem *it) {
    if (!c->p || c->at >= c->len) return 0;
    if (c->p[0] == MOVE_LIST_BROKEN) return 0;
    bool ticked = false;
    uint8_t h = c->p[c->at++];
    if ((h >> 5) == K_TICK) {
        if (read_tick(c, h) < 0 || c->at >= c->len) return MOVE_LIST_E_PARSE;
        ticked = true;
        h = c->p[c->at++];
    }
    const int kind = h >> 5, low = h & 31;
    memset(it, 0, offsetof(MoveListItem, cards));
    it->seat = low & 7;
    switch (kind) {
        case K_MARK:
            it->seat = -1;
            if (low == M_START) {
                if (c->prev != -1) return MOVE_LIST_E_PARSE;
                it->kind = MOVE_LIST_START;
            } else if (low == M_ROUND_END || low == M_CUT) {
                if (c->prev != LOG_GOOD) return MOVE_LIST_E_PARSE;
                it->kind = low == M_ROUND_END ? MOVE_LIST_ROUND_END : MOVE_LIST_CUT;
            } else {
                return MOVE_LIST_E_PARSE;
            }
            break;
        case K_ATTACK:
        case K_PASS: {
            const int cnt = low >> 3;
            int n = cnt + 1;
            if (cnt == 3) {
                if (c->at >= c->len) return MOVE_LIST_E_PARSE;
                n = c->p[c->at++];
                if (n < 4 || n > MOVE_LIST_MAX_CARDS) return MOVE_LIST_E_PARSE;
            }
            it->kind = kind == K_ATTACK ? LOG_ATTACK : LOG_PASS;
            it->n = n;
            break;
        }
        case K_COVER:
            if (low >> 3) return MOVE_LIST_E_PARSE;
            it->kind = LOG_COVER;
            it->n = 2;
            break;
        case K_PICKUP:
        case K_GOOD:
            if (low >> 3) return MOVE_LIST_E_PARSE;
            it->kind = kind == K_PICKUP ? LOG_PICKUP : LOG_GOOD;
            break;
        default:   // a second TICK, or kind 7
            return MOVE_LIST_E_PARSE;
    }
    if (c->at + it->n > c->len) return MOVE_LIST_E_PARSE;
    for (int i = 0; i < it->n; i++) {
        it->cards[i] = c->p[c->at++];
        if (it->cards[i] > 51) return MOVE_LIST_E_PARSE;
    }
    it->timed = timed_kind(it->kind);
    if (ticked && !it->timed) return MOVE_LIST_E_PARSE;   // a TICK times the item after it
    it->ms = c->ms;
    c->prev = it->kind;
    return 1;
}

int move_list_scan(const uint8_t *list, int len, MoveListTail *tail) {
    MoveListCursor c;
    MoveListItem it;
    memset(tail, 0, sizeof(*tail));
    const int o = move_list_open(&c, list, len);
    if (o < 0) return o;
    if (o == 1) { tail->broken = true; return 0; }
    int r;
    while ((r = move_list_next(&c, &it)) > 0) {}
    if (r < 0) return r;
    tail->ms = c.ms;
    tail->last_good = c.prev == LOG_GOOD;
    return 0;
}

static int broken(uint8_t *list, int cap, MoveListTail *tail) {
    if (cap < 1) return MOVE_LIST_E_CAP;
    list[0] = MOVE_LIST_BROKEN;
    memset(tail, 0, sizeof(*tail));
    tail->broken = true;
    return 1;
}

int move_list_append(uint8_t *list, int len, int cap, MoveListTail *tail, int64_t ms,
                     int type, int seat, int n_pairs, const uint8_t *pairs) {
    if (type == LOG_GAME_START) {   // a new session: a new list
        len = 0;
        memset(tail, 0, sizeof(*tail));
    }
    if (tail->broken) return len;
    const int limit = cap < MOVE_LIST_MAX ? cap : MOVE_LIST_MAX;
    uint8_t item[8 + MOVE_LIST_MAX_CARDS + 2];
    int n = 0;
    const bool atom = type == LOG_ATTACK || type == LOG_COVER || type == LOG_PASS || type == LOG_PICKUP;
    if ((atom || type == LOG_GOOD) && (seat < 0 || seat > 7)) return broken(list, cap, tail);
    if (type == LOG_GAME_START || atom) {
        if (ms < 0 || ms >= MS_LIMIT) return broken(list, cap, tail);
        if (ms != tail->ms) {
            uint64_t z = zigzag(ms - tail->ms);
            item[n++] = (uint8_t)(K_TICK << 5 | (z > 15 ? 16 : 0) | (z & 15));
            for (z >>= 4; z; z >>= 7) item[n++] = (uint8_t)((z & 0x7f) | (z > 0x7f ? 0x80 : 0));
        }
    }
    switch (type) {
        case LOG_GAME_START:
            item[n++] = K_MARK << 5 | M_START;
            break;
        case LOG_ATTACK:
        case LOG_PASS: {
            if (n_pairs < 1 || n_pairs > MOVE_LIST_MAX_CARDS) return broken(list, cap, tail);
            const int k = type == LOG_ATTACK ? K_ATTACK : K_PASS;
            item[n++] = (uint8_t)(k << 5 | (n_pairs > 3 ? 3 : n_pairs - 1) << 3 | seat);
            if (n_pairs > 3) item[n++] = (uint8_t)n_pairs;
            for (int i = 0; i < n_pairs; i++) {
                if (pairs[2 * i] > 51) return broken(list, cap, tail);
                item[n++] = pairs[2 * i];
            }
            break;
        }
        case LOG_COVER:
            if (n_pairs != 1 || pairs[0] > 51 || pairs[1] > 51) return broken(list, cap, tail);
            item[n++] = (uint8_t)(K_COVER << 5 | seat);
            item[n++] = pairs[0];
            item[n++] = pairs[1];
            break;
        case LOG_PICKUP:
            item[n++] = (uint8_t)(K_PICKUP << 5 | seat);
            break;
        case LOG_GOOD:
            item[n++] = (uint8_t)(K_GOOD << 5 | seat);
            break;
        case LOG_DISCARD:   // right after a GOOD it is the round's end; else no atom
            if (tail->last_good) item[n++] = K_MARK << 5 | M_ROUND_END;
            break;
        default:            // DRAW, DEFENDER_CHANGE, PLAYER_OUT: no atom, but a GOOD before it is done
            if (tail->last_good) item[n++] = K_MARK << 5 | M_CUT;
            break;
    }
    if (n == 0) return len;
    const int head = len == 0 ? 1 : 0;
    if (len + head + n > limit) return broken(list, cap, tail);
    if (head) list[len++] = MOVE_LIST_FORMAT;
    memcpy(list + len, item, (size_t)n);
    len += n;
    if (type == LOG_GAME_START || atom) tail->ms = ms;
    tail->last_good = type == LOG_GOOD;
    return len;
}
