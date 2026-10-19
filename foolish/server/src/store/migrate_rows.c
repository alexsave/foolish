// migrate_rows.c - DynamoDB's items into the SQL store's tables (migrate_rows.h).
//
// The items are decoded with the codecs the item path itself uses (cb_rows_user_of,
// cb_rows_game_of, ...), so a row reads here as production's code read it. The rows go in through
// the SQL store's operations (user_import, game_import, snapshot_put, ...), so the stored bytes are
// the ones serve writes. The checks read the tables back with plain SQL by each row's natural key.
#include "store/migrate_rows.h"

#include "store/rows_auth.h"
#include "store/store_sql.h"

#include "auth/names.h"
#include "util/log.h"
#include "util/str.h"
#include "util/uuid.h"
#include "../../../../shared/c/hex/hex.h"
#include "../../../../shared/c/num_parse.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

enum {
    F_USER, F_EMAIL, F_RATING, F_BOT, F_SEAT_BOT, F_MEMBER, F_GAME, F_POINTER, F_REPLAY, F_CHAT,
    F_SESSION, F_REFRESH, F_FAILS, F_SCAN, F__N
};

static const struct {
    const char *name;     // for the report
    const char *pk, *sk;  // the pk's prefix; the sk exactly, or its prefix when it ends in '#' ("" any)
    int         rank;
} FAM[F__N] = {
    [F_USER] = { "USER PROFILE", "USER#", "PROFILE", 0 },
    [F_EMAIL] = { "EMAIL", "EMAIL#", "USER", 0 },
    [F_RATING] = { "USER RATING", "USER#", "RATING", 1 },
    [F_BOT] = { "BOT", "BOT#", "BOT", 1 },
    [F_SEAT_BOT] = { "GAME BOT#", "GAME#", "BOT#", 2 },
    [F_MEMBER] = { "GAME MEMBER#", "GAME#", "MEMBER#", 2 },
    [F_GAME] = { "GAME ROW", "GAME#", "ROW", 2 },
    [F_POINTER] = { "SNAP PLAYER#", "SNAP#", "PLAYER#", 3 },
    [F_REPLAY] = { "SNAP", "SNAP#", "SNAP", 3 },
    [F_CHAT] = { "CHAT", "CHAT#", "", 4 },
    [F_SESSION] = { "SESSION", "SESSION#", "SESSION", 5 },
    [F_REFRESH] = { "REFRESH", "REFRESH#", "TOKEN", 5 },
    [F_FAILS] = { "AUTHFAIL", "AUTHFAIL#", "FAILS", 5 },
    [F_SCAN] = { "SCAN (not kept)", "SCAN", "GAME#", 5 },
};

static int family(const char *pk, const char *sk) {
    for (int f = 0; f < F__N; f++) {
        size_t pl = strlen(FAM[f].pk), sl = strlen(FAM[f].sk);
        bool prefix = sl == 0 || FAM[f].sk[sl - 1] == '#';
        if (strncmp(pk, FAM[f].pk, pl) != 0) continue;
        if (f == F_SCAN ? strcmp(pk, "SCAN") != 0 : false) continue;
        if (prefix ? strncmp(sk, FAM[f].sk, sl) == 0 : strcmp(sk, FAM[f].sk) == 0) return f;
    }
    return -1;
}

int cb_mig_rank(const char *pk, const char *sk) {
    int f = family(pk, sk);
    return f < 0 ? -1 : FAM[f].rank;
}

// ---- the copy's state ------------------------------------------------------------------------

typedef struct {
    cb_uuid user;
    int     seat, rank;
    bool    has;           // the pointer carries the summary
    int     fool, dur, n_players;
} pointer;

struct cb_mig {
    cb_store *dst;
    int64_t   now;
    FILE     *log;
    long      seen[F__N], written[F__N], skipped[F__N], failed[F__N];
    long      game_logs;   // games whose row carried a session log: one game_moves row each
    // A game's seats, held until its ROW (same pk).
    char      seat_game[CB_GAME_ID_MAX + 1];
    int       n_humans, n_bots;
    cb_uuid   humans[CB_MAX_SEATS], bots[CB_MAX_SEATS];
    // A replay's pointers, held until its SNAP (same pk).
    char      ptr_snap[CB_UUID_LEN + 1];
    int       n_ptr;
    pointer   ptr[CB_MAX_SEATS];
    // The bots, written together at the end (they replace the seeded ones).
    cb_bot_row *bots_rows;
    int         n_bots_rows, cap_bots_rows;
    // The canonical names taken so far, sorted (a duplicate fails loudly).
    char      (*names)[CB_USERNAME_MAX + 1];
    int         n_names, cap_names;
};

cb_mig *cb_mig_new(cb_store *dst, int64_t now_ms, FILE *log) {
    cb_mig *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->dst = dst;
    m->now = now_ms;
    m->log = log ? log : stderr;
    return m;
}

void cb_mig_free(cb_mig *m) {
    if (!m) return;
    free(m->bots_rows);
    free(m->names);
    free(m);
}

static int failed(cb_mig *m, int f, const char *pk, const char *sk, const char *why) {
    if (f >= 0) m->failed[f]++;
    if (m->failed[f >= 0 ? f : 0] <= 20 || f < 0) fprintf(m->log, "cbase-export: %s / %s: %s\n", pk, sk, why);
    return CB_E_INVALID;
}

static const char *after_hash(const char *s) {
    const char *h = strchr(s, '#');
    return h ? h + 1 : s;
}

// A name not yet taken goes into the sorted list: false when it is there already.
static bool claim_name(cb_mig *m, const char *name) {
    int lo = 0, hi = m->n_names;
    while (lo < hi) {
        int mid = (lo + hi) / 2, c = strcmp(m->names[mid], name);
        if (c == 0) return false;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    if (m->n_names == m->cap_names) {
        int cap = m->cap_names ? m->cap_names * 2 : 256;
        void *p = realloc(m->names, (size_t)cap * sizeof *m->names);
        if (!p) return false;
        m->names = p;
        m->cap_names = cap;
    }
    memmove(&m->names[lo + 1], &m->names[lo], (size_t)(m->n_names - lo) * sizeof *m->names);
    cb_strlcpy(m->names[lo], name, sizeof m->names[lo]);
    m->n_names++;
    return true;
}

// ---- the decoders the item path has no public one for ------------------------------------------

static bool session_of(const cb_item *it, cb_session_row *r) {
    memset(r, 0, sizeof *r);
    if (!cb_item_s(it, "id", r->id, sizeof r->id) || !cb_item_s(it, "user_id", r->user_id, sizeof r->user_id)) return false;
    r->created_at = cb_item_n_or(it, "created_at", 0);
    r->expires_at = cb_item_n_or(it, CB_ITEM_TTL_MS, 0);
    r->gen = cb_item_n_or(it, "gen", 0);
    cb_item_bool(it, "revoked", &r->revoked);
    return true;
}

static bool refresh_of(const cb_item *it, cb_refresh_row *r) {
    memset(r, 0, sizeof *r);
    if (!cb_item_s(it, "token_hash", r->token_hash, sizeof r->token_hash) ||
        !cb_item_s(it, "user_id", r->user_id, sizeof r->user_id))
        return false;
    cb_item_s(it, "session_id", r->session_id, sizeof r->session_id);
    r->created_at = cb_item_n_or(it, "created_at", 0);
    cb_item_bool(it, "revoked", &r->revoked);
    r->revoked_at = cb_item_n_or(it, "revoked_at", 0);
    r->expires_at = cb_item_n_or(it, CB_ITEM_TTL_MS, 0);
    return true;
}

static bool fails_of(const cb_item *it, cb_signin_fails *f, int64_t *expires) {
    int64_t n;
    if (!cb_item_n(it, "n", &n)) return false;
    f->n = n < 0 ? 0 : n > INT32_MAX ? INT32_MAX : (int32_t)n;
    f->last_at = cb_item_n_or(it, "last_at", 0);
    f->locked_until = cb_item_n_or(it, "locked_until", 0);
    *expires = cb_item_n_or(it, CB_ITEM_TTL_MS, 0);
    return true;
}

// The summary the replay's pointers carry (they are written from it: rows_foolish.c snapshot_put):
// the fool, the duration, and each seat's rank, which is its place in the elimination order.
static void summary_of(const cb_mig *m, int n_players, cb_snap_summary *sum) {
    memset(sum, 0, sizeof *sum);
    sum->fool = -1;
    sum->duration_s = -1;
    for (int i = 0; i < m->n_ptr; i++) {
        const pointer *p = &m->ptr[i];
        if (!p->has) continue;
        sum->has = true;
        sum->fool = (int8_t)p->fool;
        sum->duration_s = p->dur;
        if (p->rank >= 1 && p->rank < n_players && p->rank <= CB_MAX_SEATS) {
            sum->elim[p->rank - 1] = (int8_t)p->seat;
            if (p->rank > sum->n_elim) sum->n_elim = (int8_t)p->rank;
        }
    }
}

// ---- the copy ---------------------------------------------------------------------------------

int cb_mig_put(cb_mig *m, const cb_item *it) {
    char pk[CB_ITEM_KEY_MAX], sk[CB_ITEM_KEY_MAX];
    if (!cb_item_s(it, CB_ITEM_PK, pk, sizeof pk) || !cb_item_s(it, CB_ITEM_SK, sk, sizeof sk))
        return failed(m, -1, "?", "?", "an item with no pk or sk");
    int f = family(pk, sk);
    if (f < 0) return failed(m, -1, pk, sk, "an unknown family: nothing in the SQL store holds it");
    m->seen[f]++;
    int rc = CB_OK;
    switch (f) {
    case F_USER: {
        cb_user_row u;
        if (!cb_rows_user_of(it, &u)) return failed(m, f, pk, sk, "does not decode as an account");
        if (u.username[0]) {
            char canon[CB_USERNAME_MAX * 2 + 8];
            int cn = cb_name_case(u.username, strlen(u.username), canon, sizeof canon);
            if (cn < 0 || strcmp(canon, u.username) != 0)
                return failed(m, f, pk, sk, "the username is not in its canonical form (auth/names.h)");
            if (!claim_name(m, canon)) return failed(m, f, pk, sk, "the username is another account's: duplicate names");
        }
        rc = CB_STORE_CALL(m->dst, user_import, &u);
        break;
    }
    case F_EMAIL:
    case F_SCAN:
        m->skipped[f]++;   // the account row holds the email; the SQL store keeps no scan marker
        return CB_OK;
    case F_RATING: {
        cb_rating_row r;
        if (!cb_rows_rating_of(it, &r)) return failed(m, f, pk, sk, "does not decode as a rating");
        rc = CB_ROWS_CALL(m->dst, rating_import, &r);
        break;
    }
    case F_BOT: {
        cb_bot_row b;
        if (!cb_rows_bot_of(it, &b)) return failed(m, f, pk, sk, "does not decode as a bot");
        if (m->n_bots_rows == m->cap_bots_rows) {
            int cap = m->cap_bots_rows ? m->cap_bots_rows * 2 : 64;
            void *p = realloc(m->bots_rows, (size_t)cap * sizeof *m->bots_rows);
            if (!p) return CB_E_NOMEM;
            m->bots_rows = p;
            m->cap_bots_rows = cap;
        }
        m->bots_rows[m->n_bots_rows++] = b;
        return CB_OK;   // written (and counted) at cb_mig_finish
    }
    case F_SEAT_BOT:
    case F_MEMBER: {
        const char *gid = after_hash(pk), *id = after_hash(sk);
        if (strcmp(m->seat_game, gid) != 0) {   // a new game's first seat
            cb_strlcpy(m->seat_game, gid, sizeof m->seat_game);
            m->n_humans = m->n_bots = 0;
        }
        int *n = f == F_MEMBER ? &m->n_humans : &m->n_bots;
        cb_uuid *ids = f == F_MEMBER ? m->humans : m->bots;
        if (*n >= CB_MAX_SEATS || strlen(id) > CB_UUID_LEN) return failed(m, f, pk, sk, "more seats than a table has");
        cb_strlcpy(ids[(*n)++], id, sizeof(cb_uuid));
        return CB_OK;   // written with the row (game_import), counted there
    }
    case F_GAME: {
        cb_game_row g;
        if (!cb_rows_game_of(it, &g)) return failed(m, f, pk, sk, "does not decode as a game");
        bool mine = strcmp(m->seat_game, g.id) == 0;
        rc = CB_ROWS_CALL(m->dst, game_import, &g, m->humans, mine ? m->n_humans : 0, m->bots, mine ? m->n_bots : 0);
        if (rc == CB_OK) {
            if (mine) {
                m->written[F_MEMBER] += m->n_humans;
                m->written[F_SEAT_BOT] += m->n_bots;
            }
            if (g.logs_len) m->game_logs++;
        }
        m->seat_game[0] = '\0';
        m->n_humans = m->n_bots = 0;
        cb_game_row_free(&g);
        break;
    }
    case F_POINTER: {
        const char *sid = after_hash(pk);
        if (strcmp(m->ptr_snap, sid) != 0) {
            cb_strlcpy(m->ptr_snap, sid, sizeof m->ptr_snap);
            m->n_ptr = 0;
        }
        if (m->n_ptr >= CB_MAX_SEATS) return failed(m, f, pk, sk, "more players than a table has");
        pointer *p = &m->ptr[m->n_ptr++];
        memset(p, 0, sizeof *p);
        cb_strlcpy(p->user, after_hash(sk), sizeof p->user);
        p->seat = (int)cb_item_n_or(it, "seat", -1);
        p->rank = (int)cb_item_n_or(it, "rank", 0);
        p->has = cb_item_n_or(it, "dur", -2) != -2;
        p->fool = (int)cb_item_n_or(it, "fool", -1);
        p->dur = (int)cb_item_n_or(it, "dur", -1);
        p->n_players = (int)cb_item_n_or(it, "n_players", 0);
        return CB_OK;   // written by the replay's put (one row per player), counted there
    }
    case F_REPLAY: {
        cb_snapshot_row sn;
        if (!cb_rows_snapshot_of(it, &sn)) return failed(m, f, pk, sk, "does not decode as a replay");
        if (strcmp(m->ptr_snap, sn.id) != 0) m->n_ptr = 0;
        summary_of(m, sn.n_players, &sn.sum);
        rc = CB_ROWS_CALL(m->dst, snapshot_put, &sn);
        if (rc == CB_E_CONFLICT) rc = CB_OK;   // already there (--into-existing): the check compares it
        if (rc == CB_OK) m->written[F_POINTER] += m->n_ptr;
        m->ptr_snap[0] = '\0';
        m->n_ptr = 0;
        cb_snapshot_row_free(&sn);
        break;
    }
    case F_CHAT: {
        cb_chat_row c;
        if (!cb_rows_chat_of(it, &c)) return failed(m, f, pk, sk, "does not decode as a chat message");
        rc = CB_ROWS_CALL(m->dst, chat_put, &c);
        if (rc == CB_E_CONFLICT) rc = CB_OK;
        cb_chat_row_free(&c);
        break;
    }
    case F_SESSION: {
        cb_session_row r;
        if (!session_of(it, &r)) return failed(m, f, pk, sk, "does not decode as a session");
        if (r.expires_at <= m->now) { m->skipped[f]++; return CB_OK; }
        rc = CB_STORE_CALL(m->dst, session_put, &r);
        break;
    }
    case F_REFRESH: {
        cb_refresh_row r;
        if (!refresh_of(it, &r)) return failed(m, f, pk, sk, "does not decode as a refresh token");
        if (r.expires_at > 0 && r.expires_at <= m->now) { m->skipped[f]++; return CB_OK; }
        rc = CB_STORE_CALL(m->dst, refresh_put, &r);
        if (rc == CB_E_CONFLICT) rc = CB_OK;
        break;
    }
    case F_FAILS: {
        cb_signin_fails x;
        int64_t expires;
        if (!fails_of(it, &x, &expires)) return failed(m, f, pk, sk, "does not decode as failed sign-ins");
        if (expires <= m->now) { m->skipped[f]++; return CB_OK; }
        // The store keeps a count an hour after its last write: that write's time gives the same end.
        rc = CB_STORE_CALL(m->dst, signin_fails_put, after_hash(pk), &x, expires - 3600LL * 1000);
        break;
    }
    }
    if (rc != CB_OK) {
        char why[96];
        snprintf(why, sizeof why, "the SQL store refused it: %s", cb_strerror(rc));
        failed(m, f, pk, sk, why);
        return rc;
    }
    m->written[f]++;
    return CB_OK;
}

int cb_mig_finish(cb_mig *m) {
    if (!m->n_bots_rows) return CB_OK;
    int rc = CB_ROWS_CALL(m->dst, bots_replace, m->bots_rows, m->n_bots_rows);
    if (rc == CB_OK) m->written[F_BOT] += m->n_bots_rows;
    else {
        m->failed[F_BOT] += m->n_bots_rows;
        fprintf(m->log, "cbase-export: the bots were refused: %s\n", cb_strerror(rc));
    }
    return rc;
}

// ---- the checks: plain SQL by the natural key ------------------------------------------------------

typedef struct {
    int   rows, n;
    char *v[12];
} first_row;

static bool take_first(int n, const char *const *names, const char *const *values, void *ud) {
    first_row *r = ud;
    if (r->rows++ == 0)
        for (int i = 0; i < n && i < 12; i++) {
            r->v[i] = values[i] ? strdup(values[i]) : NULL;
            r->n = i + 1;
        }
    return true;
}

static void first_free(first_row *r) {
    for (int i = 0; i < r->n; i++) free(r->v[i]);
    memset(r, 0, sizeof *r);
}

// One SELECT; the first row's values (NULL for SQL NULL). The rows it answered, or a CB_E_*.
static int q1(cb_store *s, const char *sql, first_row *r) {
    memset(r, 0, sizeof *r);
    int rc = CB_STORE_CALL(s, admin_query, sql, false, take_first, r);
    return rc < 0 ? rc : r->rows;
}

// A UUID as an SQL blob literal (x'...'): false when it is not one.
static bool uuid_lit(const char *id, char out[2 * CB_UUID_BYTES + 4]) {
    unsigned char b[CB_UUID_BYTES];
    if (!cb_uuid_pack(id, b)) return false;
    out[0] = 'x';
    out[1] = '\'';
    hex_encode(b, CB_UUID_BYTES, out + 2, 2 * CB_UUID_BYTES + 1);
    out[2 + 2 * CB_UUID_BYTES] = '\'';
    out[3 + 2 * CB_UUID_BYTES] = '\0';
    return true;
}

// Text as an SQL string literal ('...' with ' doubled) into out. false when it does not fit.
static bool text_lit(const char *t, char *out, size_t cap) {
    size_t o = 0;
    if (cap < 3) return false;
    out[o++] = '\'';
    for (; *t; t++) {
        if (o + 3 >= cap) return false;
        if (*t == '\'') out[o++] = '\'';
        out[o++] = *t;
    }
    out[o++] = '\'';
    out[o] = '\0';
    return true;
}

// The value a text column of `r` holds against the expected one (NULL: SQL NULL).
static bool eq(const char *got, const char *want) {
    if (!got || !want) return got == want;
    return strcmp(got, want) == 0;
}
static bool eq_i(const char *got, int64_t want) {
    char w[24];
    snprintf(w, sizeof w, "%" PRId64, want);
    return got && strcmp(got, w) == 0;
}
// Bytes against lower(hex(...)) text ("" for none, as hex() of NULL or an empty blob reads).
static bool eq_hex(const char *got, const uint8_t *p, size_t n) {
    if (!got) return n == 0;
    if (strlen(got) != 2 * n) return false;
    for (size_t i = 0; i < n; i++)
        if (got[2 * i] != hex_digit(p[i] >> 4) || got[2 * i + 1] != hex_digit(p[i])) return false;
    return true;
}

static int differ(cb_mig *m, const char *pk, const char *sk, const char *what) {
    fprintf(m->log, "cbase-export: %s / %s differs in the SQL store: %s\n", pk, sk, what);
    return 0;
}

int cb_mig_same(cb_mig *m, const cb_item *it) {
    char pk[CB_ITEM_KEY_MAX], sk[CB_ITEM_KEY_MAX], sql[1200], k1[80], k2[80];
    if (!cb_item_s(it, CB_ITEM_PK, pk, sizeof pk) || !cb_item_s(it, CB_ITEM_SK, sk, sizeof sk)) return 0;
    int f = family(pk, sk), rc = 1;
    first_row r;
    memset(&r, 0, sizeof r);
    switch (f) {
    case F_USER: {
        cb_user_row u, got;
        if (!cb_rows_user_of(it, &u)) return differ(m, pk, sk, "does not decode");
        int g = CB_STORE_CALL(m->dst, user_get, u.id, &got);
        if (g == CB_E_NOT_FOUND) return differ(m, pk, sk, "no such account");
        if (g != CB_OK) return g;
        if (strcmp(got.email, u.email) || strcmp(got.password_hash, u.password_hash) || strcmp(got.username, u.username) ||
            got.created_at != u.created_at || got.last_sign_in_at != u.last_sign_in_at || got.session_gen != u.session_gen ||
            strcmp(got.kept_session, u.kept_session))
            return differ(m, pk, sk, "an account field");
        return 1;
    }
    case F_EMAIL: {
        cb_uuid id;
        cb_user_row got;
        if (!cb_item_s(it, "user_id", id, sizeof id)) return differ(m, pk, sk, "does not decode");
        int g = CB_STORE_CALL(m->dst, user_by_email, after_hash(pk), &got);
        if (g == CB_E_NOT_FOUND) return differ(m, pk, sk, "no account has the email");
        if (g != CB_OK) return g;
        return strcmp(got.id, id) == 0 ? 1 : differ(m, pk, sk, "the email is another account's");
    }
    case F_RATING: {
        cb_rating_row x;
        if (!cb_rows_rating_of(it, &x) || !uuid_lit(x.user_id, k1)) return differ(m, pk, sk, "does not decode");
        snprintf(sql, sizeof sql, "SELECT username, elo_rating, previous_elo, games_played, updated_at FROM ratings WHERE user_id = %s", k1);
        int n = q1(m->dst, sql, &r);
        if (n < 0) return n;
        if (n != 1 || !eq(r.v[0], x.username[0] ? x.username : NULL) || !eq_i(r.v[1], x.elo_rating) ||
            !eq_i(r.v[2], x.previous_elo) || !eq_i(r.v[3], x.games_played) || !eq_i(r.v[4], x.updated_at))
            rc = differ(m, pk, sk, "a rating field");
        break;
    }
    case F_BOT: {
        cb_bot_row b;
        if (!cb_rows_bot_of(it, &b) || !uuid_lit(b.id, k1)) return differ(m, pk, sk, "does not decode");
        snprintf(sql, sizeof sql, "SELECT nickname, strategy_key, elo_rating, previous_elo, games_played, created_at, updated_at "
                                  "FROM bots WHERE id = %s", k1);
        int n = q1(m->dst, sql, &r);
        if (n < 0) return n;
        if (n != 1 || !eq(r.v[0], b.nickname) || !eq(r.v[1], b.strategy_key) || !eq_i(r.v[2], b.elo_rating) ||
            !eq_i(r.v[3], b.previous_elo) || !eq_i(r.v[4], b.games_played) || !eq_i(r.v[5], b.created_at) ||
            !eq_i(r.v[6], b.updated_at))
            rc = differ(m, pk, sk, "a bot field");
        break;
    }
    case F_SEAT_BOT:
    case F_MEMBER: {
        if (!text_lit(after_hash(pk), k2, sizeof k2) || !uuid_lit(after_hash(sk), k1)) return differ(m, pk, sk, "a bad key");
        snprintf(sql, sizeof sql, "SELECT 1 FROM %s WHERE game_id = %s AND %s = %s", f == F_MEMBER ? "game_members" : "game_bots",
                 k2, f == F_MEMBER ? "user_id" : "bot_id", k1);
        int n = q1(m->dst, sql, &r);
        if (n < 0) return n;
        if (n != 1) rc = differ(m, pk, sk, "no such seat");
        break;
    }
    case F_GAME: {
        cb_game_row g, got;
        if (!cb_rows_game_of(it, &g)) return differ(m, pk, sk, "does not decode");
        int e = CB_ROWS_CALL(m->dst, game_get, g.id, &got);
        if (e != CB_OK) {
            cb_game_row_free(&g);
            return e == CB_E_NOT_FOUND ? differ(m, pk, sk, "no such game") : e;
        }
        uint8_t *logs = NULL;
        size_t logs_len = 0;
        e = CB_ROWS_CALL(m->dst, game_logs, g.id, got.log_from, &logs, &logs_len);
        bool same = e == CB_OK && got.status == g.status && got.version == g.version && got.needs_bots == g.needs_bots &&
                    got.round_epoch == g.round_epoch && got.created_at == g.created_at && got.updated_at == g.updated_at &&
                    got.last_commit_at == g.last_commit_at && got.has_seed == g.has_seed &&
                    (!g.has_seed || memcmp(got.seed, g.seed, CB_SEED_LEN) == 0) && got.state_len == g.state_len &&
                    !memcmp(got.state, g.state, g.state_len) && got.roster_len == g.roster_len &&
                    !memcmp(got.roster, g.roster, g.roster_len) && logs_len == g.logs_len &&
                    (!logs_len || !memcmp(logs, g.logs, logs_len));
        free(logs);
        cb_game_row_free(&got);
        cb_game_row_free(&g);
        if (e != CB_OK) return e;
        return same ? 1 : differ(m, pk, sk, "a game field, its state, roster or session log");
    }
    case F_POINTER: {
        if (!uuid_lit(after_hash(sk), k1) || !uuid_lit(after_hash(pk), k2)) return differ(m, pk, sk, "a bad key");
        snprintf(sql, sizeof sql, "SELECT seat, rank, created_at FROM replay_players WHERE user_id = %s AND replay_id = %s", k1, k2);
        int n = q1(m->dst, sql, &r);
        if (n < 0) return n;
        bool has = cb_item_n_or(it, "dur", -2) != -2;
        if (n != 1 || !eq_i(r.v[2], cb_item_n_or(it, "created_at", 0)) ||
            (has && (!eq_i(r.v[0], cb_item_n_or(it, "seat", -1)) || !eq_i(r.v[1], cb_item_n_or(it, "rank", 0)))))
            rc = differ(m, pk, sk, "no such history row, or its seat, rank or date");
        break;
    }
    case F_REPLAY: {
        cb_snapshot_row sn;
        if (!cb_rows_snapshot_of(it, &sn) || !uuid_lit(sn.id, k1)) return differ(m, pk, sk, "does not decode");
        snprintf(sql, sizeof sql, "SELECT game_id, lower(hex(players)), lower(hex(moves)), lower(hex(extras)), created_at "
                                  "FROM replays WHERE id = %s", k1);
        int n = q1(m->dst, sql, &r);
        if (n < 0) { cb_snapshot_row_free(&sn); return n; }
        uint8_t players[CB_MAX_SEATS * CB_UUID_BYTES];
        bool ok = n == 1;
        for (int i = 0; ok && i < sn.n_players; i++) ok = cb_uuid_pack(sn.player_ids[i], players + CB_UUID_BYTES * i);
        if (!ok || !eq(r.v[0], sn.game_id[0] ? sn.game_id : NULL) ||
            !eq_hex(r.v[1], players, (size_t)sn.n_players * CB_UUID_BYTES) || !eq_hex(r.v[2], sn.moves, sn.moves_len) ||
            !eq_hex(r.v[3], sn.extras, sn.extras_len) || !eq_i(r.v[4], sn.created_at))
            rc = differ(m, pk, sk, "no such replay, or its players, moves, extras, game or date");
        cb_snapshot_row_free(&sn);
        break;
    }
    case F_CHAT: {
        cb_chat_row c;
        if (!cb_rows_chat_of(it, &c)) return differ(m, pk, sk, "does not decode");
        if (!uuid_lit(c.id, k1) || !text_lit(c.game_id, k2, sizeof k2)) { cb_chat_row_free(&c); return differ(m, pk, sk, "a bad key"); }
        snprintf(sql, sizeof sql, "SELECT user_id, message, is_system, created_at FROM chat WHERE game_id = %s AND id = %s", k2, k1);
        int n = q1(m->dst, sql, &r);
        if (n < 0) { cb_chat_row_free(&c); return n; }
        if (n != 1 || !eq(r.v[0], c.user_id) || !eq(r.v[1], c.message) || !eq_i(r.v[2], c.is_system) ||
            !eq_i(r.v[3], c.created_at))
            rc = differ(m, pk, sk, "no such message, or its author, text or date");
        cb_chat_row_free(&c);
        break;
    }
    case F_SESSION: {
        cb_session_row x, got;
        if (!session_of(it, &x)) return differ(m, pk, sk, "does not decode");
        if (x.expires_at <= m->now) return 1;   // skipped by rule
        int g = CB_STORE_CALL(m->dst, session_get, x.id, m->now, &got);
        if (g == CB_E_NOT_FOUND) return differ(m, pk, sk, "no such session");
        if (g != CB_OK) return g;
        if (strcmp(got.user_id, x.user_id) || got.created_at != x.created_at || got.expires_at != x.expires_at ||
            got.gen != x.gen || got.revoked != x.revoked)
            return differ(m, pk, sk, "a session field");
        return 1;
    }
    case F_REFRESH: {
        cb_refresh_row x;
        uint8_t h[32];
        if (!refresh_of(it, &x)) return differ(m, pk, sk, "does not decode");
        if (x.expires_at > 0 && x.expires_at <= m->now) return 1;
        if (strlen(x.token_hash) != 64 || hex_decode(x.token_hash, 64, h, sizeof h) != 32) return differ(m, pk, sk, "a bad hash");
        snprintf(sql, sizeof sql, "SELECT user_id, session_id, created_at, revoked, revoked_at, expires_at FROM refresh_tokens "
                                  "WHERE token_hash = x'%s'", x.token_hash);
        int n = q1(m->dst, sql, &r);
        if (n < 0) return n;
        if (n != 1 || !eq(r.v[0], x.user_id) || !eq(r.v[1], x.session_id[0] ? x.session_id : NULL) ||
            !eq_i(r.v[2], x.created_at) || !eq_i(r.v[3], x.revoked) || !eq_i(r.v[4], x.revoked_at) ||
            !eq_i(r.v[5], x.expires_at))
            rc = differ(m, pk, sk, "no such token, or a token field");
        break;
    }
    case F_FAILS: {
        cb_signin_fails x, got;
        int64_t expires;
        if (!fails_of(it, &x, &expires)) return differ(m, pk, sk, "does not decode");
        if (expires <= m->now) return 1;
        int g = CB_STORE_CALL(m->dst, signin_fails_get, after_hash(pk), m->now, &got);
        if (g == CB_E_NOT_FOUND) return differ(m, pk, sk, "no such count");
        if (g != CB_OK) return g;
        return got.n == x.n && got.last_at == x.last_at && got.locked_until == x.locked_until ? 1
                                                                                              : differ(m, pk, sk, "a count field");
    }
    case F_SCAN:
        return 1;
    default:
        return differ(m, pk, sk, "an unknown family");
    }
    first_free(&r);
    return rc;
}

// ---- the report ------------------------------------------------------------------------------

int cb_mig_report(cb_mig *m, FILE *out) {
    int bad = 0;
    fprintf(out, "%-18s %8s %8s %8s %8s\n", "family", "items", "written", "skipped", "failed");
    for (int f = 0; f < F__N; f++) {
        fprintf(out, "%-18s %8ld %8ld %8ld %8ld%s\n", FAM[f].name, m->seen[f], m->written[f], m->skipped[f], m->failed[f],
                m->failed[f] || m->written[f] + m->skipped[f] != m->seen[f] ? "   FAILED" : "");
        if (m->failed[f] || m->written[f] + m->skipped[f] != m->seen[f]) bad++;
    }
    // Each table against the items that make its rows.
    static const struct { const char *table; int f; } T[] = {
        { "users", F_USER }, { "ratings", F_RATING }, { "bots", F_BOT }, { "games", F_GAME }, { "game_members", F_MEMBER },
        { "game_bots", F_SEAT_BOT }, { "replays", F_REPLAY }, { "replay_players", F_POINTER }, { "chat", F_CHAT },
        { "sessions", F_SESSION }, { "refresh_tokens", F_REFRESH }, { "signin_fails", F_FAILS },
    };
    fprintf(out, "%-18s %8s %8s\n", "table", "rows", "items");
    for (size_t i = 0; i < sizeof T / sizeof T[0] + 2; i++) {
        char sql[128];
        const char *name;
        long want;
        if (i < sizeof T / sizeof T[0]) {
            name = T[i].table;
            want = m->seen[T[i].f] - m->skipped[T[i].f];
            snprintf(sql, sizeof sql, "SELECT count(*) FROM %s", name);
        } else if (i == sizeof T / sizeof T[0]) {
            name = "users (emails)";   // every email item names one account row
            want = m->seen[F_EMAIL];
            snprintf(sql, sizeof sql, "SELECT count(*) FROM users");
        } else {
            name = "game_moves";   // one row per game whose row carried a session log
            want = m->game_logs;
            snprintf(sql, sizeof sql, "SELECT count(DISTINCT game_id) FROM game_moves");
        }
        first_row r;
        int n = q1(m->dst, sql, &r);
        int64_t got = -1;
        if (n != 1 || !r.v[0] || !num_parse_i64_cstr(r.v[0], 0, INT64_MAX, &got)) got = -1;
        first_free(&r);
        fprintf(out, "%-18s %8lld %8ld%s\n", name, (long long)got, want, got == want ? "" : "   DIFFERENT");
        if (got != want) bad++;
    }
    return bad;
}
