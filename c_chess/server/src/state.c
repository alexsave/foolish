/* state.c - see cc_server.h: the maps, the seeks and challenges, the pushes, the finish of a game,
 * and the clock thread that ends games on the flag. */
#include "cc_server.h"
#include "journal.h"
#include "pushes.h"

#include "realtime/realtime.h"
#include "util/clock.h"
#include "util/log.h"
#include "fnv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

pthread_mutex_t cc_lock = PTHREAD_MUTEX_INITIALIZER;

static cb_ctx *CTX;

int64_t cc_mono(void) { return cb_mono_ms(); }
int64_t cc_wall(void) { return cb_now_ms(); }

/* ---- maps: chained, power-of-two buckets, doubled at load 1 ---- */

typedef struct { void **b; uint32_t mask, n; } map_t;

static int map_init(map_t *m, uint32_t buckets) {
    m->b = calloc(buckets, sizeof *m->b);
    m->mask = buckets - 1;
    m->n = 0;
    return m->b ? 0 : -1;
}

static uint64_t hash_str(const char *s) { return fnv1a64_str(FNV64_OFFSET, s); }
static uint64_t hash_u32(uint32_t v) { uint64_t x = v * 0x9e3779b97f4a7c15ULL; return x ^ (x >> 29); }

static map_t P_ID, P_NAME, LIVE, CHAL;

static uint64_t name_hash(const char *name) {
    char low[CW_NAME + 1];
    size_t i = 0;
    for (; name[i] && i < CW_NAME; i++) low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    low[i] = 0;
    return hash_str(low);
}

static bool name_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = (char)(*a >= 'A' && *a <= 'Z' ? *a + 32 : *a), y = (char)(*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);
        if (x != y) return false;
        if (!x) return true;
    }
}

static void grow_players(void) {
    if (P_ID.n <= P_ID.mask) return;
    map_t a, b;
    if (map_init(&a, (P_ID.mask + 1) * 2) || map_init(&b, (P_ID.mask + 1) * 2)) return;
    for (uint32_t i = 0; i <= P_ID.mask; i++)
        for (cc_player *p = P_ID.b[i], *nx; p; p = nx) {
            nx = p->next;
            uint32_t h = (uint32_t)hash_str(p->id) & a.mask;
            p->next = a.b[h]; a.b[h] = p;
            uint32_t k = (uint32_t)name_hash(p->name) & b.mask;
            p->next_name = b.b[k]; b.b[k] = p;
        }
    a.n = b.n = P_ID.n;
    free(P_ID.b); free(P_NAME.b);
    P_ID = a; P_NAME = b;
}

cc_player *cc_player_get(const char *id) {
    for (cc_player *p = P_ID.b[hash_str(id) & P_ID.mask]; p; p = p->next)
        if (strcmp(p->id, id) == 0) return p;
    return NULL;
}

cc_player *cc_player_by_name(const char *name) {
    for (cc_player *p = P_NAME.b[name_hash(name) & P_NAME.mask]; p; p = p->next_name)
        if (name_eq(p->name, name)) return p;
    return NULL;
}

/* Puts a player in both maps (it must not be there). */
static void player_insert(cc_player *p) {
    uint32_t h = (uint32_t)hash_str(p->id) & P_ID.mask;
    p->next = P_ID.b[h]; P_ID.b[h] = p;
    uint32_t k = (uint32_t)name_hash(p->name) & P_NAME.mask;
    p->next_name = P_NAME.b[k]; P_NAME.b[k] = p;
    P_ID.n++; P_NAME.n++;
    grow_players();
}

cc_player *cc_player_recovered(const cc_player *in) {
    cc_player *p = malloc(sizeof *p);
    if (!p) return NULL;
    *p = *in;
    player_insert(p);
    return p;
}

cc_player *cc_player_of(const char *id, const char *name) {
    cc_player *p = cc_player_get(id);
    if (p) return p;
    if (!(p = calloc(1, sizeof *p))) return NULL;
    snprintf(p->id, sizeof p->id, "%s", id);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->r = cc_rating_new();
    player_insert(p);
    cc_journal_player(p);
    return p;
}

/* ---- live games ---- */

static uint32_t next_game_id = 1;
static uint32_t n_playing;

void cc_state_next_game_id(uint32_t id) { if (id >= next_game_id) next_game_id = id + 1; }

cc_live *cc_live_get(uint32_t id) {
    for (cc_live *l = LIVE.b[hash_u32(id) & LIVE.mask]; l; l = l->next)
        if (l->id == id) return l;
    return NULL;
}

static void live_grow(void) {
    if (LIVE.n <= LIVE.mask) return;
    map_t a;
    if (map_init(&a, (LIVE.mask + 1) * 2)) return;
    for (uint32_t i = 0; i <= LIVE.mask; i++)
        for (cc_live *l = LIVE.b[i], *nx; l; l = nx) {
            nx = l->next;
            uint32_t h = (uint32_t)hash_u32(l->id) & a.mask;
            l->next = a.b[h]; a.b[h] = l;
        }
    a.n = LIVE.n;
    free(LIVE.b);
    LIVE = a;
}

static void live_insert(cc_live *l) {
    uint32_t h = (uint32_t)hash_u32(l->id) & LIVE.mask;
    l->next = LIVE.b[h]; LIVE.b[h] = l;
    LIVE.n++;
    live_grow();
}

uint32_t cc_games_live(void) { return n_playing; }

/* Every game in memory (under cc_lock). */
void cc_live_each(void (*fn)(cc_live *l, void *ud), void *ud) {
    for (uint32_t i = 0; i <= LIVE.mask; i++)
        for (cc_live *l = LIVE.b[i]; l; l = l->next) fn(l, ud);
}

int cc_color_of(const cc_live *l, const cc_player *p) {
    if (l->p[0] == p) return CH_WHITE;
    if (l->p[1] == p) return CH_BLACK;
    return -1;
}

/* ---- the clock thread: a min-heap of (deadline, game, gen) ---- */

/* An indexed min-heap of the games whose clock can end them, one entry a game (cc_live.timer_at and
 * .heap_idx, which is 1 + its index, 0 when not in the heap): a move moves the game's entry instead
 * of adding one, so the heap holds the games in play and nothing stale. */
static cc_live **HEAP;
static uint32_t heap_n, heap_cap;
static pthread_cond_t clock_cond = PTHREAD_COND_INITIALIZER;
static pthread_t clock_thread;
static bool clock_stop, clock_started;

static inline void heap_set(uint32_t i, cc_live *l) { HEAP[i] = l; l->heap_idx = i + 1; }

static void heap_up(uint32_t i) {
    cc_live *l = HEAP[i];
    while (i && HEAP[(i - 1) / 2]->timer_at > l->timer_at) { heap_set(i, HEAP[(i - 1) / 2]); i = (i - 1) / 2; }
    heap_set(i, l);
}

static void heap_down(uint32_t i) {
    cc_live *l = HEAP[i];
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= heap_n) break;
        if (c + 1 < heap_n && HEAP[c + 1]->timer_at < HEAP[c]->timer_at) c++;
        if (HEAP[c]->timer_at >= l->timer_at) break;
        heap_set(i, HEAP[c]);
        i = c;
    }
    heap_set(i, l);
}

static void heap_remove(cc_live *l) {
    if (!l->heap_idx) return;
    uint32_t i = l->heap_idx - 1;
    l->heap_idx = 0;
    cc_live *last = HEAP[--heap_n];
    if (i == heap_n) return;
    heap_set(i, last);
    heap_up(i);
    heap_down(last->heap_idx - 1);
}

static void heap_put(cc_live *l, int64_t at) {
    if (l->heap_idx) {
        int64_t was = l->timer_at;
        l->timer_at = at;
        if (at < was) heap_up(l->heap_idx - 1); else heap_down(l->heap_idx - 1);
        return;
    }
    if (heap_n == heap_cap) {
        uint32_t c = heap_cap ? heap_cap * 2 : 1024;
        cc_live **h = realloc(HEAP, c * sizeof *h);
        if (!h) return;
        HEAP = h; heap_cap = c;
    }
    l->timer_at = at;
    heap_set(heap_n, l);
    heap_up(heap_n++);
}

static void schedule(cc_live *l, int64_t now) {
    /* when the game could next end by itself (it must not end here: the caller has pushed its state) */
    int64_t next = cc_game_until(&l->g, now);
    if (next < 0) { heap_remove(l); return; }
    bool wake = heap_n == 0 || now + next < HEAP[0]->timer_at;
    heap_put(l, now + next);
    if (wake) pthread_cond_signal(&clock_cond);
}

static void sweep(int64_t now);
static void tv_tick(int64_t now);

static void *clock_main(void *ud) {
    (void)ud;
    int64_t next_sweep = cc_mono() + 10000, next_tv = cc_mono() + 1000;
    pthread_mutex_lock(&cc_lock);
    while (!clock_stop) {
        int64_t now = cc_mono();
        while (heap_n && HEAP[0]->timer_at <= now) {
            cc_live *l = HEAP[0];
            int64_t nx;
            if (cc_game_tick(&l->g, now, &nx)) cc_live_changed(l, now);   /* over: out of the heap */
            else if (nx >= 0) heap_put(l, now + nx);
            else heap_remove(l);
        }
        if (now >= next_sweep) { sweep(now); next_sweep = now + 10000; }
        if (now >= next_tv) { tv_tick(now); next_tv = now + 1000; }
        int64_t soon = next_sweep < next_tv ? next_sweep : next_tv;
        int64_t until = heap_n && HEAP[0]->timer_at < soon ? HEAP[0]->timer_at : soon;
        /* the condition variable waits on the wall clock: the wait is the monotonic distance */
        int64_t wait_ms = until - now;
        if (wait_ms < 1) wait_ms = 1;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += wait_ms / 1000;
        ts.tv_nsec += (wait_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&clock_cond, &cc_lock, &ts);
    }
    pthread_mutex_unlock(&cc_lock);
    return NULL;
}

/* ---- pushes ---- */

void cc_state_fill(const cc_live *l, cw_state *s, int64_t now) {
    memset(s, 0, sizeof *s);
    s->v = CW_V;
    s->kind = CW_P_STATE;
    s->status = l->g.status;
    s->winner = l->g.winner;
    s->game = l->id;
    s->ply = l->g.ply;
    s->move = l->g.ply ? l->g.moves[l->g.ply - 1] : 0;
    for (int c = 0; c < 2; c++) {
        s->left_ms[c] = (uint32_t)cc_game_left(&l->g, c, now);
        s->diff[c] = l->diff[c];
        s->rating[c] = (int16_t)cc_rating_int(&l->p[c]->r);
    }
    s->draw_offer = l->g.draw_offer;
    s->rematch_offer = l->rematch_offer;
    s->clock_running = cc_game_clock_running(&l->g);
    s->next_game = l->next_game;
}

/* ---- the finish: ratings, counts, the journal ---- */

static void finish(cc_live *l, int64_t now) {
    if (l->finished) return;
    l->finished = 1;
    n_playing--;
    l->evict_at = now + CC_FINISHED_KEEP_MS;
    cc_game *g = &l->g;
    bool counts = g->status != CC_ABORTED;
    if (counts) {
        for (int c = 0; c < 2; c++) {
            cc_player *p = l->p[c];
            p->games++;
            if (g->winner == c) p->wins++;
            else if (g->winner == CC_NO_WINNER) p->draws++;
            else p->losses++;
        }
    }
    int64_t wall = cc_wall();
    if (counts && l->rated && l->p[0] != l->p[1]) {
        int before[2] = { cc_rating_int(&l->p[0]->r), cc_rating_int(&l->p[1]->r) };
        cc_rate(&l->p[0]->r, &l->p[1]->r, cc_game_white_score(g));
        for (int c = 0; c < 2; c++) {
            l->diff[c] = (int16_t)(cc_rating_int(&l->p[c]->r) - before[c]);
            l->p[c]->last_at = wall;
            cc_journal_point(l->p[c], wall);
        }
    }
    for (int c = 0; c < 2; c++) cc_journal_player(l->p[c]);
    cc_journal_game_end(l, wall);
}

void cc_live_changed(cc_live *l, int64_t now) {
    if (cc_game_over(&l->g)) finish(l, now);
    cw_state s;
    cc_state_fill(l, &s, now);
    cc_push_room_game(l->id, (const uint8_t *)&s, sizeof s);
    schedule(l, now);
}

cc_live *cc_live_start(cc_player *white, cc_player *black, const cw_seek *tc, int64_t now) {
    cc_live *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->id = next_game_id++;
    cc_game_new(&l->g, (int32_t)tc->base_s * 1000, (int32_t)tc->inc_s * 1000, now);
    l->p[0] = white;
    l->p[1] = black;
    l->rated = tc->rated && white != black;
    for (int c = 0; c < 2; c++) l->rating0[c] = (int16_t)cc_rating_int(&l->p[c]->r);
    l->started_at = cc_wall();
    live_insert(l);
    n_playing++;
    cc_journal_game_start(l);
    schedule(l, now);
    for (int c = 0; c < 2; c++) {
        cw_start st = { CW_V, CW_P_START, (uint8_t)c, 0, l->id };
        cc_push_user(l->p[c]->id, (const uint8_t *)&st, sizeof st);
    }
    return l;
}

/* A recovered game (journal.c), in play: its clock entry. */
void cc_live_recovered(cc_live *l, int64_t now) {
    live_insert(l);
    cc_state_next_game_id(l->id);
    if (!cc_game_over(&l->g)) { n_playing++; schedule(l, now); }
    else l->finished = 1, l->evict_at = now + CC_FINISHED_KEEP_MS;
}

/* ---- seeks ---- */

static cc_seek SEEKS[CC_SEEKS_MAX];
static uint32_t n_seeks, seek_seq;

static bool same_tc(const cw_seek *a, const cw_seek *b) { return a->base_s == b->base_s && a->inc_s == b->inc_s && a->rated == b->rated; }

static void seeks_expire(int64_t now) {
    for (uint32_t i = 0; i < n_seeks;)
        if (SEEKS[i].expires <= now) SEEKS[i] = SEEKS[--n_seeks];
        else i++;
}

int cc_lobby_fill(cw_lobby *lb, int64_t now) {
    seeks_expire(now);
    memset(lb, 0, 12);
    lb->v = CW_V;
    lb->kind = CW_P_LOBBY;
    lb->games = n_playing;
    lb->players = (uint32_t)P_ID.n;
    uint32_t n = 0;
    for (uint32_t i = n_seeks; i-- > 0 && n < CW_LOBBY_MAX;) {   /* newest first */
        cw_seek_row *r = &lb->seeks[n++];
        memset(r, 0, sizeof *r);
        r->id = SEEKS[i].id;
        r->base_s = SEEKS[i].tc.base_s;
        r->inc_s = SEEKS[i].tc.inc_s;
        r->rated = SEEKS[i].tc.rated;
        r->rating = (int16_t)cc_rating_int(&SEEKS[i].who->r);
        memcpy(r->name, SEEKS[i].who->name, strnlen(SEEKS[i].who->name, CW_NAME));
    }
    lb->n = (uint16_t)n;
    return (int)(12 + n * sizeof(cw_seek_row));
}

static void lobby_changed(int64_t now) {
    if (!cc_push_lobby_heard()) return;
    static cw_lobby lb;   /* under cc_lock */
    int n = cc_lobby_fill(&lb, now);
    cc_push_lobby((const uint8_t *)&lb, (size_t)n);
}

int cc_seek_add(cc_player *p, const cw_seek *tc, int64_t now, cc_live **game) {
    *game = NULL;
    seeks_expire(now);
    for (uint32_t i = 0; i < n_seeks; i++) {
        if (SEEKS[i].who == p) {   /* one seek a player: the newest wins */
            SEEKS[i] = SEEKS[--n_seeks];
            break;
        }
    }
    for (uint32_t i = 0; i < n_seeks; i++) {
        if (SEEKS[i].who != p && same_tc(&SEEKS[i].tc, tc)) {
            cc_player *other = SEEKS[i].who;
            SEEKS[i] = SEEKS[--n_seeks];
            /* colors: the one who waited longer gets white on even game ids */
            bool other_white = (next_game_id & 1) == 0;
            *game = cc_live_start(other_white ? other : p, other_white ? p : other, tc, now);
            lobby_changed(now);
            return *game ? CW_E_NONE : CW_E_BUSY;
        }
    }
    if (n_seeks == CC_SEEKS_MAX) return CW_E_BUSY;
    SEEKS[n_seeks++] = (cc_seek){ ++seek_seq, p, *tc, now + CC_SEEK_TTL_MS };
    lobby_changed(now);
    return CW_E_NONE;
}

void cc_seek_cancel(cc_player *p, int64_t now) {
    for (uint32_t i = 0; i < n_seeks; i++)
        if (SEEKS[i].who == p) { SEEKS[i] = SEEKS[--n_seeks]; lobby_changed(now); return; }
}

/* ---- challenges ---- */

cc_challenge *cc_challenge_add(cc_player *p, const cw_seek *tc, int64_t now) {
    cc_challenge *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    do {
        uint8_t r[4];
        cb_random_bytes(r, sizeof r);
        c->id = ((uint32_t)r[0] | (uint32_t)r[1] << 8 | (uint32_t)r[2] << 16 | (uint32_t)r[3] << 24) & 0x7fffffff;
    } while (!c->id || cc_challenge_get(c->id, now));
    c->who = p;
    c->tc = *tc;
    c->expires = now + CC_CHALLENGE_TTL_MS;
    uint32_t h = (uint32_t)hash_u32(c->id) & CHAL.mask;
    c->next = CHAL.b[h];
    CHAL.b[h] = c;
    CHAL.n++;
    return c;
}

cc_challenge *cc_challenge_get(uint32_t id, int64_t now) {
    for (cc_challenge *c = CHAL.b[hash_u32(id) & CHAL.mask]; c; c = c->next)
        if (c->id == id) return c->expires > now ? c : NULL;
    return NULL;
}

void cc_challenge_drop(uint32_t id) {
    for (cc_challenge **pp = (cc_challenge **)&CHAL.b[hash_u32(id) & CHAL.mask]; *pp; pp = &(*pp)->next)
        if ((*pp)->id == id) { cc_challenge *c = *pp; *pp = c->next; free(c); CHAL.n--; return; }
}

/* ---- the lobby's live games ---- */

static void tv_game(const cc_live *l, cw_tv_game *t, int64_t now) {
    memset(t, 0, sizeof *t);
    t->game = l->id;
    t->ply = l->g.ply;
    t->base_s = (uint16_t)(l->g.base_ms / 1000);
    t->inc_s = (uint8_t)(l->g.inc_ms / 1000);
    t->status = l->g.status;
    t->stm = l->g.pos.stm;
    t->clock_running = cc_game_clock_running(&l->g);
    t->last_from = t->last_to = 64;
    if (l->g.ply) { t->last_from = (uint8_t)CH_FROM(l->g.moves[l->g.ply - 1]); t->last_to = (uint8_t)CH_TO(l->g.moves[l->g.ply - 1]); }
    for (int c = 0; c < 2; c++) {
        t->rating[c] = l->rating0[c];
        t->left_ms[c] = (uint32_t)cc_game_left(&l->g, c, now);
        memcpy(t->name[c], l->p[c]->name, strnlen(l->p[c]->name, CW_NAME));
    }
    for (int sq = 0; sq < 64; sq++) t->board[sq / 2] |= (uint8_t)(l->g.pos.sq[sq] << (sq & 1 ? 4 : 0));
}

int cc_tv_fill(cw_tv *tv, int max, int64_t now) {
    if (max > CW_TV_MAX) max = CW_TV_MAX;
    const cc_live *best[CW_TV_MAX];
    int keys[CW_TV_MAX], n = 0;
    for (uint32_t i = 0; i <= LIVE.mask; i++)
        for (const cc_live *l = LIVE.b[i]; l; l = l->next) {
            if (cc_game_over(&l->g)) continue;
            int key = l->rating0[0] + l->rating0[1];
            if (n == max && key <= keys[n - 1]) continue;
            int j = n < max ? n++ : n - 1;   /* insert, the lowest falling off */
            while (j > 0 && keys[j - 1] < key) { keys[j] = keys[j - 1]; best[j] = best[j - 1]; j--; }
            keys[j] = key;
            best[j] = l;
        }
    memset(tv, 0, 8);
    tv->v = CW_V;
    tv->kind = CW_P_TV;
    tv->n = (uint16_t)n;
    for (int i = 0; i < n; i++) tv_game(best[i], &tv->games[i], now);
    return 8 + n * (int)sizeof(cw_tv_game);
}

static cw_tv TV_SENT;
static int tv_sent_len;

static void tv_tick(int64_t now) {
    if (!cc_push_lobby_heard()) { tv_sent_len = 0; return; }
    static cw_tv tv;
    int n = cc_tv_fill(&tv, CW_TV_MAX, now);
    if (n == tv_sent_len && memcmp(&tv, &TV_SENT, (size_t)n) == 0) return;
    memcpy(&TV_SENT, &tv, (size_t)n);
    tv_sent_len = n;
    cc_push_lobby((const uint8_t *)&tv, (size_t)n);
}

/* ---- the leaderboard ---- */

int cc_top_fill(cw_top *top) {
    const cc_player *best[CW_TOP_MAX];
    double keys[CW_TOP_MAX];
    int n = 0;
    for (uint32_t i = 0; i <= P_ID.mask; i++)
        for (const cc_player *p = P_ID.b[i]; p; p = p->next) {
            if (!p->games) continue;
            double key = p->r.rating;
            if (n == CW_TOP_MAX && key <= keys[n - 1]) continue;
            int j = n < CW_TOP_MAX ? n++ : n - 1;
            while (j > 0 && keys[j - 1] < key) { keys[j] = keys[j - 1]; best[j] = best[j - 1]; j--; }
            keys[j] = key;
            best[j] = p;
        }
    memset(top, 0, 8);
    top->n = (uint16_t)n;
    for (int i = 0; i < n; i++) {
        cw_top_row *r = &top->rows[i];
        memset(r, 0, sizeof *r);
        memcpy(r->name, best[i]->name, strnlen(best[i]->name, CW_NAME));
        r->rating = (int16_t)cc_rating_int(&best[i]->r);
        r->rd = (uint16_t)(best[i]->r.deviation + 0.5);
        r->games = best[i]->games;
    }
    return 8 + n * (int)sizeof(cw_top_row);
}

/* Finished games leave memory after a while (they stay in the journal); challenges past their time
 * go. */
static void sweep(int64_t now) {
    for (uint32_t i = 0; i <= LIVE.mask; i++)
        for (cc_live **pp = (cc_live **)&LIVE.b[i]; *pp;) {
            cc_live *l = *pp;
            if (l->finished && l->evict_at <= now) { *pp = l->next; LIVE.n--; free(l); }
            else pp = &l->next;
        }
    for (uint32_t i = 0; i <= CHAL.mask; i++)
        for (cc_challenge **pp = (cc_challenge **)&CHAL.b[i]; *pp;) {
            cc_challenge *c = *pp;
            if (c->expires <= now) { *pp = c->next; CHAL.n--; free(c); }
            else pp = &c->next;
        }
    size_t before = n_seeks;
    seeks_expire(now);
    if (n_seeks != before) lobby_changed(now);
}

/* ---- life ---- */

int cc_state_init(cb_ctx *ctx) {
    CTX = ctx;
    ch_init();
    if (map_init(&P_ID, 1024) || map_init(&P_NAME, 1024) || map_init(&LIVE, 1024) || map_init(&CHAL, 256)) return -1;
    return 0;
}

int cc_state_start_clock(void) {
    clock_stop = false;
    if (pthread_create(&clock_thread, NULL, clock_main, NULL) != 0) return -1;
    clock_started = true;
    return 0;
}

void cc_state_free(void) {
    if (clock_started) {
        pthread_mutex_lock(&cc_lock);
        clock_stop = true;
        pthread_cond_signal(&clock_cond);
        pthread_mutex_unlock(&cc_lock);
        pthread_join(clock_thread, NULL);
        clock_started = false;
    }
    for (uint32_t i = 0; P_ID.b && i <= P_ID.mask; i++)
        for (cc_player *p = P_ID.b[i], *nx; p; p = nx) { nx = p->next; free(p); }
    for (uint32_t i = 0; LIVE.b && i <= LIVE.mask; i++)
        for (cc_live *l = LIVE.b[i], *nx; l; l = nx) { nx = l->next; free(l); }
    for (uint32_t i = 0; CHAL.b && i <= CHAL.mask; i++)
        for (cc_challenge *c = CHAL.b[i], *nx; c; c = nx) { nx = c->next; free(c); }
    free(P_ID.b); free(P_NAME.b); free(LIVE.b); free(CHAL.b); free(HEAP);
    memset(&P_ID, 0, sizeof P_ID); memset(&P_NAME, 0, sizeof P_NAME); memset(&LIVE, 0, sizeof LIVE); memset(&CHAL, 0, sizeof CHAL);
    HEAP = NULL; heap_n = heap_cap = 0; n_seeks = 0; n_playing = 0;
}
