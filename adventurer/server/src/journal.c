// journal.c - journal.h.
#include "journal.h"

#include "store/store_sql.h"
#include "util/err.h"
#include "util/log.h"

#include "trace.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *const adv_sql_stmts[ADV_SQL_N] = {
    [ADV_SQL_USER_PUT] = "INSERT OR REPLACE INTO adv_users (id, kind, name, home_lat, home_lon) VALUES (?1, ?2, ?3, ?4, ?5)",
    [ADV_SQL_USERS_ALL] = "SELECT id, kind, name, home_lat, home_lon FROM adv_users ORDER BY id",
    [ADV_SQL_ACT_PUT] = ("INSERT INTO adv_activities (id, user, type, flags, start_ms, gen_id, n_points, matched_m, accuracy, cpu_us, hits) "
                         "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)"),
    [ADV_SQL_ACTS_ALL] = "SELECT id, user, type, flags, start_ms, gen_id, n_points, matched_m, accuracy, cpu_us, hits FROM adv_activities ORDER BY id",
    [ADV_SQL_ACT_MAX] = "SELECT COALESCE(MAX(id), 0) FROM adv_activities",
};

static int user_put_fn(cb_sql *x, void *ud) {
    const adv_w_user_def *d = ud;
    sqlite3_stmt *st = cb_sql_stmt(x, ADV_SQL_USER_PUT);
    if (!st) return CB_E_DB;
    sqlite3_bind_int64(st, 1, d->id);
    sqlite3_bind_int(st, 2, d->kind);
    sqlite3_bind_text(st, 3, d->name, d->name_len, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, d->home_lat);
    sqlite3_bind_int(st, 5, d->home_lon);
    return cb_sql_run(x, st, "adv user put");
}

int adv_journal_user_put(cb_store *s, const adv_w_user_def *d) { return cb_sql_write(s, user_put_fn, (void *)d); }

static int act_put_fn(cb_sql *x, void *ud) {
    const adv_result *r = ud;
    sqlite3_stmt *st = cb_sql_stmt(x, ADV_SQL_ACT_PUT);
    if (!st) return CB_E_DB;
    sqlite3_bind_int64(st, 1, r->activity);
    sqlite3_bind_int64(st, 2, r->user);
    sqlite3_bind_int(st, 3, r->type);
    sqlite3_bind_int(st, 4, r->flags);
    sqlite3_bind_int64(st, 5, r->start_ms);
    sqlite3_bind_int64(st, 6, r->gen_id);
    sqlite3_bind_int64(st, 7, r->n_points);
    sqlite3_bind_double(st, 8, r->matched_m);
    sqlite3_bind_double(st, 9, r->accuracy);
    sqlite3_bind_int64(st, 10, r->cpu_us);
    sqlite3_bind_blob(st, 11, r->hits, (int)(r->n_hits * sizeof *r->hits), SQLITE_STATIC);
    return cb_sql_run(x, st, "adv activity put");
}

int adv_journal_act_put(cb_store *s, const adv_result *r) { return cb_sql_write(s, act_put_fn, (void *)r); }

typedef struct {
    adv_world *w;
    uint32_t   recovered, skipped;
    int        rc;
} recover_ctx;

static int recover_fn(cb_sql *x, void *ud) {
    recover_ctx *c = ud;
    sqlite3_stmt *st = cb_sql_stmt(x, ADV_SQL_USERS_ALL);
    if (!st) return CB_E_DB;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        adv_w_user_def d;
        memset(&d, 0, sizeof d);
        d.id = (uint32_t)sqlite3_column_int64(st, 0);
        d.kind = (uint8_t)sqlite3_column_int(st, 1);
        const unsigned char *name = sqlite3_column_text(st, 2);
        int n = sqlite3_column_bytes(st, 2);
        if (n >= ADV_W_NAME) n = ADV_W_NAME - 1;
        if (name && n > 0) memcpy(d.name, name, (size_t)n);
        d.name_len = (uint8_t)n;
        d.home_lat = sqlite3_column_int(st, 3);
        d.home_lon = sqlite3_column_int(st, 4);
        if (adv_world_user_define(c->w, &d) != CB_OK) c->skipped++;
    }
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) return cb_sql_fail(x, "adv users read", rc);
    st = cb_sql_stmt(x, ADV_SQL_ACTS_ALL);
    if (!st) return CB_E_DB;
    uint32_t max_id = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        adv_result r;
        memset(&r, 0, sizeof r);
        r.activity = (uint32_t)sqlite3_column_int64(st, 0);
        r.user = (uint32_t)sqlite3_column_int64(st, 1);
        r.type = (uint8_t)sqlite3_column_int(st, 2);
        r.flags = (uint8_t)sqlite3_column_int(st, 3);
        r.start_ms = sqlite3_column_int64(st, 4);
        r.gen_id = (uint32_t)sqlite3_column_int64(st, 5);
        r.n_points = (uint32_t)sqlite3_column_int64(st, 6);
        r.matched_m = (float)sqlite3_column_double(st, 7);
        r.accuracy = (float)sqlite3_column_double(st, 8);
        r.cpu_us = (uint32_t)sqlite3_column_int64(st, 9);
        const void *blob = sqlite3_column_blob(st, 10);
        int n = sqlite3_column_bytes(st, 10);
        if (r.activity > max_id) max_id = r.activity;
        if (n % (int)sizeof(adv_hit) != 0 || n < 0) { c->skipped++; continue; }
        // The blob is SQLite's, aligned enough for a memcpy only: copy into an aligned buffer.
        uint32_t n_hits = (uint32_t)n / sizeof(adv_hit);
        adv_hit *hits = n_hits ? malloc((size_t)n) : NULL;
        if (n_hits && !hits) return CB_E_NOMEM;
        if (n_hits) memcpy(hits, blob, (size_t)n);
        bool ok = true;
        for (uint32_t i = 0; i < n_hits; i++)
            if (hits[i].seg >= c->w->g->h->n_segs || (i > 0 && hits[i].seg <= hits[i - 1].seg)) ok = false;
        if (!ok || r.type < ADV_ACT_BIKE || r.type > ADV_ACT_WALK) { free(hits); c->skipped++; continue; }
        r.hits = hits;
        r.n_hits = n_hits;
        adv_user *u = adv_world_user(c->w, r.user);
        if (!u) { free(hits); c->skipped++; continue; }
        adv_applied a;
        pthread_mutex_t *l = adv_world_user_lock(c->w, r.user);
        pthread_mutex_lock(l);
        int arc = adv_world_apply(c->w, u, &r, &a);
        pthread_mutex_unlock(l);
        free(hits);
        if (arc != CB_OK) return arc;
        if (r.accuracy >= 0) {
            c->w->acc_sum += r.accuracy;
            c->w->acc_n++;
        }
        c->recovered++;
    }
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) return cb_sql_fail(x, "adv activities read", rc);
    atomic_store(&c->w->next_activity, max_id + 1);
    atomic_store(&c->w->activities, c->recovered);
    return CB_OK;
}

int adv_journal_recover(cb_store *s, adv_world *w, uint32_t *recovered) {
    recover_ctx c = { w, 0, 0, CB_OK };
    int rc = cb_sql_read(s, recover_fn, &c);
    if (c.skipped) CB_WARN("adv", "journal: %u rows skipped as unreadable", c.skipped);
    *recovered = c.recovered;
    return rc;
}

uint64_t adv_journal_bytes(const char *db_path) {
    struct stat st;
    if (!db_path || stat(db_path, &st) != 0) return 0;
    uint64_t n = (uint64_t)st.st_size;
    char wal[1200];
    snprintf(wal, sizeof wal, "%s-wal", db_path);
    if (stat(wal, &st) == 0) n += (uint64_t)st.st_size;
    return n;
}
