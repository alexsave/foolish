// move_list.c - the session's move list (move_list.h): its one writer (a
// commit's chunk) and its one reader.
#include "move_list.h"
#include "game.h"
#include <stddef.h>
#include <string.h>

enum { K_MARK = 0, K_ATTACK = 1, K_COVER = 2, K_PASS = 3, K_PICKUP = 4, K_GOOD = 5, K_TICK = 6, K_BROKEN = 7 };
enum { M_START = 0, M_DISCARD = 1, M_OTHER = 2 };

#define MS_LIMIT ((int64_t)1 << 48)

static uint64_t zigzag(int64_t d) { return d < 0 ? ((uint64_t)(-(d + 1)) << 1) | 1u : (uint64_t)d << 1; }
static int64_t unzigzag(uint64_t z) { return (z & 1u) ? -(int64_t)(z >> 1) - 1 : (int64_t)(z >> 1); }

static bool timed_kind(int kind) {
    return kind == MOVE_LIST_START || kind == LOG_ATTACK || kind == LOG_COVER || kind == LOG_PASS
        || kind == LOG_PICKUP;
}

// ---- the reader ---------------------------------------------------------------------

int move_list_open(MoveListCursor *c, const uint8_t *list, int len, int64_t base_ms) {
    memset(c, 0, sizeof(*c));
    c->prev = -1;
    c->base = c->ms = base_ms;
    if (len < 0 || (len > 0 && !list)) return MOVE_LIST_E_PARSE;
    c->p = list;
    c->len = len;
    if (len == 0) return 0;
    if (list[0] != MOVE_LIST_FORMAT) return MOVE_LIST_E_PARSE;
    c->at = 1;
    return 0;
}

// The TICK whose head is h: the time it names. 0 or E_PARSE.
static int read_tick(MoveListCursor *c, uint8_t h) {
    uint64_t z = h & 15u;
    if (h & 16u) {
        uint64_t rest = 0;
        for (int shift = 0;; shift += 7) {
            if (c->at >= c->len || shift > 42) return MOVE_LIST_E_PARSE;
            const uint8_t b = c->p[c->at++];
            rest |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) {
                if (b == 0) return MOVE_LIST_E_PARSE;   // minimal: no empty last byte
                break;
            }
        }
        z |= rest << 4;
    }
    // A stamp is a u48, so an offset from any base is under 2^48 either way: whatever the
    // base, the list reads the same (the check of the bytes needs none).
    if (z >= ((uint64_t)1 << 49)) return MOVE_LIST_E_PARSE;
    c->ms = c->base + unzigzag(z);
    return 0;
}

int move_list_next(MoveListCursor *c, MoveListItem *it) {
    if (!c->p || c->at >= c->len) return 0;
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
            } else if (low == M_DISCARD) {
                it->kind = c->prev == LOG_GOOD ? MOVE_LIST_ROUND_END : MOVE_LIST_OTHER;
            } else if (low == M_OTHER) {
                it->kind = MOVE_LIST_OTHER;
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
        case K_BROKEN:
            return low || ticked ? MOVE_LIST_E_PARSE : MOVE_LIST_E_BROKEN;
        default:   // a second TICK
            return MOVE_LIST_E_PARSE;
    }
    if (c->prev == -1 && it->kind != MOVE_LIST_START) return MOVE_LIST_E_PARSE;   // a session begins at its deal
    if (c->at + it->n > c->len) return MOVE_LIST_E_PARSE;
    for (int i = 0; i < it->n; i++) {
        it->cards[i] = c->p[c->at++];
        if (it->cards[i] > 51) return MOVE_LIST_E_PARSE;
    }
    it->timed = timed_kind(it->kind);
    if (ticked && !it->timed) return MOVE_LIST_E_PARSE;   // a TICK times the item after it
    if (it->kind == MOVE_LIST_START && !ticked) return MOVE_LIST_E_PARSE;   // the deal is always timed
    it->ms = c->ms;
    c->prev = it->kind;
    return 1;
}

int move_list_check(const uint8_t *list, int len) {
    MoveListCursor c;
    MoveListItem it;
    const int o = move_list_open(&c, list, len, 0);
    if (o < 0) return o;
    int r;
    while ((r = move_list_next(&c, &it)) > 0) {}
    return r;
}

// ---- the writer: one commit's chunk -----------------------------------------------------

void move_list_chunk_begin(MoveListChunk *w, uint8_t *out, int cap, int64_t base_ms) {
    memset(w, 0, sizeof(*w));
    w->out = out;
    w->cap = cap;
    w->base = base_ms;
    w->prev = -1;
}

static int put(MoveListChunk *w, const uint8_t *b, int n) {
    if (w->len + n > w->cap) return MOVE_LIST_E_CAP;
    memcpy(w->out + w->len, b, (size_t)n);
    w->len += n;
    return 0;
}

int move_list_chunk_put(MoveListChunk *w, int64_t ms, int type, int seat, int n_pairs, const uint8_t *pairs) {
    const bool first = !w->any, after_good = w->prev == LOG_GOOD;
    w->any = true;
    w->prev = type;
    if (type == LOG_GAME_START) {   // a new session: what came before is the last session's
        w->len = 0;
        w->ticked = w->broken = false;
    }
    if (w->broken) return 0;
    uint8_t item[12 + MOVE_LIST_MAX_CARDS];
    int n = 0;
    const bool atom = type == LOG_ATTACK || type == LOG_COVER || type == LOG_PASS || type == LOG_PICKUP;
    bool spelled = true;
    if ((atom || type == LOG_GOOD) && (seat < 0 || seat > 7)) spelled = false;
    if (type == LOG_ATTACK || type == LOG_PASS) {
        if (n_pairs < 1 || n_pairs > MOVE_LIST_MAX_CARDS) spelled = false;
        for (int i = 0; spelled && i < n_pairs; i++) if (pairs[2 * i] > 51) spelled = false;
    }
    if (type == LOG_COVER && (n_pairs != 1 || pairs[0] > 51 || pairs[1] > 51)) spelled = false;
    if ((type == LOG_GAME_START || atom) && (ms < 0 || ms >= MS_LIMIT || w->base < 0 || w->base >= MS_LIMIT)) spelled = false;
    if (!spelled) {
        w->broken = true;
        const uint8_t b = K_BROKEN << 5;
        return put(w, &b, 1);
    }
    if (type == LOG_GAME_START) item[n++] = MOVE_LIST_FORMAT;
    // One TICK a commit: its records share one millisecond (a record that did not would get its own).
    if ((type == LOG_GAME_START || atom) && (!w->ticked || ms != w->tick_ms)) {
        uint64_t z = zigzag(ms - w->base);
        item[n++] = (uint8_t)(K_TICK << 5 | (z > 15 ? 16 : 0) | (z & 15));
        for (z >>= 4; z; z >>= 7) item[n++] = (uint8_t)((z & 0x7f) | (z > 0x7f ? 0x80 : 0));
        w->ticked = true;
        w->tick_ms = ms;
    }
    switch (type) {
        case LOG_GAME_START:
            item[n++] = K_MARK << 5 | M_START;
            break;
        case LOG_ATTACK:
        case LOG_PASS: {
            const int k = type == LOG_ATTACK ? K_ATTACK : K_PASS;
            item[n++] = (uint8_t)(k << 5 | (n_pairs > 3 ? 3 : n_pairs - 1) << 3 | seat);
            if (n_pairs > 3) item[n++] = (uint8_t)n_pairs;
            for (int i = 0; i < n_pairs; i++) item[n++] = pairs[2 * i];
            break;
        }
        case LOG_COVER:
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
        case LOG_DISCARD:   // a round's end right after a GOOD; the chunk's first, the reader decides
            if (after_good || first) item[n++] = K_MARK << 5 | M_DISCARD;
            break;
        default:            // DRAW, DEFENDER_CHANGE, PLAYER_OUT: a GOOD before it is superseded
            if (after_good || first) item[n++] = K_MARK << 5 | M_OTHER;
            break;
    }
    return n ? put(w, item, n) : 0;
}
