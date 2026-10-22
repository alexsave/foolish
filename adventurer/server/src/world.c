// world.c - world.h.
#include "world.h"

#include "mvt.h"
#include "trace.h"
#include "util/err.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

int adv_world_init(adv_world *w, const adv_graph *g) {
    memset(w, 0, sizeof *w);
    w->g = g;
    w->max_users = ADV_MAX_USERS + 1;
    w->users = calloc(w->max_users, sizeof *w->users);
    w->owners = calloc(g->h->n_segs ? g->h->n_segs : 1, sizeof *w->owners);
    w->boards = calloc(g->h->n_regions ? g->h->n_regions : 1, sizeof *w->boards);
    w->all = calloc(g->h->n_segs ? g->h->n_segs : 1, sizeof *w->all);
    w->all_region_cm = calloc((size_t)(g->h->n_regions ? g->h->n_regions : 1) * ADV_T_N, sizeof *w->all_region_cm);
    if (!w->users || !w->owners || !w->boards || !w->all || !w->all_region_cm) {
        adv_world_free(w);
        return CB_E_NOMEM;
    }
    for (int i = 0; i < ADV_USER_LOCKS; i++) pthread_mutex_init(&w->user_locks[i], NULL);
    pthread_mutex_init(&w->boards_lock, NULL);
    pthread_mutex_init(&w->acc_lock, NULL);
    atomic_store(&w->next_activity, 1);
    return CB_OK;
}

void adv_world_free(adv_world *w) {
    if (w->users)
        for (uint32_t i = 0; i < w->max_users; i++) {
            adv_cov_free(&w->users[i].cov);
            free(w->users[i].rp);
        }
    free(w->users);
    free(w->owners);
    free(w->boards);
    free(w->all);
    free(w->all_region_cm);
    memset(w, 0, sizeof *w);
}

pthread_mutex_t *adv_world_user_lock(adv_world *w, uint32_t id) { return &w->user_locks[id % ADV_USER_LOCKS]; }

adv_user *adv_world_user(adv_world *w, uint32_t id) {
    if (id == 0 || id >= w->max_users) return NULL;
    adv_user *u = &w->users[id];
    return u->id ? u : NULL;
}

int adv_world_user_define(adv_world *w, const adv_w_user_def *d) {
    if (d->id == 0 || d->id >= w->max_users || d->name_len == 0 || d->name_len >= ADV_W_NAME) return CB_E_INVALID;
    if (d->kind < ADV_UK_COMMUTER || d->kind > ADV_UK_TOURIST) return CB_E_INVALID;
    adv_pt home = { d->home_lat, d->home_lon };
    if (!adv_pt_valid(home)) return CB_E_INVALID;
    for (int i = 0; i < d->name_len; i++)
        if ((unsigned char)d->name[i] < 0x20) return CB_E_INVALID;
    pthread_mutex_t *l = adv_world_user_lock(w, d->id);
    pthread_mutex_lock(l);
    adv_user *u = &w->users[d->id];
    bool fresh = u->id == 0;
    u->id = d->id;
    u->kind = d->kind;
    u->name_len = d->name_len;
    memset(u->name, 0, sizeof u->name);
    memcpy(u->name, d->name, d->name_len);
    u->home = home;
    pthread_mutex_unlock(l);
    if (fresh) atomic_fetch_add(&w->n_users, 1);
    return CB_OK;
}

uint32_t adv_month_key(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    return (uint32_t)(tm.tm_year + 1900) * 12 + (uint32_t)tm.tm_mon;
}
uint32_t adv_year_key(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    return (uint32_t)(tm.tm_year + 1900);
}

// The user's progress record for a region, made when absent (sorted insert). NULL: no memory.
static adv_rprog *rprog_of(adv_user *u, uint16_t region) {
    uint32_t lo = 0, hi = u->n_rp;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (u->rp[mid].region < region) lo = mid + 1;
        else hi = mid;
    }
    if (lo < u->n_rp && u->rp[lo].region == region) return &u->rp[lo];
    if (u->n_rp == u->cap_rp) {
        uint32_t nc = u->cap_rp ? u->cap_rp * 2 : 8;
        if (nc > ADV_MAX_REGIONS) nc = ADV_MAX_REGIONS;
        if (nc <= u->n_rp) return NULL;
        adv_rprog *np = realloc(u->rp, nc * sizeof *np);
        if (!np) return NULL;
        u->rp = np;
        u->cap_rp = (uint16_t)nc;
    }
    memmove(&u->rp[lo + 1], &u->rp[lo], (u->n_rp - lo) * sizeof *u->rp);
    memset(&u->rp[lo], 0, sizeof u->rp[lo]);
    u->rp[lo].region = region;
    u->n_rp++;
    return &u->rp[lo];
}

// A board's row for one type: keeps user's new total m in the top 3.
static void board_put(adv_w_leader row[3], uint32_t user, float m) {
    int at = -1;
    for (int i = 0; i < 3; i++) if (row[i].user == user) at = i;
    if (at < 0) {
        // The empty or smallest slot, if m beats it.
        int min = 0;
        for (int i = 1; i < 3; i++) if (row[i].user == 0 || (row[min].user != 0 && row[i].m < row[min].m)) min = i;
        if (row[min].user != 0 && row[min].m >= m) return;
        at = min;
        row[at].user = user;
    }
    row[at].m = m;
    // Keep descending.
    for (int i = 1; i < 3; i++)
        for (int j = i; j > 0 && row[j].user != 0 && (row[j - 1].user == 0 || row[j].m > row[j - 1].m); j--) {
            adv_w_leader t = row[j];
            row[j] = row[j - 1];
            row[j - 1] = t;
        }
}

static void board_period(adv_board *b, uint32_t mk, uint32_t yk) {
    if (b->month_key != mk) { memset(b->month, 0, sizeof b->month); b->month_key = mk; }
    if (b->year_key != yk) { memset(b->year, 0, sizeof b->year); b->year_key = yk; }
}

int adv_world_apply(adv_world *w, adv_user *u, const adv_result *r, adv_applied *out) {
    memset(out, 0, sizeof *out);
    if (!u || !u->id) return CB_E_NOT_FOUND;
    const adv_graph *g = w->g;
    int type = adv_type_index(r->type);
    // The merge, with every hit's new bits.
    static _Thread_local adv_cov_delta *deltas;
    static _Thread_local uint32_t deltas_cap;
    if (r->n_hits > deltas_cap) {
        uint32_t nc = r->n_hits < 4096 ? 4096 : r->n_hits;
        adv_cov_delta *nd = realloc(deltas, (size_t)nc * sizeof *nd);
        if (!nd) return CB_E_NOMEM;
        deltas = nd;
        deltas_cap = nc;
    }
    float new_m[ADV_T_N] = { 0, 0, 0 };
    int rc = adv_cov_merge(&u->cov, g, r->hits, r->n_hits, type, deltas, new_m);
    if (rc != CB_OK) return rc;
    u->n_activities++;
    if (!u->first_ms || r->start_ms < u->first_ms) u->first_ms = r->start_ms;
    if (r->start_ms > u->last_ms) u->last_ms = r->start_ms;
    for (int t = 0; t < ADV_T_N; t++) { u->unique_m[t] += new_m[t]; out->new_m[t] = new_m[t]; }
    if (new_m[ADV_T_ALL] <= 0 && new_m[type] <= 0) return CB_OK;   // nothing new: the stats above were enough
    u->version++;
    uint32_t mk = adv_month_key(r->start_ms), yk = adv_year_key(r->start_ms);
    // The regions this apply touched, so the boards of the others are left alone (a record of an
    // older month must not reset a board it is not on).
    enum { TOUCH_MAX = 64 };
    uint16_t touched[TOUCH_MAX];
    uint32_t n_touched = 0;
    bool touched_all = false;
    // Per region: the new length of each hit goes to its segment's region.
    bool all_changed = false;
    for (uint32_t i = 0; i < r->n_hits; i++) {
        const adv_cov_delta *d = &deltas[i];
        if (!d->new_mask[ADV_T_ALL] && !d->new_mask[type]) continue;
        const adv_seg *s = &g->segs[d->seg];
        // Owners: the first user to touch a segment owns it; a second makes it everyone's.
        if (d->new_mask[ADV_T_ALL]) {
            uint32_t cur = atomic_load_explicit(&w->owners[d->seg], memory_order_relaxed);
            if (cur == 0) atomic_compare_exchange_strong(&w->owners[d->seg], &cur, u->id);
            if (cur != 0 && cur != u->id) atomic_store_explicit(&w->owners[d->seg], ADV_OWNER_MANY, memory_order_relaxed);
        }
        // Everyone: per type, whether this user is new to the segment (every bit it holds now
        // was set by this merge), and the bits nobody had (the ones not in the union before).
        adv_all_seg *a = &w->all[d->seg];
        for (int t = 0; t < ADV_T_N; t++) {
            uint16_t nb = d->new_mask[t];
            if (!nb) continue;
            uint16_t after = adv_cov_mask(&u->cov, d->seg, t);
            if ((uint16_t)(after & ~nb) == 0) {
                uint16_t cnt = atomic_load_explicit(&a->users[t], memory_order_relaxed);
                if (cnt != UINT16_MAX && atomic_fetch_add_explicit(&a->users[t], 1, memory_order_relaxed) == 0)
                    atomic_fetch_add_explicit(&w->all_segs[t], 1, memory_order_relaxed);
                all_changed = true;
            }
            uint16_t fresh = (uint16_t)(nb & ~atomic_fetch_or_explicit(&a->mask[t], nb, memory_order_relaxed));
            if (fresh) {
                uint64_t cm = (uint64_t)(adv_mask_len_m(fresh, s->len_m) * 100.0f + 0.5f);
                atomic_fetch_add_explicit(&w->all_cm[t], cm, memory_order_relaxed);
                if (s->region != ADV_NO_REGION) atomic_fetch_add_explicit(&w->all_region_cm[(size_t)s->region * ADV_T_N + t], cm, memory_order_relaxed);
                all_changed = true;
            }
        }
        if (s->region == ADV_NO_REGION) continue;
        adv_rprog *p = rprog_of(u, s->region);
        if (!p) return CB_E_NOMEM;
        bool known = false;
        for (uint32_t k = 0; k < n_touched && !known; k++) known = touched[k] == s->region;
        if (!known) {
            if (n_touched < TOUCH_MAX) touched[n_touched++] = s->region;
            else touched_all = true;
        }
        if (p->month_key != mk) { memset(p->month_m, 0, sizeof p->month_m); p->month_key = mk; }
        if (p->year_key != yk) { memset(p->year_m, 0, sizeof p->year_m); p->year_key = yk; }
        float add[ADV_T_N] = { adv_mask_len_m(d->new_mask[0], s->len_m), adv_mask_len_m(d->new_mask[1], s->len_m),
                               adv_mask_len_m(d->new_mask[2], s->len_m) };
        for (int t = 0; t < ADV_T_N; t++) {
            p->len_m[t] += add[t];
            p->month_m[t] += add[t];
            p->year_m[t] += add[t];
        }
    }
    if (all_changed) atomic_fetch_add_explicit(&w->all_version, 1, memory_order_relaxed);
    // Achievements and boards, once per touched region (the progress records are few).
    pthread_mutex_lock(&w->boards_lock);
    for (uint32_t k = 0; k < u->n_rp; k++) {
        adv_rprog *p = &u->rp[k];
        bool mine = touched_all;
        for (uint32_t t = 0; t < n_touched && !mine; t++) mine = touched[t] == p->region;
        if (!mine) continue;
        const adv_region *reg = &g->regions[p->region];
        for (int t = 0; t < ADV_T_N; t++) {
            if (reg->len_m[t] > 0) {
                float pct = p->len_m[t] / reg->len_m[t] * 100.0f;
                for (int a = 0; a < ADV_ACH_N; a++)
                    if (pct >= ADV_ACH_PCT[a]) p->ach[t] |= (uint8_t)(1u << a);
            }
        }
        adv_board *b = &w->boards[p->region];
        board_period(b, mk, yk);
        for (int t = 0; t < ADV_T_N; t++) {
            if (p->month_key == mk && p->month_m[t] > 0) board_put(b->month[t], u->id, p->month_m[t]);
            if (p->year_key == yk && p->year_m[t] > 0) board_put(b->year[t], u->id, p->year_m[t]);
        }
    }
    pthread_mutex_unlock(&w->boards_lock);
    return CB_OK;
}

void adv_world_super_unique(const adv_world *w, const adv_user *u, uint32_t *n_segs, float *len_m) {
    uint32_t n = 0;
    double m = 0;
    for (uint32_t i = 0; i < u->cov.n; i++) {
        uint32_t seg = u->cov.e[i].seg;
        if (atomic_load_explicit(&w->owners[seg], memory_order_relaxed) == u->id) {
            n++;
            m += adv_mask_len_m((uint16_t)(u->cov.e[i].mask[0] | u->cov.e[i].mask[1]), w->g->segs[seg].len_m);
        }
    }
    *n_segs = n;
    *len_m = (float)m;
}

int adv_world_rank(adv_world *w, uint16_t region, bool year, int type, uint32_t user) {
    if (region >= w->g->h->n_regions || type < 0 || type >= ADV_T_N) return 0;
    const adv_w_leader *row = year ? w->boards[region].year[type] : w->boards[region].month[type];
    for (int i = 0; i < 3; i++) if (row[i].user == user) return i + 1;
    return 0;
}

void adv_world_board_copy(adv_world *w, uint16_t region, adv_board *out) {
    memset(out, 0, sizeof *out);
    if (region >= w->g->h->n_regions) return;
    pthread_mutex_lock(&w->boards_lock);
    *out = w->boards[region];
    pthread_mutex_unlock(&w->boards_lock);
}

// The roads anyone has, keyed by (name, region), in open addressing sized for them; the top list
// is then an insertion over the table. An UNNAMED segment is no road of this list: a park path or
// a service way has nothing to call it by (a type is not a name), so it is left out.
typedef struct { uint64_t key; adv_top_seg road; } road_slot;

static uint64_t road_key(const adv_graph *g, uint32_t seg) {
    const adv_seg *s = &g->segs[seg];
    (void)seg;
    return (1ull << 63) | ((uint64_t)s->name << 16) | (uint64_t)s->region;   // a name in a region
}

uint32_t adv_world_top(const adv_world *w, int type, adv_top_seg out[ADV_TOP_N]) {
    const adv_graph *g = w->g;
    uint32_t have = atomic_load_explicit((_Atomic uint32_t *)&w->all_segs[type], memory_order_relaxed);
    if (!have) return 0;
    uint64_t cap = 16;
    while (cap < (uint64_t)have * 2) cap *= 2;
    road_slot *t = calloc(cap, sizeof *t);
    if (!t) return 0;
    for (uint32_t i = 0; i < g->h->n_segs; i++) {
        uint16_t users = atomic_load_explicit(&w->all[i].users[type], memory_order_relaxed);
        if (!users || g->segs[i].name == 0) continue;
        uint64_t key = road_key(g, i);
        uint64_t k = (key * 0x9E3779B97F4A7C15ull) >> 20;
        for (;;) {
            road_slot *slot = &t[k & (cap - 1)];
            if (!slot->key) {
                slot->key = key;
                slot->road.seg = i;
                slot->road.users = users;
                slot->road.n_segs = 1;
                slot->road.len_m = g->segs[i].len_m;
                break;
            }
            if (slot->key == key) {
                if (users > slot->road.users) { slot->road.users = users; slot->road.seg = i; }
                if (slot->road.n_segs != UINT16_MAX) slot->road.n_segs++;
                slot->road.len_m += g->segs[i].len_m;
                break;
            }
            k++;
        }
    }
    uint32_t n = 0;
    for (uint64_t i = 0; i < cap; i++) {
        if (!t[i].key) continue;
        const adv_top_seg *r = &t[i].road;
        if (n == ADV_TOP_N) {
            const adv_top_seg *last = &out[ADV_TOP_N - 1];
            if (r->users < last->users || (r->users == last->users && r->len_m <= last->len_m)) continue;
            n--;
        }
        uint32_t k = n;
        while (k > 0 && (r->users > out[k - 1].users || (r->users == out[k - 1].users && r->len_m > out[k - 1].len_m))) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = *r;
        n++;
    }
    free(t);
    return n;
}
