/* puzzles.c - see puzzles.h. The puzzles sit in an array in the file's order; an order by rating
 * (kept sorted as ratings move: a rating moves a little, so a puzzle moves a few places) finds the
 * ones near a player's rating, and an open-addressed map finds a puzzle by id. 128 bytes a puzzle
 * plus 40 of rating, order and map: 100,000 puzzles are 17 MB. */
#include "puzzles.h"
#include "api.h"
#include "journal.h"

#include "util/err.h"
#include "util/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    cp_puzzle *p;
    g2_player *r;
    uint32_t  *plays;
    uint32_t  *order;   /* puzzle indices by rating */
    uint32_t  *pos;     /* a puzzle's place in order */
    uint32_t  *map;     /* 1 + index, by id; 0 empty */
    uint32_t   n, mask;
} Z;

/* A puzzle's id: its position and line hashed (FNV-1a), never 0. */
static uint32_t pz_id(const cp_puzzle *p) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof p->fen && p->fen[i]; i++) h = (h ^ (uint8_t)p->fen[i]) * 16777619u;
    for (int i = 0; i < p->n; i++) h = (h ^ p->moves[i]) * 16777619u;
    return h ? h : 1;
}

static uint32_t mix(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16); }

static int64_t find(uint32_t id) {
    if (!Z.map) return -1;
    for (uint32_t k = mix(id) & Z.mask;; k = (k + 1) & Z.mask) {
        if (!Z.map[k]) return -1;
        if (Z.p[Z.map[k] - 1].id == id) return Z.map[k] - 1;
    }
}

static int by_rating(const void *a, const void *b) {
    double x = Z.r[*(const uint32_t *)a].rating, y = Z.r[*(const uint32_t *)b].rating;
    return x < y ? -1 : x > y;
}

void cc_puzzles_free(void) {
    free(Z.p); free(Z.r); free(Z.plays); free(Z.order); free(Z.pos); free(Z.map);
    memset(&Z, 0, sizeof Z);
}

int cc_puzzles_load(const char *path, char *err, size_t cap) {
    cc_puzzles_free();
    if (!path || !*path) return CB_OK;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, cap, "c_chess: CC_PUZZLES %s: %s", path, strerror(errno)); return CB_E_INVALID; }
    char magic[8];
    uint32_t count = 0;
    int rc = CB_E_INVALID;
    if (fread(magic, 8, 1, f) != 1 || memcmp(magic, PUZZLE_FILE_MAGIC, 8) != 0 || fread(&count, 4, 1, f) != 1 || count > (1u << 24)) {
        snprintf(err, cap, "c_chess: CC_PUZZLES %s is not a puzzle file", path);
        goto out;
    }
    uint32_t slots = 16;
    while (slots < count * 2) slots <<= 1;
    Z.p = malloc((size_t)count * sizeof *Z.p + 1);
    Z.r = malloc((size_t)count * sizeof *Z.r + 1);
    Z.plays = calloc((size_t)count + 1, sizeof *Z.plays);
    Z.order = malloc((size_t)count * sizeof *Z.order + 1);
    Z.pos = malloc((size_t)count * sizeof *Z.pos + 1);
    Z.map = calloc(slots, sizeof *Z.map);
    Z.mask = slots - 1;
    if (!Z.p || !Z.r || !Z.plays || !Z.order || !Z.pos || !Z.map) { snprintf(err, cap, "c_chess: no memory for %u puzzles", count); rc = CB_E_NOMEM; goto out; }
    uint32_t bad = 0, dup = 0;
    for (uint32_t i = 0; i < count; i++) {
        cp_puzzle q;
        if (fread(&q, sizeof q, 1, f) != 1) { snprintf(err, cap, "c_chess: CC_PUZZLES %s is short", path); goto out; }
        cp_solve s;
        if (cp_begin(&s, &q) != 0) { bad++; continue; }   /* the whole line must be legal */
        q.id = pz_id(&q);
        if (find(q.id) >= 0) { dup++; continue; }
        uint32_t k = mix(q.id) & Z.mask;
        while (Z.map[k]) k = (k + 1) & Z.mask;
        Z.map[k] = Z.n + 1;
        Z.r[Z.n] = (g2_player){ q.rating, q.rd ? q.rd : CC_RD_START, CC_VOL_START };
        Z.p[Z.n++] = q;
    }
    for (uint32_t i = 0; i < Z.n; i++) Z.order[i] = i;
    qsort(Z.order, Z.n, sizeof *Z.order, by_rating);
    for (uint32_t i = 0; i < Z.n; i++) Z.pos[Z.order[i]] = i;
    CB_INFO("cc", "puzzles: %u from %s (%u refused, %u repeated)", Z.n, path, bad, dup);
    rc = CB_OK;
out:
    fclose(f);
    if (rc != CB_OK) cc_puzzles_free();
    return rc;
}

uint32_t cc_puzzles_count(void) { return Z.n; }

bool cc_puzzles_ordered(void) {
    for (uint32_t i = 0; i + 1 < Z.n; i++)
        if (Z.r[Z.order[i]].rating > Z.r[Z.order[i + 1]].rating || Z.pos[Z.order[i]] != i) return false;
    return true;
}

/* After puzzle i's rating moved: back into its place in the order. */
static void resort(uint32_t i) {
    uint32_t at = Z.pos[i];
    while (at > 0 && Z.r[Z.order[at - 1]].rating > Z.r[i].rating) {
        Z.order[at] = Z.order[at - 1]; Z.pos[Z.order[at]] = at; at--;
    }
    while (at + 1 < Z.n && Z.r[Z.order[at + 1]].rating < Z.r[i].rating) {
        Z.order[at] = Z.order[at + 1]; Z.pos[Z.order[at]] = at; at++;
    }
    Z.order[at] = i;
    Z.pos[i] = at;
}

static void set_rating(uint32_t i, const g2_player *r) {
    Z.r[i] = *r;
    Z.p[i].rating = (int16_t)cc_rating_int(r);
    Z.p[i].rd = (uint16_t)(r->deviation + 0.5);
    resort(i);
}

void cc_puzzle_recovered(uint32_t id, const g2_player *r, uint32_t plays) {
    int64_t i = find(id);
    if (i < 0) return;
    Z.plays[i] = plays;
    set_rating((uint32_t)i, r);
}

static bool seen(const cc_player *p, uint32_t id) {
    for (int i = 0; i < 16; i++) if (p->pz_seen[i] == id) return true;
    return false;
}

#define PICK_SPREAD 24   /* a pick is one of the puzzles this many places either side of the rating */

const cp_puzzle *cc_puzzle_pick(cc_player *p, uint32_t *plays, uint32_t seed) {
    if (!Z.n) return NULL;
    uint32_t lo = 0, hi = Z.n;   /* the first puzzle rated at or above the player */
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (Z.r[Z.order[mid]].rating < p->pz.rating) lo = mid + 1; else hi = mid;
    }
    int64_t i = -1;
    for (int t = 0; t < 32 && i < 0; t++) {
        seed = mix(seed + (uint32_t)t);
        int64_t at = (int64_t)lo + (int64_t)(seed % (2 * PICK_SPREAD + 1)) - PICK_SPREAD;
        if (at < 0) at = 0;
        if (at >= Z.n) at = Z.n - 1;
        uint32_t k = Z.order[at];
        if (!seen(p, Z.p[k].id)) i = k;
    }
    if (i < 0) i = Z.order[lo < Z.n ? lo : Z.n - 1];   /* fewer puzzles than the player has seen lately */
    p->pz_open = Z.p[i].id;
    p->pz_seen[p->pz_seen_at++ & 15] = Z.p[i].id;
    *plays = Z.plays[i];
    return &Z.p[i];
}

bool cc_puzzle_result(cc_player *p, uint32_t id, bool win, cw_puzzle_done *out) {
    int64_t i = find(id);
    if (id != p->pz_open || i < 0) return false;   /* no puzzle has id 0, so none open can't match */
    p->pz_open = 0;
    out->before = (int16_t)cc_rating_int(&p->pz);
    g2_player z = Z.r[i];
    cc_rate(&p->pz, &z, win ? 1.0 : 0.0);
    set_rating((uint32_t)i, &z);
    Z.plays[i]++;
    p->pz_tried++;
    if (win) p->pz_solved++;
    out->after = (int16_t)cc_rating_int(&p->pz);
    out->puzzle = Z.p[i].rating;
    out->rd = (uint16_t)(p->pz.deviation + 0.5);
    out->tried = p->pz_tried;
    out->solved = p->pz_solved;
    cc_journal_player(p);
    cc_journal_puzzle(id, &z, Z.plays[i]);
    return true;
}

/* ---- the routes ---- */

static void h_next(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!cc_api_caller(ctx, req, res, &u)) return;   /* the token is checked outside the lock */
    cw_puzzle out;
    memset(&out, 0, sizeof out);
    pthread_mutex_lock(&cc_lock);
    cc_player *p = cc_player_of(u.id, u.username);
    const cp_puzzle *z = p ? cc_puzzle_pick(p, &out.plays, (uint32_t)cc_mono() ^ (uint32_t)(uintptr_t)p) : NULL;
    if (z) {
        out.p = *z;
        out.rating = (int16_t)cc_rating_int(&p->pz);
        out.rd = (uint16_t)(p->pz.deviation + 0.5);
        out.tried = p->pz_tried;
        out.solved = p->pz_solved;
    }
    pthread_mutex_unlock(&cc_lock);
    if (!p) { cc_api_refuse(res, 503, CW_E_BUSY, 0); return; }
    if (!z) { cc_api_refuse(res, 404, CW_E_NO_GAME, 0); return; }
    cc_api_answer(res, 200, &out, sizeof out);
}

static void h_result(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cw_puzzle_req q;
    if (req->body_len != sizeof q) { cc_api_refuse(res, 400, CW_E_BAD, 0); return; }
    memcpy(&q, req->body, sizeof q);
    cb_user u;
    if (!cc_api_caller(ctx, req, res, &u)) return;
    cw_puzzle_done out;
    memset(&out, 0, sizeof out);
    pthread_mutex_lock(&cc_lock);
    cc_player *p = cc_player_of(u.id, u.username);
    bool ok = p && cc_puzzle_result(p, q.id, q.win != 0, &out);
    pthread_mutex_unlock(&cc_lock);
    if (!p) { cc_api_refuse(res, 503, CW_E_BUSY, 0); return; }
    if (!ok) { cc_api_refuse(res, 409, CW_E_RULES, 0); return; }
    cc_api_answer(res, 200, &out, sizeof out);
}

#define ROUTE (CB_ROUTE_POOL | CB_ROUTE_INLINE | CB_ROUTE_HOME)
static const cb_route ROUTES[] = {
    { "GET", "/c/puzzle/next", ROUTE, h_next, 500 },
    { "POST", "/c/puzzle/result", ROUTE, h_result, 500 },
};
const cb_route_list cc_puzzle_routes = { ROUTES, 2 };
