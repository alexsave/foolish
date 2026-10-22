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

// The top roads of the combined view, each a name in a region (lib_fakegraph: a horizontal
// street is "Row j", a vertical one unnamed and never in the list): by users, then by the length
// anyone has; a road's length is its segments' in ITS region, counted once each, its users the
// most any of its segments has, and its seg one with that many.
static void check_top_roads(adv_world *w, const adv_graph *g) {
    adv_top_seg top[ADV_TOP_N];
    uint32_t nt = adv_world_top(w, ADV_T_ALL, top);
    // As many as there are named roads anyone has, up to ADV_TOP_N: (name, region) pairs.
    uint32_t named = 0;
    for (uint32_t k = 0; k < g->h->n_segs; k++) {
        if (!g->segs[k].name || !atomic_load(&w->all[k].users[ADV_T_ALL])) continue;
        bool first = true;
        for (uint32_t j = 0; j < k && first; j++)
            first = !(g->segs[j].name == g->segs[k].name && g->segs[j].region == g->segs[k].region && atomic_load(&w->all[j].users[ADV_T_ALL]));
        named += first;
    }
    T_CHECK_MSG(nt == (named < ADV_TOP_N ? named : ADV_TOP_N), "%u top roads, %u named roads", nt, named);
    bool grouped = false;
    for (uint32_t i = 0; i < nt; i++) {
        T_CHECK(top[i].users == 1 || top[i].users == 2);
        if (i > 0) T_CHECK(top[i].users < top[i - 1].users || (top[i].users == top[i - 1].users && top[i].len_m <= top[i - 1].len_m));
        const adv_seg *sg = &g->segs[top[i].seg];
        T_CHECK_MSG(sg->name != 0, "top road %u is unnamed", i);
        T_CHECK(atomic_load(&w->all[top[i].seg].users[ADV_T_ALL]) == top[i].users);
        float want = 0;
        uint32_t segs = 0;
        uint16_t most = 0;
        for (uint32_t k = 0; k < g->h->n_segs; k++) {
            uint16_t u16 = atomic_load(&w->all[k].users[ADV_T_ALL]);
            if (g->segs[k].region != sg->region || !u16) continue;
            bool same = sg->name ? g->segs[k].name == sg->name : k == top[i].seg;
            if (!same) continue;
            want += g->segs[k].len_m;
            segs++;
            if (u16 > most) most = u16;
        }
        T_CHECK_MSG(fabs(top[i].len_m - want) < 0.01 && top[i].n_segs == segs, "road %u: len %f want %f segs %u want %u", i, top[i].len_m, want, top[i].n_segs, segs);
        grouped |= segs > 1;
        T_CHECK_MSG(most == top[i].users, "road %u: users %u most %u", i, top[i].users, most);
    }
    T_CHECK(top[0].users == 2 && g->segs[top[0].seg].name != 0);
    T_CHECK(grouped);   // some road in the list is several segments of one name
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
    // Everyone: the union is the whole West region (user 2 ran all of it), on foot every segment
    // has one user, by bike the 30% user 1 rode, combined two users where both went and one
    // elsewhere; the combined length is the region's, the bike length user 1's ride, and each
    // apply that added something bumped the version (the repeat ride did not).
    {
        uint32_t one = 0, two = 0, bike1 = 0, foot1 = 0;
        for (uint32_t i = 0; i < g->h->n_segs; i++) {
            if (g->segs[i].region != 0) {
                T_CHECK(atomic_load(&w.all[i].users[ADV_T_ALL]) == 0 && atomic_load(&w.all[i].mask[ADV_T_ALL]) == 0);
                continue;
            }
            uint16_t ua = atomic_load(&w.all[i].users[ADV_T_ALL]), ub = atomic_load(&w.all[i].users[ADV_T_BIKE]), uf = atomic_load(&w.all[i].users[ADV_T_FOOT]);
            T_CHECK(atomic_load(&w.all[i].mask[ADV_T_ALL]) == 0xFFFF && atomic_load(&w.all[i].mask[ADV_T_FOOT]) == 0xFFFF);
            T_CHECK(uf == 1 && (ua == 1 || ua == 2) && ub == (ua == 2 ? 1 : 0));
            T_CHECK(atomic_load(&w.all[i].mask[ADV_T_BIKE]) == (ub ? 0xFFFF : 0));
            if (ua == 1) one++; else two++;
            bike1 += ub;
            foot1 += uf;
        }
        T_CHECK_MSG(two == n && one + two == n2, "two %u one %u n %u n2 %u", two, one, n, n2);
        T_CHECK(bike1 == n && foot1 == n2);
        T_CHECK(atomic_load(&w.all_segs[ADV_T_ALL]) == n2 && atomic_load(&w.all_segs[ADV_T_BIKE]) == n && atomic_load(&w.all_segs[ADV_T_FOOT]) == n2);
        T_CHECK_MSG(fabs(adv_world_all_m(&w, ADV_T_ALL) - g->regions[0].len_m[ADV_T_ALL]) < 0.05, "all %f region %f", adv_world_all_m(&w, ADV_T_ALL),
                    g->regions[0].len_m[ADV_T_ALL]);
        T_CHECK(fabs(adv_world_all_m(&w, ADV_T_BIKE) - len) < 0.05 && fabs(adv_world_all_region_m(&w, 0, ADV_T_BIKE) - len) < 0.05);
        T_CHECK(fabs(adv_world_all_region_m(&w, 0, ADV_T_ALL) - adv_world_all_m(&w, ADV_T_ALL)) < 0.01 && adv_world_all_region_m(&w, 1, ADV_T_ALL) == 0);
        T_CHECK_MSG(atomic_load(&w.all_version) == 2, "version %u", atomic_load(&w.all_version));
        // The top roads (check_top_roads): every West row has a two-user segment, so the rows come
        // first with 2; the unnamed columns are not in the list.
        check_top_roads(&w, g);
        adv_top_seg top[ADV_TOP_N];
        uint32_t nt = adv_world_top(&w, ADV_T_BIKE, top);
        T_CHECK(nt >= 1 && nt <= ADV_TOP_N && top[0].users == 1);
        adv_world w0;
        T_CHECK(adv_world_init(&w0, g) == CB_OK);
        T_CHECK(adv_world_top(&w0, ADV_T_ALL, top) == 0);
        adv_world_free(&w0);
        // A user is counted once per segment and type however many times it adds bits there:
        // user 1 bikes the first half of an East segment nobody has, user 2 bikes it whole (two
        // users, the union full), then user 1 bikes it whole: new bits for user 1, nothing new
        // for anyone, so the count stays 2, the length stays, and the version does not move.
        uint32_t y = UINT32_MAX;
        for (uint32_t i = 0; i < g->h->n_segs && y == UINT32_MAX; i++)
            if (g->segs[i].region == 1 && (g->segs[i].access & ADV_ACC_BIKE) && atomic_load(&w.all[i].users[ADV_T_ALL]) == 0) y = i;
        T_CHECK(y != UINT32_MAX);
        uint32_t v0 = atomic_load(&w.all_version);
        adv_hit half = { y, 0x00FF, 0 }, whole = { y, 0xFFFF, 0 };
        T_CHECK(apply(&w, 1, ADV_ACT_BIKE, T0 + 5000, &half, 1, &a, 10) == CB_OK);
        T_CHECK(atomic_load(&w.all[y].users[ADV_T_BIKE]) == 1 && atomic_load(&w.all[y].mask[ADV_T_BIKE]) == 0x00FF && atomic_load(&w.all_version) == v0 + 1);
        T_CHECK(apply(&w, 2, ADV_ACT_BIKE, T0 + 6000, &whole, 1, &a, 11) == CB_OK);
        T_CHECK(atomic_load(&w.all[y].users[ADV_T_BIKE]) == 2 && atomic_load(&w.all[y].mask[ADV_T_BIKE]) == 0xFFFF && atomic_load(&w.all_version) == v0 + 2);
        float before = adv_world_all_m(&w, ADV_T_BIKE);
        T_CHECK(apply(&w, 1, ADV_ACT_BIKE, T0 + 7000, &whole, 1, &a, 12) == CB_OK);
        T_CHECK(a.new_m[ADV_T_BIKE] > 0);   // new to user 1
        T_CHECK_MSG(atomic_load(&w.all[y].users[ADV_T_BIKE]) == 2 && atomic_load(&w.all[y].users[ADV_T_ALL]) == 2, "users %u", atomic_load(&w.all[y].users[ADV_T_BIKE]));
        T_CHECK(adv_world_all_m(&w, ADV_T_BIKE) == before);
        T_CHECK_MSG(atomic_load(&w.all_version) == v0 + 2, "version %u want %u", atomic_load(&w.all_version), v0 + 2);
    }
    // A new month resets the month sums and the month board, the year keeps counting.
    int64_t next_month = T0 + 40LL * 86400000;
    uint32_t n3 = region_hits(g, 1, h, 4096, 0.5);
    T_CHECK(apply(&w, 1, ADV_ACT_BIKE, next_month, h, n3, &a, 4) == CB_OK);
    T_CHECK(u->n_rp == 2 && u->rp[1].region == 1 && u->rp[1].month_key == adv_month_key(next_month));
    check_top_roads(&w, g);   // a row now runs through both regions: two roads, one per region
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
    // Everyone's words, the sums and the version come back the same from the journal.
    for (uint32_t i = 0; i < g->h->n_segs; i++)
        for (int t = 0; t < ADV_T_N; t++)
            T_CHECK(atomic_load(&w.all[i].mask[t]) == atomic_load(&w2.all[i].mask[t]) && atomic_load(&w.all[i].users[t]) == atomic_load(&w2.all[i].users[t]));
    for (int t = 0; t < ADV_T_N; t++) {
        T_CHECK(atomic_load(&w.all_cm[t]) == atomic_load(&w2.all_cm[t]) && atomic_load(&w.all_segs[t]) == atomic_load(&w2.all_segs[t]));
        T_CHECK(atomic_load(&w.all_region_cm[t]) == atomic_load(&w2.all_region_cm[t]) && atomic_load(&w.all_region_cm[ADV_T_N + t]) == atomic_load(&w2.all_region_cm[ADV_T_N + t]));
    }
    T_CHECK(atomic_load(&w.all_version) == atomic_load(&w2.all_version) && atomic_load(&w.all_version) > 0);
    T_CHECK(atomic_load(&w.all_cm[ADV_T_ALL]) > 0);
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
