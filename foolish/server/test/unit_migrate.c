// unit_migrate.c - cbase-export's body without a table (store/migrate_rows.h): items shaped as
// production holds them (the item path's own layout) go into a fresh SQL store in the copy's order,
// and every one reads back the same by its row's natural key, every count agrees; a changed item is
// caught, an unknown family and a duplicate name fail loudly.
#include "t.h"

#include "lib_proc.h"
#include "store/migrate_rows.h"
#include "store/store_import.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_dir[256];

#define U1 "11111111-1111-4111-8111-111111111111"
#define U2 "22222222-2222-4222-8222-222222222222"
#define SN "33333333-3333-4333-8333-333333333333"
#define CH "44444444-4444-4444-8444-444444444444"
#define HASH "$2b$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234"

static const uint8_t STATE[] = { 1, 2, 3 }, ROSTER[] = { 9, 8 }, LOGS[] = { 0xaa, 0xbb, 0xcc }, MOVES[] = { 7, 7 },
                     EXTRAS[] = { 5 };

// An item from attributes.
static void mk(cb_item *it, const cb_av *a, int n) {
    cb_item_init(it);
    for (int i = 0; i < n; i++) cb_item_add(it, &a[i]);
}

static bool keep_summary(const cb_snapshot_row *r, void *ud) {
    cb_snapshot_row *o = ud;
    o->sum = r->sum;
    o->rank = r->rank;
    return false;
}

// The copy's order, as export.c sorts the keys: the accounts and emails, the rating, the game's seat then
// its row, the replay's history rows then the replay, the chat, the scan marker.
static const int ORDER[] = { 0, 1, 2, 10, 3, 4, 5, 11, 6, 7, 8, 9 };
static void put_all(cb_mig *m, cb_item *items, int n) {
    for (int i = 0; i < n; i++) T_EQ_INT(cb_mig_put(m, &items[ORDER[i]]), CB_OK);
    T_EQ_INT(cb_mig_finish(m), CB_OK);
}

static void items_become_rows(void) {
    char path[320];
    snprintf(path, sizeof path, "%s/m.sqlite", g_dir);
    cb_store *s = NULL;
    T_EQ_INT(cb_store_open_sqlite(path, &s), CB_OK);
    if (!s) return;
    T_EQ_INT(CB_ROWS_CALL(s, bots_replace, NULL, 0), CB_OK);
    const char *players[] = { U1, U2 };
    enum { N = 12 };
    cb_item it[N];
    // In the copy's order (cb_mig_rank, then the key).
    mk(&it[0], (cb_av[]){ CB_AVS("pk", "USER#" U1), CB_AVS("sk", "PROFILE"), CB_AVS("id", U1), CB_AVS("email", "a@x.io"),
                         CB_AVS("password_hash", HASH), CB_AVS("username", "ANN"), CB_AVN("created_at", 5) }, 7);
    mk(&it[1], (cb_av[]){ CB_AVS("pk", "USER#" U2), CB_AVS("sk", "PROFILE"), CB_AVS("id", U2), CB_AVS("email", "b@x.io"),
                         CB_AVS("password_hash", HASH), CB_AVS("username", "BOB"), CB_AVN("created_at", 6),
                         CB_AVN("session_gen", 2) }, 8);
    mk(&it[2], (cb_av[]){ CB_AVS("pk", "EMAIL#a@x.io"), CB_AVS("sk", "USER"), CB_AVS("user_id", U1) }, 3);
    mk(&it[3], (cb_av[]){ CB_AVS("pk", "USER#" U1), CB_AVS("sk", "RATING"), CB_AVS("user_id", U1), CB_AVS("username", "ANN"),
                         CB_AVN("elo_rating", 1010), CB_AVN("previous_elo", 1000), CB_AVN("games_played", 1),
                         CB_AVN("updated_at", 9) }, 8);
    mk(&it[4], (cb_av[]){ CB_AVS("pk", "GAME#ab12cd"), CB_AVS("sk", "MEMBER#" U1), CB_AVS("game_id", "ab12cd"),
                         CB_AVS("user_id", U1) }, 4);
    mk(&it[5], (cb_av[]){ CB_AVS("pk", "GAME#ab12cd"), CB_AVS("sk", "ROW"), CB_AVS("id", "ab12cd"), CB_AVN("status", 1),
                         CB_AVB("state", STATE, 3), CB_AVB("roster", ROSTER, 2), CB_AVB("logs_packed", LOGS, 3),
                         CB_AVN("version", 4), CB_AVN("created_at", 1), CB_AVN("updated_at", 2), CB_AVN("last_commit_at", 2) }, 11);
    mk(&it[6], (cb_av[]){ CB_AVS("pk", "SNAP#" SN), CB_AVS("sk", "PLAYER#" U2), CB_AVS("id", SN), CB_AVS("game_id", "ab12cd"),
                         CB_AVN("created_at", 8), CB_AVN("n_players", 2), CB_AVN("seat", 1), CB_AVN("rank", 1),
                         CB_AVN("fool", 0), CB_AVN("dur", 61) }, 10);
    mk(&it[7], (cb_av[]){ CB_AVS("pk", "SNAP#" SN), CB_AVS("sk", "SNAP"), CB_AVS("id", SN), CB_AVS("game_id", "ab12cd"),
                         CB_AVSL("player_ids", players, 2), CB_AVB("moves", MOVES, 2), CB_AVB("extras", EXTRAS, 1),
                         CB_AVN("created_at", 8) }, 8);
    mk(&it[8], (cb_av[]){ CB_AVS("pk", "CHAT#ab12cd"), CB_AVS("sk", "000000000000009#" CH), CB_AVS("id", CH),
                         CB_AVS("game_id", "ab12cd"), CB_AVS("user_id", U1), CB_AVS("message", "it's me"),
                         CB_AVBOOL("is_system", false), CB_AVN("created_at", 9) }, 8);
    mk(&it[10], (cb_av[]){ CB_AVS("pk", "EMAIL#b@x.io"), CB_AVS("sk", "USER"), CB_AVS("user_id", U2) }, 3);
    mk(&it[11], (cb_av[]){ CB_AVS("pk", "SNAP#" SN), CB_AVS("sk", "PLAYER#" U1), CB_AVS("id", SN), CB_AVS("game_id", "ab12cd"),
                          CB_AVN("created_at", 8), CB_AVN("n_players", 2), CB_AVN("seat", 0), CB_AVN("rank", 2),
                          CB_AVN("fool", 0), CB_AVN("dur", 61) }, 10);
    mk(&it[9], (cb_av[]){ CB_AVS("pk", "SCAN"), CB_AVS("sk", "GAME#ab12cd"), CB_AVS("game_id", "ab12cd") }, 3);
    cb_mig *m = cb_mig_new(s, 100, stderr);
    put_all(m, it, N);
    for (int i = 0; i < N; i++) T_EQ_INT(cb_mig_same(m, &it[i]), 1);
    T_EQ_INT(cb_mig_report(m, stdout), 0);
    // The summary came from the pointer: BOB went out first, ANN was the fool.
    cb_snapshot_query q = { .viewer_id = U2, .summary_only = true, .limit = 1 };
    cb_snapshot_row got = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, keep_summary, &got), 1);
    T_CHECK(got.sum.has);
    T_EQ_INT(got.sum.fool, 0);
    T_EQ_INT(got.sum.duration_s, 61);
    T_EQ_INT(got.rank, 1);
    // A changed item (the game's state) no longer reads back the same.
    cb_item changed;
    mk(&changed, (cb_av[]){ CB_AVS("pk", "GAME#ab12cd"), CB_AVS("sk", "ROW"), CB_AVS("id", "ab12cd"), CB_AVN("status", 1),
                           CB_AVB("state", ROSTER, 2), CB_AVB("roster", ROSTER, 2), CB_AVB("logs_packed", LOGS, 3),
                           CB_AVN("version", 4), CB_AVN("created_at", 1), CB_AVN("updated_at", 2),
                           CB_AVN("last_commit_at", 2) }, 11);
    T_EQ_INT(cb_mig_same(m, &changed), 0);
    cb_item_free(&changed);
    // An unknown family, and a name another account holds, fail loudly.
    cb_item bad;
    mk(&bad, (cb_av[]){ CB_AVS("pk", "LOG#ab12cd"), CB_AVS("sk", "00000000000000000001") }, 2);
    T_CHECK(cb_mig_put(m, &bad) != CB_OK);
    cb_item_free(&bad);
    mk(&bad, (cb_av[]){ CB_AVS("pk", "USER#55555555-5555-4555-8555-555555555555"), CB_AVS("sk", "PROFILE"),
                       CB_AVS("id", "55555555-5555-4555-8555-555555555555"), CB_AVS("email", "c@x.io"),
                       CB_AVS("password_hash", HASH), CB_AVS("username", "ANN"), CB_AVN("created_at", 7) }, 7);
    T_CHECK(cb_mig_put(m, &bad) != CB_OK);
    cb_item_free(&bad);
    T_CHECK(cb_mig_report(m, stdout) != 0);
    cb_mig_free(m);
    for (int i = 0; i < N; i++) cb_item_free(&it[i]);
    cb_store_close(s);
}

int main(void) {
    if (tp_tempdir(g_dir, sizeof g_dir) != 0) return 1;
    T_RUN(items_become_rows);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) return 1;
    return T_DONE();
}
