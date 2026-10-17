// ask.c - the request that is not a move, and the refusal. See ask.h.
#include "ask.h"
#include "../../../shared/c/le_bytes.h"
#include <string.h>

// ---------- a cursor over the body ------------------------------------------------
//
// Both directions run off the end the same way: `bad` latches, every later step
// is a no-op, and the caller asks once at the end.

typedef struct { uint8_t *p; int at, cap; int bad; } Put;
typedef struct { const uint8_t *p; int at, len; int bad; } Get;

static void put_u8(Put *w, int v) {
    if (w->bad || w->at + 1 > w->cap) { w->bad = 1; return; }
    w->p[w->at++] = (uint8_t)v;
}

static void put_bytes(Put *w, const void *src, int n) {
    if (w->bad || w->at + n > w->cap) { w->bad = 1; return; }
    if (n > 0) memcpy(w->p + w->at, src, (size_t)n);
    w->at += n;
}

static int get_u8(Get *r) {
    if (r->bad || r->at + 1 > r->len) { r->bad = 1; return 0; }
    return r->p[r->at++];
}

// `n` bytes into a slot that holds `max`; a length past the slot is malformed.
static void get_bytes(Get *r, void *dst, int n, int max) {
    if (r->bad || n > max || r->at + n > r->len) { r->bad = 1; return; }
    if (n > 0) memcpy(dst, r->p + r->at, (size_t)n);
    r->at += n;
}

// ---------- the ask ---------------------------------------------------------------

static int kind_known(int kind) { return kind >= ASK_JOIN && kind <= ASK_BUMP; }

// The struct's own counts, each inside its array. ask_encode refuses a struct
// that fails this, and ask_decode never produces one.
static int ask_fits(const TableAsk *a) {
    if (!kind_known(a->kind)) return 0;
    if (a->gid_len == 0 || a->gid_len > ROSTER_GAME_ID_MAX) return 0;
    if (a->bot_len > ROSTER_ID_MAX || a->player_len > ROSTER_ID_MAX) return 0;
    if (a->n_idx > MAX_HAND_SIZE || a->n_order > MAX_PLAYERS) return 0;
    if (a->title_len > ASK_TITLE_MAX) return 0;
    for (int i = 0; i < a->n_order; i++) {
        if (a->order[i].len > ROSTER_ID_MAX) return 0;
    }
    return a->kind != ASK_SET_RULES || a->passing <= 1;
}

int ask_encode(const TableAsk *a, uint8_t *out, int cap) {
    if (!a || !ask_fits(a)) return ASK_E_WIRE;
    if (!out) return ASK_E_CAP;
    Put w = { out, 0, cap, 0 };
    put_u8(&w, ASK_FORMAT);
    put_u8(&w, a->kind);
    put_u8(&w, a->gid_len);
    put_bytes(&w, a->gid, a->gid_len);
    switch (a->kind) {
        case ASK_ADD_BOT:
            put_u8(&w, a->bot_len);
            put_bytes(&w, a->bot, a->bot_len);
            break;
        case ASK_EXIT:
            put_u8(&w, a->bot_len);
            put_bytes(&w, a->bot, a->bot_len);
            put_u8(&w, a->player_len);
            put_bytes(&w, a->player, a->player_len);
            break;
        case ASK_REARRANGE_HAND:
            put_u8(&w, a->n_idx);
            put_bytes(&w, a->idx, a->n_idx);
            break;
        case ASK_REARRANGE_PLAYERS:
            put_u8(&w, a->n_order);
            for (int i = 0; i < a->n_order; i++) {
                put_u8(&w, a->order[i].len);
                put_bytes(&w, a->order[i].id, a->order[i].len);
            }
            break;
        case ASK_UPDATE_NAME: {
            uint8_t n[2];
            le_put_u16(n, a->title_len);
            put_bytes(&w, n, 2);
            put_bytes(&w, a->title, a->title_len);
            break;
        }
        case ASK_SET_RULES:
            put_u8(&w, a->passing);
            break;
        default:
            break;
    }
    return w.bad ? ASK_E_CAP : w.at;
}

int ask_decode(const uint8_t *p, int len, TableAsk *out) {
    memset(out, 0, sizeof(*out));
    if (!p || len < 0) return ASK_E_WIRE;
    Get r = { p, 0, len, 0 };
    if (get_u8(&r) != ASK_FORMAT) return ASK_E_WIRE;
    out->kind = (uint8_t)get_u8(&r);
    out->gid_len = (uint8_t)get_u8(&r);
    get_bytes(&r, out->gid, out->gid_len, ROSTER_GAME_ID_MAX);
    switch (out->kind) {
        case ASK_ADD_BOT:
            out->bot_len = (uint8_t)get_u8(&r);
            get_bytes(&r, out->bot, out->bot_len, ROSTER_ID_MAX);
            break;
        case ASK_EXIT:
            out->bot_len = (uint8_t)get_u8(&r);
            get_bytes(&r, out->bot, out->bot_len, ROSTER_ID_MAX);
            out->player_len = (uint8_t)get_u8(&r);
            get_bytes(&r, out->player, out->player_len, ROSTER_ID_MAX);
            break;
        case ASK_REARRANGE_HAND:
            out->n_idx = (uint8_t)get_u8(&r);
            get_bytes(&r, out->idx, out->n_idx, MAX_HAND_SIZE);
            break;
        case ASK_REARRANGE_PLAYERS:
            out->n_order = (uint8_t)get_u8(&r);
            if (out->n_order > MAX_PLAYERS) r.bad = 1;
            for (int i = 0; !r.bad && i < out->n_order; i++) {
                out->order[i].len = (uint8_t)get_u8(&r);
                get_bytes(&r, out->order[i].id, out->order[i].len, ROSTER_ID_MAX);
            }
            break;
        case ASK_UPDATE_NAME: {
            uint8_t n[2] = { 0, 0 };
            get_bytes(&r, n, 2, 2);
            out->title_len = le_get_u16(n);
            get_bytes(&r, out->title, out->title_len, ASK_TITLE_MAX);
            break;
        }
        case ASK_SET_RULES:
            out->passing = (uint8_t)get_u8(&r);
            break;
        default:
            break;
    }
    // Read whole, and what it read is an ask: one judge for both directions.
    if (r.bad || r.at != len || !ask_fits(out)) {
        memset(out, 0, sizeof(*out));
        return ASK_E_WIRE;
    }
    return ASK_OK;
}

// ---------- the refusal -----------------------------------------------------------

static int why_known(int why) { return why >= ASK_WHY_AUTH && why <= ASK_WHY_BUSY; }

int ask_refusal_encode(const AskRefusal *r, uint8_t *out, int cap) {
    if (!r || !why_known(r->why)) return ASK_E_WIRE;
    if (!out || cap < ASK_REFUSAL_BYTES) return ASK_E_CAP;
    out[0] = ASK_REFUSAL_FORMAT;
    out[1] = r->why;
    le_put_u32(out + 2, (uint32_t)r->code);
    le_put_u32(out + 6, (uint32_t)r->detail);
    return ASK_REFUSAL_BYTES;
}

int ask_refusal_decode(const uint8_t *p, int len, AskRefusal *out) {
    memset(out, 0, sizeof(*out));
    if (!p || len != ASK_REFUSAL_BYTES || p[0] != ASK_REFUSAL_FORMAT || !why_known(p[1])) return ASK_E_WIRE;
    out->why = p[1];
    out->code = (int32_t)le_get_u32(p + 2);
    out->detail = (int32_t)le_get_u32(p + 6);
    return ASK_OK;
}
