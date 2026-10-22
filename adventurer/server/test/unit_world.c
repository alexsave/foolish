// unit_world.c - the world's apply (coverage, totals, regions, achievements, boards, owners), the
// journal's round trip (what recovery rebuilds equals what the live path built).
#include "coverage.h"
#include "journal.h"
#include "mvt.h"
#include "lib_fakegraph.h"
#include "server.h"
#include "store/store.h"
#include "t.h"
#include "trace.h"
#include "util/err.h"
#include "world.h"

#include <math.h>
#include <unistd.h>

static const int64_t T0 = 1760000000000LL;   // 2025-10-09

// Hits over every segment of one region, full masks (so a user "completes" it).
static uint32_t region_hits(const adv_graph *g, uint16_t region, adv_hit *out, uint32_t cap, double frac_of_segs) {
    uint32_t n = 0, seen = 0;
    for (uint32_t i = 0; i < g->h->n_segs && n < cap; i++) {
        if (g->segs[i].region != region) continue;
        seen++;
        if ((double)n < frac_of_segs * seen || frac_of_segs >= 1) { out[n].seg = i; out[n].mask = 0xFFFF; out[n].pad = 0; n++; }
    }
    return adv_hits_finish(out, n);
}

static void define_users(adv_world *w) {
    adv_w_user_def d1 = { .id = 1, .kind = ADV_UK_EXPLORER, .name_len = 4, .home_lat = 418000000, .home_lon = -714000000, .name = "Mara" };
    adv_w_user_def d2 = { .id = 2, .kind = ADV_UK_RUNNER, .name_len = 3, .home_lat = 418000000, .home_lon = -714000000, .name = "Jon" };
    T_CHECK(adv_world_user_define(w, &d1) == CB_OK);
    T_CHECK(adv_world_user_define(w, &d2) == CB_OK);
    adv_w_user_def bad = d1;
    bad.id = 0;
    T_CHECK(adv_world_user_define(w, &bad) == CB_E_INVALID);
    bad = d1;
    bad.name_len = 0;
    T_CHECK(adv_world_user_define(w, &bad) == CB_E_INVALID);
    bad = d1;
    bad.kind = 9;
    T_CHECK(adv_world_user_define(w, &bad) == CB_E_INVALID);
    T_CHECK(atomic_load(&w->n_users) == 2);
    T_CHECK(adv_world_user(w, 3) == NULL && adv_world_user(w, 1)->kind == ADV_UK_EXPLORER);
}

static int apply(adv_world *w, uint32_t user, uint8_t type, int64_t start_ms, adv_hit *h, uint32_t n, adv_applied *a, uint32_t act) {
    adv_result r = { .activity = act, .user = user, .type = type, .start_ms = start_ms, .gen_id = act, .n_points = 100,
                     .matched_m = adv_hits_len_m(w->g, h, n), .accuracy = 0.97f, .cpu_us = 1234, .hits = h, .n_hits = n };
    adv_user *u = adv_world_user(w, user);
    if (!u) return CB_E_NOT_FOUND;
    pthread_mutex_t *l = adv_world_user_lock(w, user);
    pthread_mutex_lock(l);
    int rc = adv_world_apply(w, u, &r, a);
    pthread_mutex_unlock(l);
    return rc;
}

static void test_apply(adv_graph *g) {
    adv_world w;
    T_CHECK(adv_world_init(&w, g) == CB_OK);
    define_users(&w);
    static adv_hit h[4096];
    // User 1 rides 30% of the West region's segments.
    uint32_t n = region_hits(g, 0, h, 4096, 0.3);
    T_CHECK(n > 0);
    adv_applied a;
    T_CHECK(apply(&w, 1, ADV_ACT_BIKE, T0, h, n, &a, 1) == CB_OK);
    adv_user *u = adv_world_user(&w, 1);
    float len = adv_hits_len_m(g, h, n);
    T_CHECK_MSG(fabs(a.new_m[ADV_T_BIKE] - len) < 0.01 && fabs(a.new_m[ADV_T_ALL] - len) < 0.01 && a.new_m[ADV_T_FOOT] == 0, "new %f", a.new_m[0]);
    T_CHECK(u->n_activities == 1 && u->version == 1 && fabs(u->unique_m[ADV_T_ALL] - len) < 0.01);
    T_CHECK(u->n_rp == 1 && u->rp[0].region == 0);
    float pct = u->rp[0].len_m[ADV_T_BIKE] / g->regions[0].len_m[ADV_T_BIKE] * 100;
    T_CHECK_MSG(pct > 20 && pct < 45, "pct %f", pct);
    T_CHECK((u->rp[0].ach[ADV_T_BIKE] & 1) == (pct >= 25 ? 1 : 0));
    T_CHECK(u->rp[0].ach[ADV_T_FOOT] == 0);
    T_CHECK(u->rp[0].month_key == adv_month_key(T0) && fabs(u->rp[0].month_m[ADV_T_ALL] - len) < 0.01);
    // The same ride again: nothing new, no version bump, the activity counted.
    T_CHECK(apply(&w, 1, ADV_ACT_BIKE, T0 + 3600000, h, n, &a, 2) == CB_OK);
    T_CHECK(a.new_m[ADV_T_ALL] == 0 && u->version == 1 && u->n_activities == 2);
    // Owners: every segment is user 1's; super-unique is the whole ride.
    uint32_t su_n;
    float su_m;
    adv_world_super_unique(&w, u, &su_n, &su_m);
    T_CHECK(su_n == n && fabs(su_m - len) < 0.01);
    T_CHECK(adv_world_rank(&w, 0, false, ADV_T_BIKE, 1) == 1 && adv_world_rank(&w, 0, false, ADV_T_ALL, 1) == 1);
    T_CHECK(adv_world_rank(&w, 0, true, ADV_T_FOOT, 1) == 0);
    // User 2 runs the whole West region: achievements up to 99, first on the foot and combined boards,
    // and user 1's segments become everyone's.
    uint32_t n2 = region_hits(g, 0, h, 4096, 1.0);
    T_CHECK(apply(&w, 2, ADV_ACT_RUN, T0 + 60000, h, n2, &a, 3) == CB_OK);
    adv_user *u2 = adv_world_user(&w, 2);
    T_CHECK(u2->rp[0].ach[ADV_T_FOOT] == 0x1F && u2->rp[0].ach[ADV_T_ALL] == 0x1F && u2->rp[0].ach[ADV_T_BIKE] == 0);
    T_CHECK(adv_world_rank(&w, 0, false, ADV_T_FOOT, 2) == 1 && adv_world_rank(&w, 0, false, ADV_T_ALL, 2) == 1);
    T_CHECK(adv_world_rank(&w, 0, false, ADV_T_ALL, 1) == 2 && adv_world_rank(&w, 0, false, ADV_T_BIKE, 1) == 1);
    adv_world_super_unique(&w, u, &su_n, &su_m);
    T_CHECK(su_n == 0 && su_m == 0);
    adv_world_super_unique(&w, u2, &su_n, &su_m);
    T_CHECK_MSG(su_n == n2 - n, "su %u n2 %u n %u", su_n, n2, n);
    // A new month resets the month sums and the month board, the year keeps counting.
    int64_t next_month = T0 + 40LL * 86400000;
    uint32_t n3 = region_hits(g, 1, h, 4096, 0.5);
    T_CHECK(apply(&w, 1, ADV_ACT_BIKE, next_month, h, n3, &a, 4) == CB_OK);
    T_CHECK(u->n_rp == 2 && u->rp[1].region == 1 && u->rp[1].month_key == adv_month_key(next_month));
    T_CHECK(adv_world_rank(&w, 1, false, ADV_T_BIKE, 1) == 1);
    adv_board b;
    adv_world_board_copy(&w, 0, &b);
    T_CHECK(b.month[ADV_T_FOOT][0].user == 2 && b.month_key == adv_month_key(T0 + 60000));
    T_CHECK(adv_month_key(next_month) == adv_month_key(T0) + 1 && adv_year_key(T0) == 2025);
    // Totals from scratch agree with the running ones.
    float tot[3];
    adv_cov_total_m(&u->cov, g, tot);
    T_CHECK(fabs(tot[ADV_T_ALL] - u->unique_m[ADV_T_ALL]) < 0.05 && fabs(tot[ADV_T_BIKE] - u->unique_m[ADV_T_BIKE]) < 0.05);
    adv_world_free(&w);
}

static void test_boards(adv_graph *g) {
    adv_world w;
    T_CHECK(adv_world_init(&w, g) == CB_OK);
    // Five users with growing distances on region 0: the board keeps the top three, descending.
    for (uint32_t id = 1; id <= 5; id++) {
        adv_w_user_def d = { .id = id, .kind = ADV_UK_TOURIST, .name_len = 1, .home_lat = 418000000, .home_lon = -714000000, .name = "x" };
        T_CHECK(adv_world_user_define(&w, &d) == CB_OK);
        static adv_hit h[4096];
        uint32_t n = region_hits(g, 0, h, 4096, 0.15 * id);
        adv_applied a;
        T_CHECK(apply(&w, id, ADV_ACT_BIKE, T0, h, n, &a, id) == CB_OK);
    }
    adv_board b;
    adv_world_board_copy(&w, 0, &b);
    T_CHECK_MSG(b.month[ADV_T_BIKE][0].user == 5 && b.month[ADV_T_BIKE][1].user == 4 && b.month[ADV_T_BIKE][2].user == 3, "board %u %u %u",
                b.month[ADV_T_BIKE][0].user, b.month[ADV_T_BIKE][1].user, b.month[ADV_T_BIKE][2].user);
    T_CHECK(b.month[ADV_T_BIKE][0].m >= b.month[ADV_T_BIKE][1].m && b.month[ADV_T_BIKE][1].m >= b.month[ADV_T_BIKE][2].m);
    T_CHECK(adv_world_rank(&w, 0, false, ADV_T_BIKE, 1) == 0 && adv_world_rank(&w, 0, true, ADV_T_BIKE, 5) == 1);
    // Every segment of region 0 has been ridden by at least user 5 and user 4 (both covered 60%+): owners are MANY there.
    uint32_t many = 0, single = 0;
    for (uint32_t i = 0; i < g->h->n_segs; i++) {
        uint32_t o = atomic_load(&w.owners[i]);
        if (o == ADV_OWNER_MANY) many++;
        else if (o) single++;
    }
    T_CHECK(many > 0 && single > 0);
    adv_world_free(&w);
}

static void test_journal(adv_graph *g) {
    char db[512];
    fg_tmp_path(db, sizeof db, "unit_world_db");
    cb_store *s = NULL;
    T_CHECK(cb_store_open_sqlite(db, &s) == CB_OK && s);
    adv_world w;
    T_CHECK(adv_world_init(&w, g) == CB_OK);
    define_users(&w);
    adv_w_user_def d1 = { .id = 1, .kind = ADV_UK_EXPLORER, .name_len = 4, .home_lat = 418000000, .home_lon = -714000000, .name = "Mara" };
    adv_w_user_def d2 = { .id = 2, .kind = ADV_UK_RUNNER, .name_len = 3, .home_lat = 418000000, .home_lon = -714000000, .name = "Jon" };
    T_CHECK(adv_journal_user_put(s, &d1) == CB_OK && adv_journal_user_put(s, &d2) == CB_OK);
    static adv_hit h[4096];
    for (uint32_t act = 1; act <= 6; act++) {
        uint32_t user = act % 2 ? 1 : 2;
        uint32_t n = region_hits(g, (uint16_t)(act % 2), h, 4096, 0.2 * act);
        adv_applied a;
        T_CHECK(apply(&w, user, user == 1 ? ADV_ACT_BIKE : ADV_ACT_WALK, T0 + act * 86400000LL, h, n, &a, act) == CB_OK);
        adv_result r = { .activity = act, .user = user, .type = user == 1 ? ADV_ACT_BIKE : ADV_ACT_WALK, .start_ms = T0 + act * 86400000LL,
                         .gen_id = act, .n_points = 100, .matched_m = adv_hits_len_m(g, h, n), .accuracy = 0.9f, .cpu_us = 7, .hits = h, .n_hits = n };
        T_CHECK(adv_journal_act_put(s, &r) == CB_OK);
    }
    T_CHECK(adv_journal_bytes(db) > 0);
    // A second world from the journal alone equals the first.
    adv_world w2;
    T_CHECK(adv_world_init(&w2, g) == CB_OK);
    uint32_t recovered = 0;
    T_CHECK(adv_journal_recover(s, &w2, &recovered) == CB_OK);
    T_CHECK_MSG(recovered == 6, "recovered %u", recovered);
    T_CHECK(atomic_load(&w2.next_activity) == 7 && atomic_load(&w2.n_users) == 2);
    for (uint32_t id = 1; id <= 2; id++) {
        adv_user *a = adv_world_user(&w, id), *b = adv_world_user(&w2, id);
        T_CHECK(a && b && strcmp(a->name, b->name) == 0 && a->kind == b->kind);
        T_CHECK(a->n_activities == b->n_activities && a->version == b->version);
        T_CHECK(a->cov.n == b->cov.n && memcmp(a->cov.e, b->cov.e, a->cov.n * sizeof *a->cov.e) == 0);
        T_CHECK(fabs(a->unique_m[ADV_T_ALL] - b->unique_m[ADV_T_ALL]) < 0.01);
        T_CHECK(a->n_rp == b->n_rp && memcmp(a->rp, b->rp, a->n_rp * sizeof *a->rp) == 0);
        T_CHECK(a->first_ms == b->first_ms && a->last_ms == b->last_ms);
    }
    for (uint32_t i = 0; i < g->h->n_segs; i++) T_CHECK(atomic_load(&w.owners[i]) == atomic_load(&w2.owners[i]));
    adv_board b1, b2;
    adv_world_board_copy(&w, 0, &b1);
    adv_world_board_copy(&w2, 0, &b2);
    T_CHECK(memcmp(&b1, &b2, sizeof b1) == 0);
    T_CHECK(w2.acc_n == 6 && fabs(w2.acc_sum / 6 - 0.9) < 1e-6);
    adv_world_free(&w);
    adv_world_free(&w2);
    cb_store_close(s);
    unlink(db);
    char wal[600];
    snprintf(wal, sizeof wal, "%s-wal", db);
    unlink(wal);
    snprintf(wal, sizeof wal, "%s-shm", db);
    unlink(wal);
}


int main(void) {
    fg_opts o = { .cols = 6, .rows = 5, .regions = true, .foot_only_rows = true };
    char path[512];
    fg_tmp_path(path, sizeof path, "unit_world");
    adv_graph g;
    T_CHECK(fg_build(&o, path, &g) == 0);
    test_apply(&g);
    test_boards(&g);
    test_journal(&g);
    adv_graph_close(&g);
    unlink(path);
    return t_done("unit_world");
}
