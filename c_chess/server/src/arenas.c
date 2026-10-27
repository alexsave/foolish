/* arenas.c - see arenas.h. */
#include "arenas.h"
#include "ai.h"
#include "journal.h"
#include "pushes.h"

#include "util/clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cc_arena *ARENAS[CC_ARENA_SLOTS];
static uint32_t next_arena_id = 1;
static bool schedule_on = true;
static uint64_t n_ticks, tick_us, tick_max_us, n_pushes, push_bytes;

void cc_arena_schedule(bool on) { schedule_on = on; }

void cc_arena_counts(uint64_t *ticks, uint64_t *t_us, uint64_t *t_max, uint64_t *pushes, uint64_t *bytes) {
    *ticks = n_ticks; *t_us = tick_us; *t_max = tick_max_us; *pushes = n_pushes; *bytes = push_bytes;
}

cc_arena *cc_arena_get(uint32_t id) {
    for (int i = 0; i < CC_ARENA_SLOTS; i++)
        if (ARENAS[i] && ARENAS[i]->id == id) return ARENAS[i];
    return NULL;
}

static void arena_free(cc_arena *a) {
    free(a->pl);
    free(a->rank);
    free(a->sent);
    free(a);
}

/* A free slot: an empty one, else the finished arena that ended first (it is in the journal). */
static int slot_for_new(void) {
    int best = -1;
    for (int i = 0; i < CC_ARENA_SLOTS; i++) {
        if (!ARENAS[i]) return i;
        const cc_arena *a = ARENAS[i];
        if (a->status == CC_ARENA_FINISHED && !a->playing && (best < 0 || a->ends_at < ARENAS[best]->ends_at)) best = i;
    }
    if (best >= 0) { arena_free(ARENAS[best]); ARENAS[best] = NULL; }
    return best;
}

static int live_arenas(void) {
    int n = 0;
    for (int i = 0; i < CC_ARENA_SLOTS; i++) n += ARENAS[i] && ARENAS[i]->status != CC_ARENA_FINISHED;
    return n;
}

static bool grow(cc_arena *a) {
    if (a->n < a->cap) return true;
    uint32_t c = a->cap ? a->cap * 2 : 16;
    cc_arena_player *pl = realloc(a->pl, c * sizeof *pl);
    if (!pl) return false;
    a->pl = pl;
    uint32_t *rk = realloc(a->rank, c * sizeof *rk);
    if (!rk) return false;
    a->rank = rk;
    a->cap = c;
    return true;
}

static cc_arena_player *find(cc_arena *a, const cc_player *p) {
    for (uint32_t i = 0; i < a->n; i++)
        if (a->pl[i].p == p) return &a->pl[i];
    return NULL;
}

static void changed(cc_arena *a) { a->dirty = a->unsorted = 1; }

/* ---- making one ---- */

static cc_arena *make(const cc_player *by, const char *name, size_t nn, const cw_seek *tc, int64_t starts_at, int64_t ends_at) {
    int s = slot_for_new();
    if (s < 0) return NULL;
    cc_arena *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->id = next_arena_id++;
    a->status = CC_ARENA_UPCOMING;
    a->tc = *tc;
    a->speed = (uint8_t)cc_speed(tc->base_s, tc->inc_s);
    memcpy(a->name, name, nn);
    if (by) memcpy(a->creator, by->id, sizeof a->creator);
    a->starts_at = starts_at;
    a->ends_at = ends_at;
    changed(a);
    ARENAS[s] = a;
    cc_journal_arena(a);
    return a;
}

int cc_arena_create(cc_player *by, const cw_arena_req *q, int64_t wall, cc_arena **out) {
    *out = NULL;
    size_t nn = strnlen(q->name, CW_ARENA_NAME);
    if (by && strncmp(by->name, "guest-", 6) == 0) return CW_E_ARENA_GUEST;
    if (!cc_arena_name_ok(q->name, nn)) return CW_E_ARENA_NAME;
    cw_seek tc = { q->base_s, q->inc_s, q->rated };
    if (!cc_tc_ok(&tc) || q->duration_s < CC_ARENA_DURATION_MIN || q->duration_s > CC_ARENA_DURATION_MAX || q->start_in_s > CC_ARENA_START_MAX)
        return CW_E_BAD;
    if (by) {
        int mine = 0;
        for (int i = 0; i < CC_ARENA_SLOTS; i++)
            mine += ARENAS[i] && ARENAS[i]->status == CC_ARENA_UPCOMING && strcmp(ARENAS[i]->creator, by->id) == 0;
        if (mine >= CC_ARENA_PER_CREATOR) return CW_E_ARENA_TOO_MANY;
    }
    if (live_arenas() >= CC_ARENAS_MAX) return CW_E_BUSY;
    int64_t starts = wall + (int64_t)q->start_in_s * 1000;
    *out = make(by, q->name, nn, &tc, starts, starts + (int64_t)q->duration_s * 1000);
    return *out ? CW_E_NONE : CW_E_BUSY;
}

/* ---- joining and leaving ---- */

int cc_arena_join(cc_arena *a, cc_player *p) {
    if (a->status == CC_ARENA_FINISHED) return CW_E_ARENA_OVER;
    cc_arena_player *ap = find(a, p);
    if (ap && !ap->withdrawn) return CW_E_ARENA_JOINED;
    if (ap) ap->withdrawn = 0;
    else {
        if (a->n >= CC_ARENA_PLAYERS_MAX || !grow(a)) return CW_E_ARENA_TOO_MANY;
        ap = &a->pl[a->n++];
        memset(ap, 0, sizeof *ap);
        ap->p = p;
        ap->order = a->joins++;
    }
    changed(a);
    cc_journal_arena_player(a, ap);
    return CW_E_NONE;
}

/* Before the start a player leaves the arena; after it, a withdrawn player stays in the standings
 * and is not paired (and may come back). */
int cc_arena_withdraw(cc_arena *a, cc_player *p) {
    cc_arena_player *ap = find(a, p);
    if (!ap || ap->withdrawn) return CW_E_ARENA_NOT_JOINED;
    changed(a);
    if (a->status == CC_ARENA_UPCOMING) {
        cc_journal_arena_player_gone(a, p);
        *ap = a->pl[--a->n];
        return CW_E_NONE;
    }
    ap->withdrawn = 1;
    cc_journal_arena_player(a, ap);
    return CW_E_NONE;
}

/* ---- ranking ---- */

static const cc_arena *SORT_A;   /* the arena qsort's comparisons read (under cc_lock) */

static int rating_of(const cc_arena *a, const cc_arena_player *ap) { return cc_rating_int(&ap->p->r[a->speed]); }

static int by_standing(const void *x, const void *y) {
    const cc_arena_player *a = &SORT_A->pl[*(const uint32_t *)x], *b = &SORT_A->pl[*(const uint32_t *)y];
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    int ra = rating_of(SORT_A, a), rb = rating_of(SORT_A, b);
    if (ra != rb) return ra > rb ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

static int by_rating(const void *x, const void *y) {
    const cc_arena_player *a = &SORT_A->pl[*(const uint32_t *)x], *b = &SORT_A->pl[*(const uint32_t *)y];
    int ra = rating_of(SORT_A, a), rb = rating_of(SORT_A, b);
    if (ra != rb) return ra > rb ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

void cc_arena_rank(cc_arena *a) {
    if (!a->unsorted) return;
    for (uint32_t i = 0; i < a->n; i++) a->rank[i] = i;
    SORT_A = a;
    qsort(a->rank, a->n, sizeof *a->rank, by_standing);
    for (uint32_t r = 0; r < a->n; r++) a->pl[a->rank[r]].rank = (uint16_t)(r + 1 > 0xffff ? 0xffff : r + 1);
    a->unsorted = 0;
}

/* ---- pairing ---- */

static uint32_t *FREE, *PAIR, *LEFT;   /* the pass's free players, its pairs, those left (indices into pl) */
static uint32_t free_cap;
#define TAKEN UINT32_MAX

static void start(cc_arena *a, cc_arena_player *x, cc_arena_player *y, int64_t now) {
    /* white to whoever has had it less often; between equals, the higher rated on an even game */
    int dx = x->white - x->black, dy = y->white - y->black;
    bool x_white = dx < dy || (dx == dy && (a->games & 1) == 0);
    cc_arena_player *w = x_white ? x : y, *b = x_white ? y : x;
    cc_seek_cancel(w->p, now);   /* paired here: a seek of theirs would pair them twice */
    cc_seek_cancel(b->p, now);
    cc_live *l = cc_live_start_in(w->p, b->p, &a->tc, a->id, now);
    if (!l) return;
    w->white++;
    b->black++;
    w->game = b->game = l->id;
    w->last = b->p;
    b->last = w->p;
    a->games++;
    a->playing += 2;
    a->dirty = 1;
}

/* Whether x and y did not meet in their last arena games. */
static bool fresh(const cc_arena_player *x, const cc_arena_player *y) { return x->last != y->p && y->last != x->p; }

static void pair(cc_arena *a, int64_t now) {
    if (a->n > free_cap) {
        uint32_t *f = realloc(FREE, a->cap * sizeof *f), *p = f ? realloc(PAIR, a->cap * sizeof *p) : NULL, *l = p ? realloc(LEFT, a->cap * sizeof *l) : NULL;
        if (f) FREE = f;
        if (p) PAIR = p;
        if (!l) return;
        LEFT = l;
        free_cap = a->cap;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        const cc_arena_player *ap = &a->pl[i];
        if (!ap->withdrawn && !ap->game && !ap->p->playing && now >= ap->rest_until && !cc_ai_level(ap->p)) FREE[n++] = i;
    }
    if (n < 2) return;
    if (n & 1) {
        /* an odd one waits: whoever has played the most, then finished last, then is rated lowest
         * (not always the lowest rated, who would otherwise wait every pass) */
        uint32_t out = 0;
        for (uint32_t i = 1; i < n; i++) {
            const cc_arena_player *x = &a->pl[FREE[i]], *o = &a->pl[FREE[out]];
            if (x->games != o->games ? x->games > o->games
                : x->rest_until != o->rest_until ? x->rest_until > o->rest_until : rating_of(a, x) < rating_of(a, o)) out = i;
        }
        FREE[out] = FREE[--n];
    }
    SORT_A = a;
    qsort(FREE, n, sizeof *FREE, by_rating);
    /* from the top, each with the nearest below it that it did not just play (two alone may) */
    uint32_t np = 0, nl = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (FREE[i] == TAKEN) continue;
        const cc_arena_player *x = &a->pl[FREE[i]];
        uint32_t j = TAKEN;
        for (uint32_t k = i + 1; k < n && j == TAKEN; k++)
            if (FREE[k] != TAKEN && (n == 2 || fresh(x, &a->pl[FREE[k]]))) j = k;
        if (j == TAKEN) LEFT[nl++] = FREE[i];
        else { PAIR[2 * np] = FREE[i]; PAIR[2 * np + 1] = FREE[j]; np++; FREE[j] = TAKEN; }
        FREE[i] = TAKEN;
    }
    /* those left came in twos that just met (any other two would have paired): each two trades
     * partners with the nearest pair above it where both new games are fresh */
    for (uint32_t l = 0; l + 1 < nl; l += 2) {
        uint32_t u = LEFT[l], v = LEFT[l + 1];
        for (uint32_t k = np; k-- > 0;) {
            const cc_arena_player *p = &a->pl[PAIR[2 * k]], *q = &a->pl[PAIR[2 * k + 1]];
            if (!(fresh(p, &a->pl[u]) && fresh(q, &a->pl[v]))) { uint32_t t = u; u = v; v = t; }
            if (!(fresh(p, &a->pl[u]) && fresh(q, &a->pl[v]))) continue;
            PAIR[2 * np] = PAIR[2 * k + 1];
            PAIR[2 * np + 1] = v;
            PAIR[2 * k + 1] = u;
            np++;
            break;
        }
    }
    uint32_t games = a->games;
    for (uint32_t k = 0; k < np; k++) start(a, &a->pl[PAIR[2 * k]], &a->pl[PAIR[2 * k + 1]], now);
    if (a->games != games) cc_journal_arena(a);   /* its count of games, once a pass that made some */
}

/* ---- a game ends ---- */

/* The rows of an arena that has ended and has no game left: its final standings, journaled once. */
static void final_rows(cc_arena *a) {
    if (a->final_written) return;
    a->final_written = 1;
    cc_arena_rank(a);
    for (uint32_t i = 0; i < a->n; i++) cc_journal_arena_player(a, &a->pl[i]);
    cc_journal_arena(a);
}

void cc_arena_game_over(const cc_live *l, int64_t now) {
    cc_arena *a = cc_arena_get(l->arena);
    if (!a) return;
    const cc_game *g = &l->g;
    for (int c = 0; c < 2; c++) {
        cc_arena_player *ap = find(a, l->p[c]);
        if (!ap || ap->game != l->id) continue;
        ap->game = 0;
        if (a->playing) a->playing--;
        ap->rest_until = now + CC_ARENA_REST_MS;
        if (g->status == CC_ABORTED) {
            /* only the first-move clock aborts an arena game (api.c refuses the abort): whoever did
             * not move is not there, and is withdrawn until they come back */
            if (g->pos.stm == c) ap->withdrawn = 1;
        } else {
            int r = g->winner == c ? CC_ARENA_WIN : g->winner == CC_NO_WINNER ? CC_ARENA_DRAW : CC_ARENA_LOSS;
            ap->score = (uint16_t)(ap->score + cc_arena_points(r, ap->streak, g->ply));
            ap->streak = (uint8_t)cc_arena_streak(r, ap->streak);
            ap->games++;
        }
        cc_journal_arena_player(a, ap);
    }
    changed(a);
    if (a->status == CC_ARENA_FINISHED && !a->playing) final_rows(a);
}

/* ---- frames ---- */

static void copy_name(char *dst, const char *src, size_t cap) { memcpy(dst, src, strnlen(src, cap)); }

size_t cc_arena_fill(cc_arena *a, const cc_player *me, int64_t wall, uint8_t *out) {
    cc_arena_rank(a);
    cw_arena_hdr h;
    memset(&h, 0, sizeof h);
    h.v = CW_V;
    h.kind = CW_P_ARENA;
    h.status = a->status;
    h.speed = a->speed;
    h.id = a->id;
    h.base_s = a->tc.base_s;
    h.inc_s = a->tc.inc_s;
    h.rated = a->tc.rated;
    h.n_players = (uint16_t)(a->n > 0xffff ? 0xffff : a->n);
    h.playing = a->playing;
    h.games = a->games;
    h.now = wall;
    h.starts_at = a->starts_at;
    h.ends_at = a->ends_at;
    copy_name(h.name, a->name, CW_ARENA_NAME);
    const cc_player *by = a->creator[0] ? cc_player_get(a->creator) : NULL;
    if (by) copy_name(h.by, by->name, CW_NAME);
    uint32_t rows = a->n < CW_ARENA_ROWS ? a->n : CW_ARENA_ROWS;
    h.n_rows = (uint16_t)rows;
    const cc_arena_player *mine = me ? find(a, me) : NULL;
    if (mine) {
        h.me_in = mine->withdrawn ? CW_ARENA_WITHDRAWN : CW_ARENA_IN;
        h.me_fire = cc_arena_on_fire(mine->streak);
        h.me_rank = mine->rank;
        h.me_score = mine->score;
        h.me_games = mine->games;
    }
    memcpy(out, &h, sizeof h);
    for (uint32_t r = 0; r < rows; r++) {
        const cc_arena_player *ap = &a->pl[a->rank[r]];
        cw_arena_row w;
        memset(&w, 0, sizeof w);
        copy_name(w.name, ap->p->name, CW_NAME);
        w.score = ap->score;
        w.games = ap->games;
        w.rank = ap->rank;
        w.rating = (int16_t)rating_of(a, ap);
        w.fire = cc_arena_on_fire(ap->streak);
        w.withdrawn = ap->withdrawn;
        w.playing = ap->game != 0;
        memcpy(out + sizeof h + r * sizeof w, &w, sizeof w);
    }
    return sizeof h + rows * sizeof(cw_arena_row);
}

static int list_order(const void *x, const void *y) {
    const cc_arena *a = *(cc_arena *const *)x, *b = *(cc_arena *const *)y;
    static const int group[] = { [CC_ARENA_RUNNING] = 0, [CC_ARENA_UPCOMING] = 1, [CC_ARENA_FINISHED] = 2 };
    if (a->status != b->status) return group[a->status] - group[b->status];
    if (a->status == CC_ARENA_FINISHED) return a->ends_at > b->ends_at ? -1 : a->ends_at < b->ends_at;
    if (a->starts_at != b->starts_at) return a->starts_at < b->starts_at ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id;
}

size_t cc_arena_list_fill(cw_arena_list *out, int64_t wall) {
    cc_arena *all[CC_ARENA_SLOTS];
    int n = 0;
    for (int i = 0; i < CC_ARENA_SLOTS; i++) if (ARENAS[i]) all[n++] = ARENAS[i];
    qsort(all, (size_t)n, sizeof *all, list_order);
    memset(out, 0, 16);
    out->n = (uint16_t)n;
    out->now = wall;
    for (int i = 0; i < n; i++) {
        cc_arena *a = all[i];
        cw_arena_item *r = &out->rows[i];
        memset(r, 0, sizeof *r);
        r->id = a->id;
        r->status = a->status;
        r->speed = a->speed;
        r->base_s = a->tc.base_s;
        r->inc_s = a->tc.inc_s;
        r->rated = a->tc.rated;
        r->n_players = (uint16_t)(a->n > 0xffff ? 0xffff : a->n);
        r->starts_at = a->starts_at;
        r->ends_at = a->ends_at;
        copy_name(r->name, a->name, CW_ARENA_NAME);
        if (a->status == CC_ARENA_FINISHED && a->n) {
            cc_arena_rank(a);
            copy_name(r->winner, a->pl[a->rank[0]].p->name, CW_NAME);
        }
    }
    return 16 + (size_t)n * sizeof(cw_arena_item);
}

static uint8_t FRAME[CW_ARENA_FRAME_MAX];   /* under cc_lock */

static void push(cc_arena *a, int64_t wall) {
    a->dirty = 0;
    size_t n = cc_arena_fill(a, NULL, 0, FRAME);   /* `now` zero: compared without it */
    if (a->sent && a->sent_len == n && memcmp(a->sent, FRAME, n) == 0) return;
    if (!a->sent) a->sent = malloc(CW_ARENA_FRAME_MAX);
    if (a->sent) { memcpy(a->sent, FRAME, n); a->sent_len = (uint32_t)n; }
    ((cw_arena_hdr *)(void *)FRAME)->now = wall;
    char room[16];
    snprintf(room, sizeof room, "a%u", a->id);
    cc_push_room(room, FRAME, n);
    n_pushes++;
    push_bytes += n;
}

/* ---- the schedule ---- */

static const struct { uint16_t base_s; uint8_t inc_s; uint16_t minutes; const char *name; } HOURLY[] = {
    { 60, 0, 27, "Hourly Bullet" },
    { 180, 0, 57, "Hourly Blitz" },
    { 600, 0, 57, "Hourly Rapid" },
};

static void schedule(int64_t wall) {
    for (size_t k = 0; k < sizeof HOURLY / sizeof *HOURLY; k++) {
        bool have = false;
        for (int i = 0; i < CC_ARENA_SLOTS && !have; i++) {
            const cc_arena *a = ARENAS[i];
            have = a && !a->creator[0] && a->status == CC_ARENA_UPCOMING && a->tc.base_s == HOURLY[k].base_s && a->tc.inc_s == HOURLY[k].inc_s;
        }
        if (have || live_arenas() >= CC_ARENAS_MAX) continue;
        int64_t hour = (wall / 3600000 + 1) * 3600000;
        cw_seek tc = { HOURLY[k].base_s, HOURLY[k].inc_s, 1 };
        make(NULL, HOURLY[k].name, strlen(HOURLY[k].name), &tc, hour, hour + (int64_t)HOURLY[k].minutes * 60000);
    }
}

/* ---- the clock thread's pass ---- */

void cc_arena_tick(int64_t now, int64_t wall) {
    int64_t t0 = cb_mono_us();
    for (int i = 0; i < CC_ARENA_SLOTS; i++) {
        cc_arena *a = ARENAS[i];
        if (!a) continue;
        if (a->status == CC_ARENA_UPCOMING && wall >= a->starts_at) {
            a->status = CC_ARENA_RUNNING;
            changed(a);
            cc_journal_arena(a);
        }
        if (a->status == CC_ARENA_RUNNING && wall >= a->ends_at) {
            a->status = CC_ARENA_FINISHED;   /* no more pairings; the games in play still count */
            changed(a);
            cc_journal_arena(a);
            if (!a->playing) final_rows(a);
        }
        if (a->status == CC_ARENA_RUNNING) pair(a, now);
        if (a->dirty) push(a, wall);
        if (a->status == CC_ARENA_FINISHED && !a->playing && wall >= a->ends_at + CC_ARENA_KEEP_MS) {
            arena_free(a);   /* it reads back from the journal (journal.c cc_db_arena) */
            ARENAS[i] = NULL;
        }
    }
    if (schedule_on) schedule(wall);   /* after the starts: the hour's arena has just begun, the next is made */
    uint64_t t = (uint64_t)(cb_mono_us() - t0);
    n_ticks++;
    tick_us += t;
    if (t > tick_max_us) tick_max_us = t;
}

/* ---- life and recovery ---- */

void cc_arena_free(void) {
    for (int i = 0; i < CC_ARENA_SLOTS; i++) if (ARENAS[i]) { arena_free(ARENAS[i]); ARENAS[i] = NULL; }
    free(FREE);
    free(PAIR);
    free(LEFT);
    FREE = PAIR = LEFT = NULL;
    free_cap = 0;
    next_arena_id = 1;
    n_ticks = tick_us = tick_max_us = n_pushes = push_bytes = 0;
}

void cc_arena_next_id(uint32_t max) { if (max >= next_arena_id) next_arena_id = max + 1; }

cc_arena *cc_arena_recovered(const cc_arena *in) {
    int s = slot_for_new();
    if (s < 0) return NULL;
    cc_arena *a = malloc(sizeof *a);
    if (!a) return NULL;
    *a = *in;
    a->pl = NULL;
    a->rank = NULL;
    a->sent = NULL;
    a->n = a->cap = a->joins = 0;
    a->playing = 0;
    changed(a);
    ARENAS[s] = a;
    cc_arena_next_id(a->id);
    return a;
}

void cc_arena_player_recovered(cc_arena *a, const cc_arena_player *in) {
    if (!grow(a)) return;
    cc_arena_player *ap = &a->pl[a->n++];
    *ap = *in;
    ap->game = 0;
    ap->last = NULL;
    ap->rest_until = 0;
    ap->order = a->joins++;
}

void cc_live_each(void (*fn)(cc_live *l, void *ud), void *ud);

static void game_back(cc_live *l, void *ud) {
    (void)ud;
    if (!l->arena || cc_game_over(&l->g)) return;
    cc_arena *a = cc_arena_get(l->arena);
    if (!a) return;
    for (int c = 0; c < 2; c++) {
        cc_arena_player *ap = find(a, l->p[c]);
        if (!ap) continue;
        ap->game = l->id;
        ap->last = l->p[c ^ 1];
        a->playing++;
    }
}

void cc_arena_games_recovered(void) { cc_live_each(game_back, NULL); }
