// migrate_merge.c - the transform step of cbase-export (migrate_merge.h).
#include "store/migrate_merge.h"

#include "app/v1_http.h"
#include "auth/names.h"
#include "roster.h"
#include "store/store.h"
#include "table.h"
#include "util/err.h"
#include "util/uuid.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char id[16];
    char          key[512];
    int64_t       created;
    char         *hash, *name, *email;
} acct;

typedef struct {
    unsigned char from[16], to[16];
    char          from_email[CB_V1_EMAIL_BYTES], to_email[CB_V1_EMAIL_BYTES];
} remap;

typedef struct {
    sqlite3 *db;
    FILE    *log;
    acct    *a;
    int      n;
    remap   *r;
    int      nr;
    cb_mig_tx *tx;
    bool     fail;
} ctx;

static int by_key(const void *x, const void *y) {
    const acct *a = x, *b = y;
    int c = strcmp(a->key, b->key);
    if (c) return c;
    if (a->created != b->created) return a->created < b->created ? -1 : 1;
    return memcmp(a->id, b->id, 16);
}

static int by_id(const void *x, const void *y) { return memcmp(((const acct *)x)->id, ((const acct *)y)->id, 16); }

static bool run(ctx *c, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(c->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(c->log, "transform: %s: %s\n", sql, err ? err : "?");
        sqlite3_free(err);
        c->fail = true;
        return false;
    }
    return true;
}

static sqlite3_stmt *prep(ctx *c, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(c->db, sql, -1, &s, NULL) != SQLITE_OK) {
        fprintf(c->log, "transform: %s: %s\n", sql, sqlite3_errmsg(c->db));
        c->fail = true;
        return NULL;
    }
    return s;
}

// One statement with up to three blob/blob/text binds; rows changed.
static long exec_b(ctx *c, const char *sql, const unsigned char *b1, const unsigned char *b2) {
    sqlite3_stmt *s = prep(c, sql);
    if (!s) return 0;
    if (b1) sqlite3_bind_blob(s, 1, b1, 16, SQLITE_STATIC);
    if (b2) sqlite3_bind_blob(s, 2, b2, 16, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    long n = rc == SQLITE_DONE ? sqlite3_changes(c->db) : 0;
    if (rc != SQLITE_DONE) { fprintf(c->log, "transform: %s: %s\n", sql, sqlite3_errmsg(c->db)); c->fail = true; }
    sqlite3_finalize(s);
    return n;
}

static long count(ctx *c, const char *sql) {
    sqlite3_stmt *s = prep(c, sql);
    long n = -1;
    if (s && sqlite3_step(s) == SQLITE_ROW) n = (long)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

static const remap *find(const ctx *c, const unsigned char *id) {
    for (int i = 0; i < c->nr; i++) if (!memcmp(c->r[i].from, id, 16)) return &c->r[i];
    return NULL;
}

static bool is_user(const ctx *c, const unsigned char *id) {
    acct k;
    memcpy(k.id, id, 16);
    return bsearch(&k, c->a, (size_t)c->n, sizeof *c->a, by_id) != NULL;
}

static char *xdup(const unsigned char *t) { return strdup(t ? (const char *)t : ""); }

// Accounts, sorted by id for lookup; groups found on a copy sorted by key.
static bool load_accounts(ctx *c) {
    long total = count(c, "SELECT count(*) FROM users");
    if (total < 0) return false;
    c->a = calloc((size_t)total + 1, sizeof *c->a);
    sqlite3_stmt *s = prep(c, "SELECT id, username, created_at, password_hash, email FROM users");
    if (!c->a || !s) return false;
    while (sqlite3_step(s) == SQLITE_ROW && c->n < total) {
        acct *u = &c->a[c->n++];
        if (sqlite3_column_bytes(s, 0) != 16) { c->fail = true; continue; }
        memcpy(u->id, sqlite3_column_blob(s, 0), 16);
        u->name = xdup(sqlite3_column_text(s, 1));
        u->created = sqlite3_column_int64(s, 2);
        u->hash = xdup(sqlite3_column_text(s, 3));
        u->email = xdup(sqlite3_column_text(s, 4));
        if (u->name[0] && cb_name_key(u->name, strlen(u->name), u->key, sizeof u->key) < 0) c->fail = true;
        if (!u->name[0]) snprintf(u->key, sizeof u->key, "\x01%02x%02x%02x%02x%02x%02x%02x%02x", u->id[0], u->id[1], u->id[2], u->id[3], u->id[4], u->id[5], u->id[6], u->id[7]);   // no name: never grouped
    }
    sqlite3_finalize(s);
    return !c->fail;
}

static void email_now(const acct *u, char out[CB_V1_EMAIL_BYTES]) {
    char canon[512];
    int n = cb_name_case(u->name, strlen(u->name), canon, sizeof canon);
    if (n < 0) { memcpy(canon, u->name, strlen(u->name) + 1); n = (int)strlen(u->name); }
    cb_v1_email_of(canon, (size_t)n, out);
}

static void text_of(const unsigned char *id, char out[CB_UUID_LEN + 1]) { cb_uuid_unpack(id, out); }

// Replaces merged-away ids in a roster; classifies the result.
//  0 unchanged, 1 changed (new bytes in *r), 2 self-vs-self (delete), 3 unexpected duplicate
static int roster_fix(const ctx *c, Roster *r) {
    bool changed = false;
    for (int i = 0; i < r->n; i++) {
        unsigned char b[16];
        char id[CB_UUID_LEN + 1];
        if (r->seats[i].id_len != CB_UUID_LEN) continue;
        memcpy(id, r->seats[i].id, CB_UUID_LEN);
        id[CB_UUID_LEN] = 0;
        if (!cb_uuid_pack(id, b)) continue;
        const remap *m = find(c, b);
        if (!m) continue;
        text_of(m->to, id);
        memcpy(r->seats[i].id, id, CB_UUID_LEN);
        changed = true;
    }
    if (!changed) return 0;
    bool dup_id = false, all_same = true;
    int first = -1;
    for (int i = 0; i < r->n; i++) {
        for (int j = i + 1; j < r->n; j++)
            if (r->seats[i].id_len == r->seats[j].id_len && !memcmp(r->seats[i].id, r->seats[j].id, r->seats[i].id_len)) dup_id = true;
        if (r->seats[i].brain_len) continue;
        if (first < 0) first = i;
        else if (r->seats[i].id_len != r->seats[first].id_len || memcmp(r->seats[i].id, r->seats[first].id, r->seats[i].id_len)) all_same = false;
    }
    if (!dup_id) return 1;
    return all_same ? 2 : 3;
}

static void games_pass(ctx *c) {
    sqlite3_stmt *s = prep(c, "SELECT id, roster FROM games");
    sqlite3_stmt *up = prep(c, "UPDATE games SET roster = ?2 WHERE id = ?1");
    sqlite3_stmt *gone = prep(c, "INSERT OR IGNORE INTO _gone_games VALUES (?1)");
    if (!s || !up || !gone) return;
    while (sqlite3_step(s) == SQLITE_ROW) {
        Roster r;
        const unsigned char *rb = sqlite3_column_blob(s, 1);
        if (sqlite3_column_bytes(s, 1) != ROSTER_BYTES || roster_decode(&r, rb, ROSTER_BYTES) != ROSTER_OK) {
            fprintf(c->log, "transform: a game's roster does not decode\n");
            c->tx->unexpected++;
            continue;
        }
        int k = roster_fix(c, &r);
        if (k == 0) continue;
        if (k == 3) { fprintf(c->log, "transform: game %s seats the merged user beside another human\n", sqlite3_column_text(s, 0)); c->tx->unexpected++; continue; }
        if (k == 2) {
            sqlite3_bind_text(gone, 1, (const char *)sqlite3_column_text(s, 0), -1, SQLITE_TRANSIENT);
            sqlite3_step(gone);
            sqlite3_reset(gone);
            continue;
        }
        unsigned char out[ROSTER_BYTES];
        if (roster_encode(&r, out, sizeof out) != ROSTER_BYTES) { c->tx->unexpected++; continue; }
        sqlite3_bind_text(up, 1, (const char *)sqlite3_column_text(s, 0), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(up, 2, out, ROSTER_BYTES, SQLITE_STATIC);
        sqlite3_step(up);
        sqlite3_reset(up);
        c->tx->rosters_repointed++;
    }
    sqlite3_finalize(s);
    sqlite3_finalize(up);
    sqlite3_finalize(gone);
}

static void replays_pass(ctx *c) {
    sqlite3_stmt *s = prep(c, "SELECT id, game_id, players FROM replays");
    sqlite3_stmt *up = prep(c, "UPDATE replays SET players = ?2 WHERE id = ?1");
    sqlite3_stmt *gone = prep(c, "INSERT OR IGNORE INTO _gone_replays VALUES (?1)");
    if (!s || !up || !gone) return;
    while (sqlite3_step(s) == SQLITE_ROW) {
        int len = sqlite3_column_bytes(s, 2);
        const unsigned char *p = sqlite3_column_blob(s, 2);
        unsigned char buf[16 * 64];
        if (len % 16 || len > (int)sizeof buf) { c->tx->unexpected++; continue; }
        memcpy(buf, p, (size_t)len);
        bool changed = false;
        for (int i = 0; i < len; i += 16) {
            const remap *m = find(c, buf + i);
            if (m) { memcpy(buf + i, m->to, 16); changed = true; }
        }
        bool in_gone = false;
        if (sqlite3_column_type(s, 1) == SQLITE_TEXT) {
            sqlite3_stmt *q = prep(c, "SELECT 1 FROM _gone_games WHERE game_id = ?1");
            if (q) {
                sqlite3_bind_text(q, 1, (const char *)sqlite3_column_text(s, 1), -1, SQLITE_TRANSIENT);
                in_gone = sqlite3_step(q) == SQLITE_ROW;
                sqlite3_finalize(q);
            }
        }
        bool dup_human = false, all_same = true;
        const unsigned char *first = NULL;
        for (int i = 0; i < len; i += 16) {
            if (!is_user(c, buf + i)) continue;
            if (!first) first = buf + i;
            else if (memcmp(first, buf + i, 16)) all_same = false;
            for (int j = i + 16; j < len; j += 16) if (!memcmp(buf + i, buf + j, 16)) dup_human = true;
        }
        if (in_gone || (changed && dup_human && all_same)) {
            sqlite3_bind_blob(gone, 1, sqlite3_column_blob(s, 0), 16, SQLITE_TRANSIENT);
            sqlite3_step(gone);
            sqlite3_reset(gone);
        } else if (changed && dup_human) { c->tx->unexpected++; fprintf(c->log, "transform: a replay seats the merged user beside another human\n"); }
        else if (changed) {
            sqlite3_bind_blob(up, 1, sqlite3_column_blob(s, 0), 16, SQLITE_TRANSIENT);
            sqlite3_bind_blob(up, 2, buf, len, SQLITE_STATIC);
            sqlite3_step(up);
            sqlite3_reset(up);
            c->tx->replays_repointed++;
        }
    }
    sqlite3_finalize(s);
    sqlite3_finalize(up);
    sqlite3_finalize(gone);
}

static const char *const KNOWN[] = { "users", "sessions", "refresh_tokens", "signin_fails", "games", "game_moves", "game_members", "game_bots",
                                     "bots", "ratings", "replays", "replay_players", "chat", "_gone_games", "_gone_replays" };

static void unknown_tables(ctx *c) {
    sqlite3_stmt *s = prep(c, "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'");
    while (s && sqlite3_step(s) == SQLITE_ROW) {
        bool ok = false;
        for (size_t i = 0; i < sizeof KNOWN / sizeof *KNOWN; i++) if (!strcmp(KNOWN[i], (const char *)sqlite3_column_text(s, 0))) ok = true;
        if (!ok) { fprintf(c->log, "transform: a table the transform does not know: %s\n", sqlite3_column_text(s, 0)); c->tx->unexpected++; }
    }
    sqlite3_finalize(s);
}

static void body(ctx *c, unsigned flags) {
    cb_mig_tx *t = c->tx;
    if (!run(c, "CREATE TEMP TABLE _gone_games (game_id TEXT PRIMARY KEY)") || !run(c, "CREATE TEMP TABLE _gone_replays (id BLOB PRIMARY KEY)")) return;
    unknown_tables(c);
    if (!load_accounts(c)) return;
    long users0 = c->n;
    acct *sorted = malloc((size_t)c->n * sizeof *sorted + 1);
    c->r = calloc((size_t)c->n + 1, sizeof *c->r);
    if (!sorted || !c->r) { c->fail = true; free(sorted); return; }
    memcpy(sorted, c->a, (size_t)c->n * sizeof *sorted);
    qsort(sorted, (size_t)c->n, sizeof *sorted, by_key);
    qsort(c->a, (size_t)c->n, sizeof *c->a, by_id);
    char em[CB_V1_EMAIL_BYTES];
    for (int i = 0; i < c->n; i++) {   // informational: accounts whose email is not today's transform
        if (!sorted[i].name[0]) continue;
        email_now(&sorted[i], em);
        if (strcmp(em, sorted[i].email)) t->email_mismatch++;
    }
    struct { acct *kept, *newest; } *grp = calloc((size_t)c->n + 1, sizeof *grp);
    int ng = 0;
    if (flags & CB_MIG_MERGE_CASE_DUPLICATES) {
        for (int i = 0; i < c->n;) {
            int j = i + 1;
            while (j < c->n && !strcmp(sorted[j].key, sorted[i].key)) j++;
            if (j - i > 1) {
                acct *kept = &sorted[i], *newest = &sorted[i];
                for (int k = i + 1; k < j; k++) {
                    if (sorted[k].created > newest->created) newest = &sorted[k];
                    remap *m = &c->r[c->nr++];
                    memcpy(m->from, sorted[k].id, 16);
                    memcpy(m->to, kept->id, 16);
                    snprintf(m->from_email, sizeof m->from_email, "%s", sorted[k].email);
                }
                grp[ng].kept = kept;
                grp[ng++].newest = newest;
                t->groups++;
            }
            i = j;
        }
    }
    t->merged_away = c->nr;
    long games0 = count(c, "SELECT count(*) FROM games"), replays0 = count(c, "SELECT count(*) FROM replays");
    long members0 = count(c, "SELECT count(*) FROM game_members"), chat0 = count(c, "SELECT count(*) FROM chat");
    long rp0 = count(c, "SELECT count(*) FROM replay_players");
    if (c->nr) {
        games_pass(c);
        replays_pass(c);
        // self-vs-self games leave entirely
        run(c, "INSERT OR IGNORE INTO _gone_replays SELECT id FROM replays WHERE game_id IN (SELECT game_id FROM _gone_games)");
    }
    if (!c->fail && c->nr) {
        sqlite3_stmt *q;
#define DEL(field, sql) do { q = prep(c, sql); if (q) { sqlite3_step(q); t->field += sqlite3_changes(c->db); sqlite3_finalize(q); } } while (0)
        DEL(replay_players_deleted, "DELETE FROM replay_players WHERE replay_id IN (SELECT id FROM _gone_replays) OR replay_id IN (SELECT id FROM replays WHERE game_id IN (SELECT game_id FROM _gone_games))");
        DEL(replays_deleted, "DELETE FROM replays WHERE id IN (SELECT id FROM _gone_replays)");
        DEL(moves_deleted, "DELETE FROM game_moves WHERE game_id IN (SELECT game_id FROM _gone_games)");
        DEL(members_deleted, "DELETE FROM game_members WHERE game_id IN (SELECT game_id FROM _gone_games)");
        DEL(bots_deleted, "DELETE FROM game_bots WHERE game_id IN (SELECT game_id FROM _gone_games)");
        DEL(chat_deleted, "DELETE FROM chat WHERE game_id IN (SELECT game_id FROM _gone_games)");
        DEL(games_deleted, "DELETE FROM games WHERE id IN (SELECT game_id FROM _gone_games)");
#undef DEL
        for (int i = 0; i < c->nr; i++) {
            const unsigned char *f = c->r[i].from, *to = c->r[i].to;
            t->members_repointed += exec_b(c, "UPDATE OR IGNORE game_members SET user_id = ?2 WHERE user_id = ?1", f, to);
            t->members_deduped += exec_b(c, "DELETE FROM game_members WHERE user_id = ?1", f, NULL);
            t->chat_repointed += exec_b(c, "UPDATE chat SET user_id = ?2 WHERE user_id = ?1", f, to);
            t->replay_players_repointed += exec_b(c, "UPDATE OR IGNORE replay_players SET user_id = ?2 WHERE user_id = ?1", f, to);
            if (exec_b(c, "DELETE FROM replay_players WHERE user_id = ?1", f, NULL)) { t->unexpected++; fprintf(c->log, "transform: replay_players clash\n"); }
            t->sessions_dropped += exec_b(c, "DELETE FROM sessions WHERE user_id = ?1", f, NULL);
            t->refresh_dropped += exec_b(c, "DELETE FROM refresh_tokens WHERE user_id = ?1", f, NULL);
            t->ratings_dropped += exec_b(c, "DELETE FROM ratings WHERE user_id = ?1", f, NULL);
            exec_b(c, "DELETE FROM users WHERE id = ?1", f, NULL);
        }
        for (int i = 0; i < ng; i++) {
            acct *k = grp[i].kept, *nw = grp[i].newest;
            char email[CB_V1_EMAIL_BYTES];
            email_now(k, email);
            // sign-in failures are counted by email: the kept account's old one and each merged-away one go
            sqlite3_stmt *u = prep(c, "UPDATE users SET password_hash = ?2, email = ?3 WHERE id = ?1");
            sqlite3_stmt *d = prep(c, "DELETE FROM signin_fails WHERE email = ?1");
            if (!u || !d) break;
            sqlite3_bind_blob(u, 1, k->id, 16, SQLITE_STATIC);
            sqlite3_bind_text(u, 2, nw->hash, -1, SQLITE_STATIC);
            sqlite3_bind_text(u, 3, email, -1, SQLITE_STATIC);
            if (sqlite3_step(u) != SQLITE_DONE) { fprintf(c->log, "transform: kept account update: %s\n", sqlite3_errmsg(c->db)); c->fail = true; }
            if (nw != k) t->passwords_taken++;
            if (strcmp(email, k->email)) t->emails_changed++;
            const char *olds[2] = { k->email, email };
            for (int e = 0; e < 2; e++) { sqlite3_bind_text(d, 1, olds[e], -1, SQLITE_STATIC); sqlite3_step(d); t->fails_dropped += sqlite3_changes(c->db); sqlite3_reset(d); }
            for (int r = 0; r < c->nr; r++) if (!memcmp(c->r[r].to, k->id, 16)) { sqlite3_bind_text(d, 1, c->r[r].from_email, -1, SQLITE_STATIC); sqlite3_step(d); t->fails_dropped += sqlite3_changes(c->db); sqlite3_reset(d); }
            sqlite3_finalize(u);
            sqlite3_finalize(d);
        }
    }
    if (!c->fail && (flags & CB_MIG_RESET_RATINGS)) {
        char sql[400];
        snprintf(sql, sizeof sql, "UPDATE ratings SET elo_rating = %d, previous_elo = %d, games_played = 0, updated_at = COALESCE((SELECT created_at FROM users u WHERE u.id = ratings.user_id), updated_at)", TABLE_RATING_START, TABLE_RATING_START);
        if (run(c, sql)) t->ratings_reset = sqlite3_changes(c->db);
    }
    // the unique index the copy ran without comes back: a group left over refuses it
    if (!c->fail) run(c, "CREATE UNIQUE INDEX IF NOT EXISTS users_username_key ON users(username_key)");
    // CHECKS: nothing refers to a merged-away id; every table is what the transform says it should be
    for (int i = 0; i < c->nr && !c->fail; i++) {
        static const char *const Q[] = { "SELECT count(*) FROM users WHERE id = ?1", "SELECT count(*) FROM game_members WHERE user_id = ?1",
            "SELECT count(*) FROM chat WHERE user_id = ?1", "SELECT count(*) FROM replay_players WHERE user_id = ?1", "SELECT count(*) FROM ratings WHERE user_id = ?1",
            "SELECT count(*) FROM sessions WHERE user_id = ?1", "SELECT count(*) FROM refresh_tokens WHERE user_id = ?1",
            "SELECT count(*) FROM replays WHERE instr(players, ?1) > 0" };
        for (size_t q = 0; q < sizeof Q / sizeof *Q; q++) {
            sqlite3_stmt *s = prep(c, Q[q]);
            if (!s) break;
            sqlite3_bind_blob(s, 1, c->r[i].from, 16, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int64(s, 0)) { fprintf(c->log, "transform: a merged-away id is still referenced (%zu)\n", q); t->unexpected++; }
            sqlite3_finalize(s);
        }
    }
    {
        long u1 = count(c, "SELECT count(*) FROM users"), g1 = count(c, "SELECT count(*) FROM games"), r1 = count(c, "SELECT count(*) FROM replays");
        long m1 = count(c, "SELECT count(*) FROM game_members"), ch1 = count(c, "SELECT count(*) FROM chat"), p1 = count(c, "SELECT count(*) FROM replay_players");
        if (u1 != users0 - t->merged_away || g1 != games0 - t->games_deleted || r1 != replays0 - t->replays_deleted ||
            m1 != members0 - t->members_deleted - t->members_deduped || ch1 != chat0 - t->chat_deleted ||
            p1 != rp0 - t->replay_players_deleted) { fprintf(c->log, "transform: a table's count is not what the transform explains\n"); t->unexpected++; }
        fprintf(c->log, "transform: users %ld -> %ld, games %ld -> %ld, replays %ld -> %ld, game_members %ld -> %ld, chat %ld -> %ld, replay_players %ld -> %ld\n",
                users0, u1, games0, g1, replays0, r1, members0, m1, chat0, ch1, rp0, p1);
    }
    for (int i = 0; i < c->n; i++) { free(c->a[i].name); free(c->a[i].hash); free(c->a[i].email); }
    free(sorted);
    free(grp);
}

int cb_mig_transform(const char *path, unsigned flags, FILE *log, cb_mig_tx *out) {
    cb_mig_tx local;
    cb_mig_tx *t = out ? out : &local;
    memset(t, 0, sizeof *t);
    ctx c = { .log = log ? log : stderr, .tx = t };
    if (sqlite3_open(path, &c.db) != SQLITE_OK) { sqlite3_close(c.db); return CB_E_DB; }
    sqlite3_busy_timeout(c.db, 5000);
    int rc = CB_OK;
    if (run(&c, "BEGIN IMMEDIATE")) {
        body(&c, flags);
        if (c.fail || t->unexpected) { run(&c, "ROLLBACK"); rc = CB_E_INVALID; }
        else if (!run(&c, "COMMIT")) rc = CB_E_DB;
    } else rc = CB_E_DB;
    FILE *o = c.log;
    fprintf(o, "transform: %ld duplicate groups, %ld accounts merged away, %ld passwords taken from a newer account, %ld emails set to today's transform\n"
               "transform: re-pointed: game_members %ld (+%ld already seated, dropped), roster seats in %ld games, replays %ld, replay_players %ld, chat %ld\n"
               "transform: dropped with the merged-away accounts: sessions %ld, refresh tokens %ld, sign-in failures %ld, ratings %ld\n"
               "transform: self-vs-self games deleted %ld (moves %ld, members %ld, bot links %ld, chat %ld, replays %ld, replay_players %ld)\n"
               "transform: ratings reset %ld; %ld other accounts' emails differ from today's transform (left as they are); unexpected %ld\n",
            t->groups, t->merged_away, t->passwords_taken, t->emails_changed, t->members_repointed, t->members_deduped, t->rosters_repointed,
            t->replays_repointed, t->replay_players_repointed, t->chat_repointed, t->sessions_dropped, t->refresh_dropped, t->fails_dropped,
            t->ratings_dropped, t->games_deleted, t->moves_deleted, t->members_deleted, t->bots_deleted, t->chat_deleted, t->replays_deleted,
            t->replay_players_deleted, t->ratings_reset, t->email_mismatch, t->unexpected);
    free(c.a);
    free(c.r);
    sqlite3_close(c.db);
    return rc;
}
