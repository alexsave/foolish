/* chats.c - see chats.h. */
#include "chats.h"
#include "arenas.h"
#include "journal.h"
#include "pushes.h"

#include "rate_limits.h"
#include "fnv.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- rooms ---- */

/* "lobby" is 'l'; "g<id>" and "a<id>" are 'g' and 'a' with the id (a u32, no leading zero, not 0):
 * the kind, or 0 for a name that is none of them. */
static int room_parse(const char *room, uint32_t *id) {
    *id = 0;
    if (strcmp(room, "lobby") == 0) return 'l';
    if ((room[0] != 'g' && room[0] != 'a') || room[1] < '1' || room[1] > '9') return 0;
    uint64_t v = 0;
    for (const char *s = room + 1; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > UINT32_MAX) return 0;
    }
    *id = (uint32_t)v;
    return room[0];
}

bool cc_chat_room_ok(const char *room) {
    uint32_t id;
    switch (room_parse(room, &id)) {
    case 'l': return true;
    case 'g': return cc_live_get(id) != NULL;
    case 'a': return cc_arena_get(id) != NULL;
    default: return false;
    }
}

/* ---- the rings: a room's last lines, oldest at head once full ---- */

typedef struct ring {
    char          room[CW_ROOM];
    uint16_t      n, head, cap;
    cw_chat_line *l;
    struct ring  *next;
} ring;

static ring   **B;            /* chained, power-of-two buckets, doubled at load 1 */
static uint32_t MASK, N;
static uint64_t n_lines, n_refused, filter_ns;

static uint32_t bucket(const char *room, uint32_t mask) { return (uint32_t)fnv1a64_str(FNV64_OFFSET, room) & mask; }

static ring *ring_get(const char *room) {
    if (!B) return NULL;
    for (ring *r = B[bucket(room, MASK)]; r; r = r->next)
        if (strcmp(r->room, room) == 0) return r;
    return NULL;
}

static void grow_map(void) {
    if (N <= MASK) return;
    uint32_t mask = MASK * 2 + 1;
    ring **b = calloc((size_t)mask + 1, sizeof *b);
    if (!b) return;
    for (uint32_t i = 0; i <= MASK; i++)
        for (ring *r = B[i], *nx; r; r = nx) {
            nx = r->next;
            uint32_t h = bucket(r->room, mask);
            r->next = b[h];
            b[h] = r;
        }
    free(B);
    B = b;
    MASK = mask;
}

static ring *ring_make(const char *room) {
    if (!B) {
        if (!(B = calloc(256, sizeof *B))) return NULL;
        MASK = 255;
    }
    ring *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    memcpy(r->room, room, strlen(room) + 1);
    uint32_t h = bucket(room, MASK);
    r->next = B[h];
    B[h] = r;
    N++;
    grow_map();
    return r;
}

_Static_assert(CW_CHAT_LOG >= 4 && (CW_CHAT_LOG & (CW_CHAT_LOG - 1)) == 0, "doubling from 4 lands on the log's size");

static void ring_put(ring *r, const cw_chat_line *line) {
    if (r->n == r->cap && r->cap < CW_CHAT_LOG) {   /* rooms mostly hold a few lines: grow to the cap */
        uint16_t cap = r->cap ? (uint16_t)(r->cap * 2) : 4;
        cw_chat_line *l = realloc(r->l, cap * sizeof *l);
        if (!l) return;
        r->l = l;
        r->cap = cap;
    }
    if (r->n < r->cap) { r->l[r->n++] = *line; return; }
    r->l[r->head] = *line;   /* full: the oldest goes */
    r->head = (uint16_t)((r->head + 1) % r->cap);
}

size_t cc_chat_log(const char *room, uint8_t *out) {
    if (!cc_chat_room_ok(room)) return 0;
    const ring *r = ring_get(room);
    cw_chat_log h;
    memset(&h, 0, sizeof h);
    h.n = r ? r->n : 0;
    memcpy(out, &h, sizeof h);
    for (uint16_t i = 0; i < h.n; i++)
        memcpy(out + sizeof h + i * sizeof(cw_chat_line), &r->l[(r->head + i) % r->cap], sizeof(cw_chat_line));
    return sizeof h + (size_t)h.n * sizeof(cw_chat_line);
}

/* ---- saying a line ---- */

static int64_t ns_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

void cc_chat_mute(cc_player *p, uint32_t minutes, int64_t wall) {
    p->mute_until = minutes ? wall + (int64_t)minutes * 60000 : 0;
    memset(p->strike_at, 0, sizeof p->strike_at);
    cc_journal_player(p);
}

/* A banned word: true when it is the third within CC_CHAT_STRIKE_MS (the player is then muted). */
static bool strike(cc_player *p, int64_t now, int64_t wall) {
    memmove(p->strike_at, p->strike_at + 1, (CC_CHAT_STRIKES - 1) * sizeof *p->strike_at);
    p->strike_at[CC_CHAT_STRIKES - 1] = now;
    if (!p->strike_at[0] || now - p->strike_at[0] > CC_CHAT_STRIKE_MS) return false;   /* an empty slot is no strike, even near boot */
    cc_chat_mute(p, CC_CHAT_TIMEOUT_MS / 60000, wall);
    return true;
}

static uint32_t seconds_until(int64_t until, int64_t now) { return (uint32_t)((until - now + 999) / 1000); }

int cc_chat_post(cc_player *p, const cw_chat_req *q, int64_t now, int64_t wall, uint32_t *wait_s, int *why) {
    *wait_s = 0;
    *why = CC_CHAT_OK;
    int code = CW_E_NONE;
    char room[CW_ROOM];
    memcpy(room, q->room, CW_ROOM);
    if (room[CW_ROOM - 1]) code = CW_E_CHAT_ROOM;
    else if ((*why = cc_chat_valid(q->text, q->len)) != CC_CHAT_OK) code = CW_E_CHAT_TEXT;
    else if (!cc_chat_room_ok(room)) code = CW_E_CHAT_ROOM;
    else if (cc_name_guest(p->name)) code = CW_E_CHAT_GUEST;
    else if (p->mute_until > wall) { code = CW_E_CHAT_MUTED; *wait_s = seconds_until(p->mute_until, wall); }
    else if (p->chat_at && now - p->chat_at < CC_CHAT_FLOOR_MS) { code = CW_E_CHAT_FAST; *wait_s = 1; }
    else {
        int retry = 0;
        if (!cb_limit_take(CB_LIMIT_CHAT_USER, p->id, wall, &retry)) { code = CW_E_CHAT_FAST; *wait_s = retry > 0 ? (uint32_t)retry : 1; }
    }
    if (code != CW_E_NONE) { n_refused++; return code; }
    p->chat_at = now;   /* a refused word counts against the rate too */
    int64_t t0 = ns_now();
    bool bad = cc_chat_banned(q->text, q->len);
    filter_ns += (uint64_t)(ns_now() - t0);
    if (bad) {
        n_refused++;
        if (!strike(p, now, wall)) return CW_E_CHAT_WORDS;
        *wait_s = seconds_until(p->mute_until, wall);
        return CW_E_CHAT_MUTED;
    }
    ring *r = ring_get(room);
    if (!r && !(r = ring_make(room))) return CW_E_BUSY;
    cw_chat_line line;
    memset(&line, 0, sizeof line);
    line.v = CW_V;
    line.kind = CW_P_CHAT;
    line.len = q->len;
    memcpy(line.room, room, CW_ROOM);
    line.at = wall;
    memcpy(line.name, p->name, strnlen(p->name, CW_NAME));
    memcpy(line.text, q->text, q->len);
    ring_put(r, &line);
    n_lines++;
    cc_push_room(room, (const uint8_t *)&line, sizeof line);
    return CW_E_NONE;
}

/* ---- life ---- */

static void ring_free(ring *r) {
    free(r->l);
    free(r);
}

void cc_chat_sweep(void) {
    for (uint32_t i = 0; B && i <= MASK; i++)
        for (ring **pp = &B[i]; *pp;) {
            ring *r = *pp;
            if (!cc_chat_room_ok(r->room)) { *pp = r->next; N--; ring_free(r); }
            else pp = &r->next;
        }
}

void cc_chat_free(void) {
    for (uint32_t i = 0; B && i <= MASK; i++)
        for (ring *r = B[i], *nx; r; r = nx) { nx = r->next; ring_free(r); }
    free(B);
    B = NULL;
    MASK = N = 0;
    n_lines = n_refused = filter_ns = 0;
}

void cc_chat_counts(uint64_t *lines, uint64_t *refused, uint64_t *rooms, uint64_t *fns) {
    *lines = n_lines; *refused = n_refused; *rooms = N; *fns = filter_ns;
}
