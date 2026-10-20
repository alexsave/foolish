// rows_sql.c - foolish's rows on the SQL store (`cbase serve`): the operations of
// rows_foolish.h as SQL over foolish/server/sql/schema.sql, through store_sql.h.
// cb_rows_of answers this table for a SQLite store (cb_foolish_store sql_rows).
//
// A MOVE is one write transaction of two small statements: the games row
// updated in place (fixed size: the state and the roster, no log) fenced on
// the version it read, and the move's log records appended as one game_moves
// row. Seats and ratings are written only when they change. Nothing is read
// back first: the fence answers a lost race, and the caller holds the game's
// order lock (app/table_ops.c) so on serve the fence never loses.
//
// No lease, bots_idle or SCAN marker here: on serve a bot drive's lease lives
// in memory (app/bots.c), and the once-at-start recovery scan reads the games
// table (games_needing_bots).
#include "store/rows_foolish.h"

#include "store/store_import.h"
#include "store/store_sql.h"

#include "table.h"
#include "util/log.h"
#include "util/str.h"
#include "util/uuid.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#define MOD "store"

extern const char cb_foolish_schema_sql[];

enum {
    S_GAME_GET, S_GAME_EXISTS, S_GAME_INSERT, S_GAME_REPLACE, S_GAME_CAS, S_GAME_DELETE,
    S_MOVES_CLEAR, S_MOVES_ADD, S_MOVES_READ,
    S_MEMBERS_READ, S_BOTS_READ, S_MEMBER_ADD, S_MEMBER_DEL, S_SEAT_BOT_ADD, S_SEAT_BOT_DEL,
    S_MEMBERS_CLEAR, S_SEAT_BOTS_CLEAR, S_CHAT_CLEAR, S_REPLAYS_ORPHAN,
    S_GAMES_OF_USER, S_IS_MEMBER, S_NEEDING_BOTS,
    S_BOT_RATE, S_USER_RATE,
    S_BOTS_ALL, S_BOTS_CLEAR, S_BOT_PUT, S_BOT_ELO, S_USER_ELO,
    S_RATINGS_PLAYED, S_RATING_NAME, S_RATING_PUT,
    S_REPLAY_PUT, S_REPLAY_PLAYER_PUT, S_REPLAYS_NEW, S_REPLAYS_OLD, S_REPLAY_EXTRAS,
    S_CHAT_PUT, S_CHAT_LIST,
    S_DEL_MEMBERS, S_DEL_CHAT, S_DEL_RATING, S_DEL_HISTORY,
    S_KEYS_USERS, S_KEYS_BOTS, S_KEYS_RATINGS, S_KEYS_GAMES, S_KEYS_MEMBERS, S_KEYS_SEAT_BOTS, S_KEYS_REPLAYS, S_KEYS_CHAT,
    S__COUNT
};

// POSITIONS. A row type's columns are listed once (GAME_COLS, ...) with a name per position: the same
// number is the bind parameter of a statement that writes the list in order (VALUES of GAME_VALS) and,
// less one (COL), the result column of one that selects it. A small statement of one or two keys
// numbers its parameters in place (?1 the key, ?2 the second), as its comment says.
#define XS_(x) #x
#define XS(x) XS_(x)
#define P(n) "?" XS(n)
#define COL(n) ((n) - 1)
#define NO_LIMIT (-1)   // a LIMIT that limits nothing

#define GAME_COLS "id, status, version, state, roster, seed, needs_bots, round_epoch, log_from, created_at, updated_at, last_commit_at"
#define G_ID             1
#define G_STATUS         2
#define G_VERSION        3
#define G_STATE          4
#define G_ROSTER         5
#define G_SEED           6
#define G_NEEDS_BOTS     7
#define G_ROUND_EPOCH    8
#define G_LOG_FROM       9
#define G_CREATED_AT     10
#define G_UPDATED_AT     11
#define G_LAST_COMMIT_AT 12
#define G_EXPECTED       13   // S_GAME_CAS's fence: the version the commit read
#define GAME_VALS P(G_ID) ", " P(G_STATUS) ", " P(G_VERSION) ", " P(G_STATE) ", " P(G_ROSTER) ", " P(G_SEED) ", " \
                  P(G_NEEDS_BOTS) ", " P(G_ROUND_EPOCH) ", " P(G_LOG_FROM) ", " P(G_CREATED_AT) ", " P(G_UPDATED_AT) ", " P(G_LAST_COMMIT_AT)

#define BOT_COLS "id, nickname, strategy_key, elo_rating, previous_elo, games_played, created_at, updated_at"
#define B_ID           1
#define B_NICKNAME     2
#define B_STRATEGY     3
#define B_ELO          4
#define B_PREVIOUS_ELO 5
#define B_PLAYED       6
#define B_CREATED_AT   7
#define B_UPDATED_AT   8
#define BOT_VALS P(B_ID) ", " P(B_NICKNAME) ", " P(B_STRATEGY) ", " P(B_ELO) ", " P(B_PREVIOUS_ELO) ", " P(B_PLAYED) ", " \
                 P(B_CREATED_AT) ", " P(B_UPDATED_AT)

#define RATING_COLS "user_id, username, elo_rating, previous_elo, games_played, updated_at"
#define R_USER         1
#define R_USERNAME     2
#define R_ELO          3
#define R_PREVIOUS_ELO 4
#define R_PLAYED       5
#define R_UPDATED_AT   6
#define RATING_VALS P(R_USER) ", " P(R_USERNAME) ", " P(R_ELO) ", " P(R_PREVIOUS_ELO) ", " P(R_PLAYED) ", " P(R_UPDATED_AT)

#define REPLAY_COLS "id, game_id, players, moves, extras, created_at, fool, dur"
#define Y_ID         1
#define Y_GAME       2
#define Y_PLAYERS    3
#define Y_MOVES      4
#define Y_EXTRAS     5
#define Y_CREATED_AT 6
#define Y_FOOL       7
#define Y_DUR        8
#define Y_SEAT       9    // the viewer's history row, after the replay's columns (REPLAYS_Q)
#define Y_RANK       10
#define REPLAY_VALS P(Y_ID) ", " P(Y_GAME) ", " P(Y_PLAYERS) ", " P(Y_MOVES) ", " P(Y_EXTRAS) ", " P(Y_CREATED_AT) ", " \
                    P(Y_FOOL) ", " P(Y_DUR)
// The viewer's replays, with what narrows them (NULL: any): an id, a game, a ref (as a replay id and as a
// game id), and the limit (NO_LIMIT: none).
#define Q_VIEWER   1
#define Q_ID       2
#define Q_GAME     3
#define Q_REF_ID   4
#define Q_REF_GAME 5
#define Q_LIMIT    6
#define REPLAYS_Q(dir) \
    "SELECT r.id, r.game_id, r.players, r.moves, r.extras, r.created_at, r.fool, r.dur, p.seat, p.rank " \
    "FROM replay_players p JOIN replays r ON r.id = p.replay_id WHERE p.user_id = " P(Q_VIEWER) " " \
    "AND (" P(Q_ID) " IS NULL OR r.id = " P(Q_ID) ") AND (" P(Q_GAME) " IS NULL OR r.game_id = " P(Q_GAME) ") " \
    "AND (" P(Q_REF_ID) " IS NULL OR r.id = " P(Q_REF_ID) " OR r.game_id = " P(Q_REF_GAME) ") " \
    "ORDER BY p.created_at " dir ", p.replay_id " dir " LIMIT " P(Q_LIMIT)

#define REPLAY_PLAYER_COLS "user_id, created_at, replay_id, seat, rank"
#define H_USER       1
#define H_CREATED_AT 2
#define H_REPLAY     3
#define H_SEAT       4
#define H_RANK       5

#define CHAT_COLS "game_id, created_at, id, user_id, message, is_system"
#define C_GAME       1
#define C_CREATED_AT 2
#define C_ID         3
#define C_USER       4
#define C_MESSAGE    5
#define C_SYSTEM     6
#define C_LIMIT      2   // S_CHAT_LIST: after the game id

static const char *const STMTS[S__COUNT] = {
    [S_GAME_GET] = "SELECT " GAME_COLS " FROM games WHERE id = ?1",
    [S_GAME_EXISTS] = "SELECT 1 FROM games WHERE id = ?1",
    [S_GAME_INSERT] = "INSERT INTO games (" GAME_COLS ") VALUES (" GAME_VALS ")",
    [S_GAME_REPLACE] = "INSERT OR REPLACE INTO games (" GAME_COLS ") VALUES (" GAME_VALS ")",
    // THE CAS (created_at is bound and not written).
    [S_GAME_CAS] = "UPDATE games SET status = " P(G_STATUS) ", version = " P(G_VERSION) ", state = " P(G_STATE) ", "
                   "roster = " P(G_ROSTER) ", seed = " P(G_SEED) ", needs_bots = " P(G_NEEDS_BOTS) ", "
                   "round_epoch = " P(G_ROUND_EPOCH) ", log_from = " P(G_LOG_FROM) ", updated_at = " P(G_UPDATED_AT) ", "
                   "last_commit_at = " P(G_LAST_COMMIT_AT) " WHERE id = " P(G_ID) " AND version = " P(G_EXPECTED),
    // ?1 the game, ?2 the version it must still be at.
    [S_GAME_DELETE] = "DELETE FROM games WHERE id = ?1 AND version = ?2",
    [S_MOVES_CLEAR] = "DELETE FROM game_moves WHERE game_id = ?1",
    // ?1 the game, ?2 the seq (the version the commit wrote), ?3 the move's log records.
    [S_MOVES_ADD] = "INSERT OR REPLACE INTO game_moves (game_id, seq, action) VALUES (?1, ?2, ?3)",
    // ?1 the game, ?2 log_from.
    [S_MOVES_READ] = "SELECT action FROM game_moves WHERE game_id = ?1 AND seq >= ?2 ORDER BY seq",
    [S_MEMBERS_READ] = "SELECT user_id FROM game_members WHERE game_id = ?1",
    [S_BOTS_READ] = "SELECT bot_id FROM game_bots WHERE game_id = ?1",
    // ?1 the game, ?2 the seat's id, ?3 when (a human's). A seat for an account that is gone is not
    // written (the roster keeps the id).
    [S_MEMBER_ADD] = "INSERT OR IGNORE INTO game_members (game_id, user_id, joined_at) "
                     "SELECT ?1, ?2, ?3 WHERE EXISTS (SELECT 1 FROM users WHERE id = ?2)",
    [S_MEMBER_DEL] = "DELETE FROM game_members WHERE game_id = ?1 AND user_id = ?2",
    [S_SEAT_BOT_ADD] = "INSERT OR IGNORE INTO game_bots (game_id, bot_id) VALUES (?1, ?2)",
    [S_SEAT_BOT_DEL] = "DELETE FROM game_bots WHERE game_id = ?1 AND bot_id = ?2",
    [S_MEMBERS_CLEAR] = "DELETE FROM game_members WHERE game_id = ?1",
    [S_SEAT_BOTS_CLEAR] = "DELETE FROM game_bots WHERE game_id = ?1",
    [S_CHAT_CLEAR] = "DELETE FROM chat WHERE game_id = ?1",
    [S_REPLAYS_ORPHAN] = "UPDATE replays SET game_id = NULL WHERE game_id = ?1",
    // ?1 the user, ?2 the limit.
    [S_GAMES_OF_USER] = "SELECT m.game_id, g.updated_at FROM game_members m JOIN games g ON g.id = m.game_id "
                        "WHERE m.user_id = ?1 ORDER BY g.updated_at DESC, m.game_id LIMIT ?2",
    [S_IS_MEMBER] = "SELECT 1 FROM game_members WHERE game_id = ?1 AND user_id = ?2",
    // ?1 stale before, ?2 active after, ?3 the limit.
    [S_NEEDING_BOTS] = "SELECT id FROM games WHERE needs_bots = 1 AND updated_at < ?1 AND last_commit_at > ?2 "
                       "ORDER BY updated_at, id LIMIT ?3",
    // A rated seat (?1), rating = max(0, rating + delta ?2), previous = the old one, one more game, at ?3.
    [S_BOT_RATE] = "UPDATE bots SET previous_elo = elo_rating, elo_rating = max(0, elo_rating + ?2), "
                   "games_played = games_played + 1, updated_at = ?3 WHERE id = ?1",
    // The same for a human (?1, named ?2, delta ?3, at ?5): with no row yet it starts from the start
    // rating ?4, if the account is still there.
    [S_USER_RATE] = "INSERT INTO ratings (" RATING_COLS ") SELECT ?1, ?2, max(0, ?4 + ?3), ?4, 1, ?5 "
                    "WHERE EXISTS (SELECT 1 FROM users WHERE id = ?1) "
                    "ON CONFLICT (user_id) DO UPDATE SET previous_elo = elo_rating, elo_rating = max(0, elo_rating + ?3), "
                    "games_played = games_played + 1, updated_at = ?5",
    [S_BOTS_ALL] = "SELECT " BOT_COLS " FROM bots",
    [S_BOTS_CLEAR] = "DELETE FROM bots",
    [S_BOT_PUT] = "INSERT OR REPLACE INTO bots (" BOT_COLS ") VALUES (" BOT_VALS ")",
    [S_BOT_ELO] = "SELECT elo_rating FROM bots WHERE id = ?1",
    [S_USER_ELO] = "SELECT elo_rating FROM ratings WHERE user_id = ?1",
    [S_RATINGS_PLAYED] = "SELECT " RATING_COLS " FROM ratings WHERE games_played > 0 ORDER BY elo_rating DESC, user_id LIMIT ?1",
    [S_RATING_NAME] = "UPDATE ratings SET username = ?2 WHERE user_id = ?1",
    [S_RATING_PUT] = "INSERT OR REPLACE INTO ratings (" RATING_COLS ") VALUES (" RATING_VALS ")",
    [S_REPLAY_PUT] = "INSERT INTO replays (" REPLAY_COLS ") VALUES (" REPLAY_VALS ")",
    [S_REPLAY_PLAYER_PUT] = "INSERT OR IGNORE INTO replay_players (" REPLAY_PLAYER_COLS ") VALUES (" P(H_USER) ", "
                            P(H_CREATED_AT) ", " P(H_REPLAY) ", " P(H_SEAT) ", " P(H_RANK) ")",
    [S_REPLAYS_NEW] = REPLAYS_Q("DESC"),
    [S_REPLAYS_OLD] = REPLAYS_Q("ASC"),
    // ?1 the replay, ?2 its extras.
    [S_REPLAY_EXTRAS] = "UPDATE replays SET extras = ?2 WHERE id = ?1",
    // A message for a game or an account that is gone is refused (CB_E_NOT_FOUND).
    [S_CHAT_PUT] = "INSERT INTO chat (" CHAT_COLS ") SELECT " P(C_GAME) ", " P(C_CREATED_AT) ", " P(C_ID) ", "
                   P(C_USER) ", " P(C_MESSAGE) ", " P(C_SYSTEM) " WHERE EXISTS (SELECT 1 FROM games WHERE id = " P(C_GAME)
                   ") AND EXISTS (SELECT 1 FROM users WHERE id = " P(C_USER) ")",
    [S_CHAT_LIST] = "SELECT " CHAT_COLS " FROM chat WHERE game_id = " P(C_GAME) " ORDER BY created_at DESC, id DESC "
                    "LIMIT " P(C_LIMIT),
    [S_DEL_MEMBERS] = "DELETE FROM game_members WHERE user_id = ?1",
    [S_DEL_CHAT] = "DELETE FROM chat WHERE user_id = ?1",
    [S_DEL_RATING] = "DELETE FROM ratings WHERE user_id = ?1",
    [S_DEL_HISTORY] = "DELETE FROM replay_players WHERE user_id = ?1",
    // The migration's walk (cb_rows_sql_keys): each family's id column, in the families' order.
    [S_KEYS_USERS] = "SELECT id FROM users",
    [S_KEYS_BOTS] = "SELECT id FROM bots",
    [S_KEYS_RATINGS] = "SELECT user_id FROM ratings",
    [S_KEYS_GAMES] = "SELECT id FROM games",
    [S_KEYS_MEMBERS] = "SELECT game_id FROM game_members",
    [S_KEYS_SEAT_BOTS] = "SELECT game_id FROM game_bots",
    [S_KEYS_REPLAYS] = "SELECT id FROM replays",
    [S_KEYS_CHAT] = "SELECT game_id FROM chat",
};

// ---- helpers ------------------------------------------------------------------------------

typedef sqlite3_stmt stmt_t;

static void b_i64(stmt_t *s, int i, int64_t v) { sqlite3_bind_int64(s, i, v); }
static void b_text(stmt_t *s, int i, const char *v) { sqlite3_bind_text(s, i, v ? v : "", -1, SQLITE_TRANSIENT); }
static void b_bytes_or_null(stmt_t *s, int i, const void *p, size_t n) {
    if (p && n) sqlite3_bind_blob64(s, i, p, n, SQLITE_TRANSIENT);
    else sqlite3_bind_null(s, i);
}

// Steps st once: 1 a row, 0 done, or a CB_E_* (st reset then).
static int step_row(cb_sql *x, stmt_t *st, const char *what) {
    if (!st) return CB_E_DB;
    int r = sqlite3_step(st);
    if (r == SQLITE_ROW) return 1;
    if (r == SQLITE_DONE) return 0;
    int rc = cb_sql_fail(x, what, r);
    sqlite3_reset(st);
    return rc;
}

// One statement with a game id at ?1 and maybe a UUID at ?2: run.
static int run_game_id(cb_sql *x, int id, const char *game, const char *uuid) {
    stmt_t *s = cb_sql_stmt(x, id);
    if (s) {
        b_text(s, 1, game);
        if (uuid) cb_sql_bind_uuid(s, 2, uuid);
    }
    return cb_sql_run(x, s, STMTS[id]);
}

static bool listed(const cb_uuid *ids, int n, const char *id) {
    for (int i = 0; i < n; i++)
        if (strcmp(ids[i], id) == 0) return true;
    return false;
}

// A growing array of fixed-size rows.
typedef struct {
    void  *rows;
    int    n, cap;
    size_t size;
} vec;

static void *vec_push(vec *v) {
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 16;
        void *p = realloc(v->rows, (size_t)cap * v->size);
        if (!p) return NULL;
        v->rows = p;
        v->cap = cap;
    }
    void *e = (char *)v->rows + (size_t)v->n++ * v->size;
    memset(e, 0, v->size);
    return e;
}

// ---- games ----------------------------------------------------------------------------------

static void game_bind(stmt_t *s, const cb_game_row *g, int64_t last_commit_at) {
    b_text(s, G_ID, g->id);
    b_i64(s, G_STATUS, g->status);
    b_i64(s, G_VERSION, g->version);
    cb_sql_bind_bytes(s, G_STATE, g->state, g->state_len);
    cb_sql_bind_bytes(s, G_ROSTER, g->roster, g->roster_len);
    b_bytes_or_null(s, G_SEED, g->has_seed ? g->seed : NULL, g->has_seed ? CB_SEED_LEN : 0);
    b_i64(s, G_NEEDS_BOTS, g->needs_bots);
    b_i64(s, G_ROUND_EPOCH, g->round_epoch);
    b_i64(s, G_LOG_FROM, g->log_from);
    b_i64(s, G_CREATED_AT, g->created_at);
    b_i64(s, G_UPDATED_AT, g->updated_at);
    b_i64(s, G_LAST_COMMIT_AT, last_commit_at);
}

// The row at st's current result. false: no memory.
static bool game_of(stmt_t *s, cb_game_row *g) {
    memset(g, 0, sizeof *g);
    cb_sql_col_text(s, COL(G_ID), g->id, sizeof g->id);
    g->status = sqlite3_column_int(s, COL(G_STATUS));
    g->version = sqlite3_column_int64(s, COL(G_VERSION));
    if (!cb_sql_col_bytes(s, COL(G_STATE), &g->state, &g->state_len) ||
        !cb_sql_col_bytes(s, COL(G_ROSTER), &g->roster, &g->roster_len)) {
        cb_game_row_free(g);
        return false;
    }
    const void *seed = sqlite3_column_blob(s, COL(G_SEED));
    if (seed && sqlite3_column_bytes(s, COL(G_SEED)) == CB_SEED_LEN) {
        g->has_seed = true;
        memcpy(g->seed, seed, CB_SEED_LEN);
    }
    g->needs_bots = sqlite3_column_int(s, COL(G_NEEDS_BOTS)) != 0;
    g->round_epoch = sqlite3_column_int64(s, COL(G_ROUND_EPOCH));
    g->log_from = sqlite3_column_int64(s, COL(G_LOG_FROM));
    g->created_at = sqlite3_column_int64(s, COL(G_CREATED_AT));
    g->updated_at = sqlite3_column_int64(s, COL(G_UPDATED_AT));
    g->last_commit_at = sqlite3_column_int64(s, COL(G_LAST_COMMIT_AT));
    // The seats are written in the same transaction as the row that names them, and an account's delete
    // takes its seats with it: the stored seats are exactly the roster's (less any deleted account).
    g->seats_exact = true;
    return true;
}

typedef struct {
    const char  *id;
    cb_game_row *out;
} game_read;

static int game_get_step(cb_sql *x, void *ud) {
    game_read *r = ud;
    stmt_t *s = cb_sql_stmt(x, S_GAME_GET);
    if (!s) return CB_E_DB;
    b_text(s, 1, r->id);
    int k = step_row(x, s, "game get"), rc = k;
    if (k == 1) rc = game_of(s, r->out) ? CB_OK : CB_E_NOMEM;
    else if (k == 0) rc = CB_E_NOT_FOUND;
    sqlite3_reset(s);
    return rc;
}

static int sql_game_get(cb_store *s, const char *id, cb_game_row *out) {
    memset(out, 0, sizeof *out);
    game_read r = { id, out };
    return cb_sql_read(s, game_get_step, &r);
}

typedef struct {
    const char *id;
    int64_t     from;
    cb_buf      log;
} logs_read;

static int logs_step(cb_sql *x, void *ud) {
    logs_read *r = ud;
    stmt_t *s = cb_sql_stmt(x, S_MOVES_READ);
    if (!s) return CB_E_DB;
    b_text(s, 1, r->id);
    b_i64(s, 2, r->from);
    int k;
    while ((k = step_row(x, s, "moves read")) == 1)
        cb_buf_append(&r->log, sqlite3_column_blob(s, 0), (size_t)sqlite3_column_bytes(s, 0));
    sqlite3_reset(s);
    return k < 0 ? k : r->log.oom ? CB_E_NOMEM : CB_OK;
}

// The current session's log: every game_moves row from log_from, in order, as one buffer.
static int sql_game_logs(cb_store *s, const char *id, int64_t log_from, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (log_from <= 0) return CB_OK;
    logs_read r = { .id = id, .from = log_from };
    cb_buf_init(&r.log);
    int rc = cb_sql_read(s, logs_step, &r);
    if (rc != CB_OK || !r.log.len) {
        cb_buf_free(&r.log);
        return rc;
    }
    *out = r.log.p;   // the buffer's malloc'd bytes become the caller's
    *out_len = r.log.len;
    return CB_OK;
}

typedef struct {
    const cb_game_row *g;
    const char        *creator;
} create_op;

static int create_step(cb_sql *x, void *ud) {
    create_op *o = ud;
    stmt_t *s = cb_sql_stmt(x, S_GAME_INSERT);
    if (!s) return CB_E_DB;
    game_bind(s, o->g, o->g->updated_at);   // a new row's last commit is its creation
    int rc = cb_sql_run(x, s, "game create");
    if (rc != CB_OK || !o->creator || !*o->creator) return rc;
    if (!(s = cb_sql_stmt(x, S_MEMBER_ADD))) return CB_E_DB;
    b_text(s, 1, o->g->id);
    cb_sql_bind_uuid(s, 2, o->creator);
    b_i64(s, 3, o->g->updated_at);
    return cb_sql_run(x, s, "creator seat");
}

static int sql_game_create(cb_store *s, const cb_game_row *g, const char *creator) {
    create_op o = { g, creator };
    return cb_sql_write(s, create_step, &o);
}

// The seats of a game as stored (a roster's worth; more read back are not kept).
typedef struct {
    int     n_humans, n_bots;
    cb_uuid humans[CB_MAX_SEATS], bots[CB_MAX_SEATS];
} seats_t;

static int read_seats(cb_sql *x, const char *game, seats_t *st) {
    st->n_humans = st->n_bots = 0;
    for (int pass = 0; pass < 2; pass++) {
        stmt_t *s = cb_sql_stmt(x, pass ? S_BOTS_READ : S_MEMBERS_READ);
        if (!s) return CB_E_DB;
        b_text(s, 1, game);
        int k;
        while ((k = step_row(x, s, "seats")) == 1) {
            if (pass && st->n_bots < CB_MAX_SEATS) cb_sql_col_uuid(s, 0, st->bots[st->n_bots++]);
            if (!pass && st->n_humans < CB_MAX_SEATS) cb_sql_col_uuid(s, 0, st->humans[st->n_humans++]);
        }
        sqlite3_reset(s);
        if (k < 0) return k;
    }
    return CB_OK;
}

// One seat added or removed.
static int seat_write(cb_sql *x, int id, const char *game, const char *uuid, int64_t at) {
    stmt_t *s = cb_sql_stmt(x, id);
    if (!s) return CB_E_DB;
    b_text(s, 1, game);
    cb_sql_bind_uuid(s, 2, uuid);
    if (id == S_MEMBER_ADD) b_i64(s, 3, at);
    return cb_sql_run(x, s, STMTS[id]);
}

// The stored seats brought to c's: only the differences are written.
static int seats_to(cb_sql *x, const cb_game_commit *c, const seats_t *st) {
    const cb_game_row *g = c->row;
    int rc = CB_OK;
    for (int i = 0; rc == CB_OK && i < c->n_humans; i++)
        if (!listed(st->humans, st->n_humans, c->humans[i])) rc = seat_write(x, S_MEMBER_ADD, g->id, c->humans[i], g->updated_at);
    for (int i = 0; rc == CB_OK && i < st->n_humans; i++)
        if (!listed(c->humans, c->n_humans, st->humans[i])) rc = seat_write(x, S_MEMBER_DEL, g->id, st->humans[i], 0);
    for (int i = 0; rc == CB_OK && i < c->n_bots; i++)
        if (!listed(st->bots, st->n_bots, c->bots[i])) rc = seat_write(x, S_SEAT_BOT_ADD, g->id, c->bots[i], 0);
    for (int i = 0; rc == CB_OK && i < st->n_bots; i++)
        if (!listed(c->bots, c->n_bots, st->bots[i])) rc = seat_write(x, S_SEAT_BOT_DEL, g->id, st->bots[i], 0);
    return rc;
}

static int rate(cb_sql *x, const cb_game_commit *c) {
    int rc = CB_OK;
    for (int i = 0; rc == CB_OK && i < c->n_rated; i++) {
        bool dup = false;   // one write per id: a repeated id is rated once
        for (int j = 0; j < i; j++) dup |= strcmp(c->rated_ids[j], c->rated_ids[i]) == 0;
        if (dup) continue;
        stmt_t *s = cb_sql_stmt(x, c->rated_is_bot[i] ? S_BOT_RATE : S_USER_RATE);
        if (!s) return CB_E_DB;
        cb_sql_bind_uuid(s, 1, c->rated_ids[i]);
        if (c->rated_is_bot[i]) {
            b_i64(s, 2, c->rating_delta[i]);
            b_i64(s, 3, c->row->updated_at);
        } else {
            cb_sql_bind_text_or_null(s, 2, c->rated_names[i]);
            b_i64(s, 3, c->rating_delta[i]);
            b_i64(s, 4, TABLE_RATING_START);
            b_i64(s, 5, c->row->updated_at);
        }
        rc = cb_sql_run(x, s, "rating");
    }
    return rc;
}

static int moves_write(cb_sql *x, const char *game, bool clear, int64_t seq, const uint8_t *log, size_t len) {
    int rc = clear ? run_game_id(x, S_MOVES_CLEAR, game, NULL) : CB_OK;
    if (rc != CB_OK || !len) return rc;
    stmt_t *s = cb_sql_stmt(x, S_MOVES_ADD);
    if (!s) return CB_E_DB;
    b_text(s, 1, game);
    b_i64(s, 2, seq);
    cb_sql_bind_bytes(s, 3, log, len);
    return cb_sql_run(x, s, "move append");
}

// THE CAS: the row updated in place if still at expected_version, then the move appended, then the seats
// and ratings that change.
static int commit_step(cb_sql *x, void *ud) {
    const cb_game_commit *c = ud;
    const cb_game_row *g = c->row;
    stmt_t *s = cb_sql_stmt(x, S_GAME_CAS);
    if (!s) return CB_E_DB;
    game_bind(s, g, g->updated_at);   // a commit's time is its last commit
    b_i64(s, G_EXPECTED, c->expected_version);
    int rc = cb_sql_run(x, s, "game commit");
    if (rc != CB_OK) return rc;
    if (cb_sql_changes(x) == 0) {   // moved on, or gone
        stmt_t *e = cb_sql_stmt(x, S_GAME_EXISTS);
        if (!e) return CB_E_DB;
        b_text(e, 1, g->id);
        int k = step_row(x, e, "game exists");
        sqlite3_reset(e);
        return k < 0 ? k : k ? CB_E_CONFLICT : CB_E_NOT_FOUND;
    }
    if ((rc = moves_write(x, g->id, c->log_clear, g->version, c->log_add, c->log_add_len)) != CB_OK) return rc;
    seats_t *st = malloc(sizeof *st);
    if (!st) return CB_E_NOMEM;
    if (c->prev_known) {
        st->n_humans = st->n_bots = 0;
        for (int i = 0; i < c->n_prev_humans && i < CB_MAX_SEATS; i++) memcpy(st->humans[st->n_humans++], c->prev_humans[i], sizeof(cb_uuid));
        for (int i = 0; i < c->n_prev_bots && i < CB_MAX_SEATS; i++) memcpy(st->bots[st->n_bots++], c->prev_bots[i], sizeof(cb_uuid));
    } else rc = read_seats(x, g->id, st);
    if (rc == CB_OK) rc = seats_to(x, c, st);
    free(st);
    if (rc == CB_OK) rc = rate(x, c);
    return rc;
}

static int sql_game_commit(cb_store *s, const cb_game_commit *c) {
    if (!c || !c->row || c->n_humans < 0 || c->n_humans > CB_MAX_SEATS || c->n_bots < 0 || c->n_bots > CB_MAX_SEATS ||
        c->n_rated < 0 || c->n_rated > CB_MAX_SEATS || c->n_prev_humans < 0 || c->n_prev_bots < 0)
        return CB_E_INVALID;
    return cb_sql_write(s, commit_step, (void *)c);
}

typedef struct {
    const cb_game_row *g;
    const cb_uuid     *humans, *bots;
    int                n_humans, n_bots;
} import_op;

// The row as given (its whole log, if any, as one move at its version), with exactly these seats.
static int import_step(cb_sql *x, void *ud) {
    import_op *o = ud;
    cb_game_row row = *o->g;
    row.log_from = o->g->logs_len ? o->g->version : 0;
    stmt_t *s = cb_sql_stmt(x, S_GAME_REPLACE);
    if (!s) return CB_E_DB;
    game_bind(s, &row, row.last_commit_at);
    int rc = cb_sql_run(x, s, "game import");
    if (rc == CB_OK) rc = moves_write(x, row.id, true, row.version, row.logs, row.logs_len);
    if (rc == CB_OK) rc = run_game_id(x, S_MEMBERS_CLEAR, row.id, NULL);
    if (rc == CB_OK) rc = run_game_id(x, S_SEAT_BOTS_CLEAR, row.id, NULL);
    for (int i = 0; rc == CB_OK && i < o->n_humans; i++) rc = seat_write(x, S_MEMBER_ADD, row.id, o->humans[i], row.updated_at);
    for (int i = 0; rc == CB_OK && i < o->n_bots; i++) rc = seat_write(x, S_SEAT_BOT_ADD, row.id, o->bots[i], 0);
    return rc;
}

static int sql_game_import(cb_store *s, const cb_game_row *g, const cb_uuid *humans, int n_humans, const cb_uuid *bots,
                           int n_bots) {
    if (n_humans < 0 || n_humans > CB_MAX_SEATS || n_bots < 0 || n_bots > CB_MAX_SEATS) return CB_E_INVALID;
    import_op o = { g, humans, bots, n_humans, n_bots };
    return cb_sql_write(s, import_step, &o);
}

typedef struct {
    const char *id;
    int64_t     expected;
} delete_op;

// The row if still at the version, and with it its seats, its log and its chat; its replays stay, naming no game.
static int delete_step(cb_sql *x, void *ud) {
    delete_op *o = ud;
    stmt_t *s = cb_sql_stmt(x, S_GAME_DELETE);
    if (!s) return CB_E_DB;
    b_text(s, 1, o->id);
    b_i64(s, 2, o->expected);
    int rc = cb_sql_run(x, s, "game delete");
    if (rc != CB_OK) return rc;
    if (cb_sql_changes(x) == 0) {
        stmt_t *e = cb_sql_stmt(x, S_GAME_EXISTS);
        if (!e) return CB_E_DB;
        b_text(e, 1, o->id);
        int k = step_row(x, e, "game exists");
        sqlite3_reset(e);
        return k < 0 ? k : k ? CB_E_CONFLICT : CB_E_NOT_FOUND;
    }
    static const int AFTER[] = { S_MEMBERS_CLEAR, S_SEAT_BOTS_CLEAR, S_MOVES_CLEAR, S_CHAT_CLEAR, S_REPLAYS_ORPHAN };
    for (size_t i = 0; rc == CB_OK && i < sizeof AFTER / sizeof AFTER[0]; i++) rc = run_game_id(x, AFTER[i], o->id, NULL);
    return rc;
}

static int sql_game_delete(cb_store *s, const char *id, int64_t expected) {
    delete_op o = { id, expected };
    return cb_sql_write(s, delete_step, &o);
}

typedef struct {
    const cb_game_flush  *f;
    int                   n;
    const cb_game_commit *c;
    const char           *del_id;
    int64_t               del_version;
} flush_op;

static int flush_step(cb_sql *x, void *ud) {
    flush_op *o = ud;
    for (int i = 0; i < o->n; i++) {
        const cb_game_flush *f = &o->f[i];
        stmt_t *s = cb_sql_stmt(x, S_GAME_CAS);
        if (!s) return CB_E_DB;
        game_bind(s, f->row, f->row->last_commit_at);
        b_i64(s, G_EXPECTED, f->sql_version);
        int rc = cb_sql_run(x, s, "journal flush");
        if (rc != CB_OK) return rc;
        if (cb_sql_changes(x) == 0) {
            CB_ERROR("store", "journal flush: game %s is not at version %lld in the tables", f->row->id,
                     (long long)f->sql_version);
            return CB_E_CONFLICT;
        }
        for (int k = 0; k < f->n_moves; k++)
            if ((rc = moves_write(x, f->row->id, false, f->moves[k].seq, f->moves[k].log, f->moves[k].len)) != CB_OK) return rc;
    }
    if (o->c) return commit_step(x, (void *)o->c);
    if (o->del_id) {
        delete_op d = { o->del_id, o->del_version };
        return delete_step(x, &d);
    }
    return CB_OK;
}

int cb_rows_sql_flush(cb_store *s, const cb_game_flush *f, int n, const cb_game_commit *c, const char *del_id,
                      int64_t del_version) {
    flush_op o = { f, n, c, del_id, del_version };
    if (n == 0 && !c && !del_id) return CB_OK;
    return cb_sql_write(s, flush_step, &o);
}


// ---- membership and the startup scan -----------------------------------------------------------

typedef struct {
    const char *a, *b;     // the statement's text or UUID keys
    int64_t     n1, n2;
    int         limit;
    vec         v;
} list_op;

static int members_of_step(cb_sql *x, void *ud) {
    list_op *o = ud;
    stmt_t *s = cb_sql_stmt(x, S_GAMES_OF_USER);
    if (!s) return CB_E_DB;
    cb_sql_bind_uuid(s, 1, o->a);
    b_i64(s, 2, o->limit > 0 ? o->limit : NO_LIMIT);
    int k;
    while ((k = step_row(x, s, "games of user")) == 1) {
        cb_member_row *m = vec_push(&o->v);
        if (!m) { sqlite3_reset(s); return CB_E_NOMEM; }
        cb_sql_col_text(s, 0, m->game_id, sizeof m->game_id);
        m->updated_at = sqlite3_column_int64(s, 1);
    }
    sqlite3_reset(s);
    return k < 0 ? k : CB_OK;
}

// The games a user sits at, most recently updated first.
static int sql_games_of_user(cb_store *s, const char *user, int limit, cb_member_fn fn, void *ud) {
    list_op o = { .a = user, .limit = limit, .v = { .size = sizeof(cb_member_row) } };
    int rc = cb_sql_read(s, members_of_step, &o), n = 0;
    for (int i = 0; rc == CB_OK && i < o.v.n; i++) {
        n++;
        if (!fn(&((cb_member_row *)o.v.rows)[i], ud)) break;
    }
    free(o.v.rows);
    return rc == CB_OK ? n : rc;
}

static int is_member_step(cb_sql *x, void *ud) {
    list_op *o = ud;
    stmt_t *s = cb_sql_stmt(x, S_IS_MEMBER);
    if (!s) return CB_E_DB;
    b_text(s, 1, o->a);
    cb_sql_bind_uuid(s, 2, o->b);
    int k = step_row(x, s, "is member");
    sqlite3_reset(s);
    return k;
}

static int sql_is_member(cb_store *s, const char *game, const char *user) {
    list_op o = { .a = game, .b = user };
    return cb_sql_read(s, is_member_step, &o);
}

static int members_step(cb_sql *x, void *ud) {
    list_op *o = ud;
    seats_t *st = malloc(sizeof *st);
    if (!st) return CB_E_NOMEM;
    int rc = read_seats(x, o->a, st);
    for (int i = 0; rc == CB_OK && i < st->n_humans; i++) {
        cb_uuid *u = vec_push(&o->v);
        if (!u) rc = CB_E_NOMEM;
        else memcpy(*u, st->humans[i], sizeof *u);
    }
    free(st);
    return rc;
}

static int sql_game_members(cb_store *s, const char *game, cb_name_fn fn, void *ud) {
    list_op o = { .a = game, .v = { .size = sizeof(cb_uuid) } };
    int rc = cb_sql_read(s, members_step, &o), n = 0;
    for (int i = 0; rc == CB_OK && i < o.v.n; i++) {
        n++;
        if (!fn(((cb_uuid *)o.v.rows)[i], ud)) break;
    }
    free(o.v.rows);
    return rc == CB_OK ? n : rc;
}

static int needing_step(cb_sql *x, void *ud) {
    list_op *o = ud;
    stmt_t *s = cb_sql_stmt(x, S_NEEDING_BOTS);
    if (!s) return CB_E_DB;
    b_i64(s, 1, o->n1);
    b_i64(s, 2, o->n2);
    b_i64(s, 3, o->limit > 0 ? o->limit : NO_LIMIT);
    int k;
    while ((k = step_row(x, s, "games needing bots")) == 1) {
        char *id = vec_push(&o->v);
        if (!id) { sqlite3_reset(s); return CB_E_NOMEM; }
        cb_sql_col_text(s, 0, id, CB_GAME_ID_MAX + 1);
    }
    sqlite3_reset(s);
    return k < 0 ? k : CB_OK;
}

// The recovery scan at start (and the admin count): games with bots due, from the games table (a full
// scan: it runs once, not per move).
static int sql_games_needing_bots(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn,
                                  void *ud) {
    list_op o = { .n1 = stale_before, .n2 = active_after, .limit = limit, .v = { .size = CB_GAME_ID_MAX + 1 } };
    int rc = cb_sql_read(s, needing_step, &o), n = 0;
    for (int i = 0; rc == CB_OK && i < o.v.n; i++) {
        n++;
        if (!fn((char *)o.v.rows + (size_t)i * (CB_GAME_ID_MAX + 1), ud)) break;
    }
    free(o.v.rows);
    return rc == CB_OK ? n : rc;
}

typedef struct {
    const char *const *ids;
    int                n;
    vec                v;
} views_op;

static int views_step(cb_sql *x, void *ud) {
    views_op *o = ud;
    for (int i = 0; i < o->n; i++) {
        cb_game_row row;
        game_read r = { o->ids[i], &row };
        int rc = game_get_step(x, &r);
        if (rc == CB_E_NOT_FOUND) continue;
        if (rc != CB_OK) return rc;
        cb_game_row *e = vec_push(&o->v);
        if (!e) { cb_game_row_free(&row); return CB_E_NOMEM; }
        *e = row;
    }
    return CB_OK;
}

// The rows of ids for a list of views, read in one go.
static int sql_games_get_views(cb_store *s, const char *const *ids, int n, cb_game_fn fn, void *ud) {
    views_op o = { ids, n, { .size = sizeof(cb_game_row) } };
    int rc = cb_sql_read(s, views_step, &o), count = 0;
    bool more = rc == CB_OK;
    for (int i = 0; i < o.v.n; i++) {
        cb_game_row *g = &((cb_game_row *)o.v.rows)[i];
        if (more) {
            count++;
            more = fn(g, ud);
        }
        cb_game_row_free(g);
    }
    free(o.v.rows);
    return rc == CB_OK ? count : rc;
}

// ---- bots and ratings -----------------------------------------------------------------------

static void bot_of(stmt_t *s, cb_bot_row *b) {
    cb_sql_col_uuid(s, COL(B_ID), b->id);
    cb_sql_col_text(s, COL(B_NICKNAME), b->nickname, sizeof b->nickname);
    cb_sql_col_text(s, COL(B_STRATEGY), b->strategy_key, sizeof b->strategy_key);
    b->elo_rating = sqlite3_column_int(s, COL(B_ELO));
    b->previous_elo = sqlite3_column_int(s, COL(B_PREVIOUS_ELO));
    b->games_played = sqlite3_column_int(s, COL(B_PLAYED));
    b->created_at = sqlite3_column_int64(s, COL(B_CREATED_AT));
    b->updated_at = sqlite3_column_int64(s, COL(B_UPDATED_AT));
}

static void rating_of(stmt_t *s, cb_rating_row *r) {
    cb_sql_col_uuid(s, COL(R_USER), r->user_id);
    cb_sql_col_text(s, COL(R_USERNAME), r->username, sizeof r->username);
    r->elo_rating = sqlite3_column_int(s, COL(R_ELO));
    r->previous_elo = sqlite3_column_int(s, COL(R_PREVIOUS_ELO));
    r->games_played = sqlite3_column_int(s, COL(R_PLAYED));
    r->updated_at = sqlite3_column_int64(s, COL(R_UPDATED_AT));
}

static int bots_step(cb_sql *x, void *ud) {
    list_op *o = ud;
    stmt_t *s = cb_sql_stmt(x, S_BOTS_ALL);
    if (!s) return CB_E_DB;
    int k;
    while ((k = step_row(x, s, "bots")) == 1) {
        cb_bot_row *b = vec_push(&o->v);
        if (!b) { sqlite3_reset(s); return CB_E_NOMEM; }
        bot_of(s, b);
    }
    sqlite3_reset(s);
    return k < 0 ? k : CB_OK;
}

static int sql_bots_list(cb_store *s, cb_bot_fn fn, void *ud) {
    list_op o = { .v = { .size = sizeof(cb_bot_row) } };
    int rc = cb_sql_read(s, bots_step, &o), n = 0;
    if (rc == CB_OK) cb_store_bots_sort(o.v.rows, o.v.n);
    for (int i = 0; rc == CB_OK && i < o.v.n; i++) {
        n++;
        if (!fn(&((cb_bot_row *)o.v.rows)[i], ud)) break;
    }
    free(o.v.rows);
    return rc == CB_OK ? n : rc;
}

static int ratings_step(cb_sql *x, void *ud) {
    list_op *o = ud;
    stmt_t *s = cb_sql_stmt(x, S_RATINGS_PLAYED);
    if (!s) return CB_E_DB;
    b_i64(s, 1, o->limit > 0 ? o->limit : NO_LIMIT);
    int k;
    while ((k = step_row(x, s, "ratings")) == 1) {
        cb_rating_row *r = vec_push(&o->v);
        if (!r) { sqlite3_reset(s); return CB_E_NOMEM; }
        rating_of(s, r);
    }
    sqlite3_reset(s);
    return k < 0 ? k : CB_OK;
}

static int ratings_emit(cb_store *s, int limit, cb_rating_fn fn, void *ud) {
    list_op o = { .limit = limit, .v = { .size = sizeof(cb_rating_row) } };
    int rc = cb_sql_read(s, ratings_step, &o), n = 0;
    for (int i = 0; rc == CB_OK && i < o.v.n; i++) {
        n++;
        if (!fn(&((cb_rating_row *)o.v.rows)[i], ud)) break;
    }
    free(o.v.rows);
    return rc == CB_OK ? n : rc;
}

// The played rows, best first (the leaderboard's whole order).
static int sql_ratings_list(cb_store *s, cb_rating_fn fn, void *ud) { return ratings_emit(s, 0, fn, ud); }
static int sql_ratings_top(cb_store *s, int limit, cb_rating_fn fn, void *ud) {
    return limit <= 0 ? 0 : ratings_emit(s, limit, fn, ud);
}

typedef struct {
    const char *const *ids;
    int                n;
    int32_t           *out;
} elo_op;

static int elo_step(cb_sql *x, void *ud) {
    elo_op *o = ud;
    for (int i = 0; i < o->n; i++) {
        o->out[i] = TABLE_RATING_NONE;
        for (int pass = 0; pass < 2 && o->out[i] == TABLE_RATING_NONE; pass++) {   // a bot's row first
            stmt_t *s = cb_sql_stmt(x, pass ? S_USER_ELO : S_BOT_ELO);
            if (!s) return CB_E_DB;
            cb_sql_bind_uuid(s, 1, o->ids[i]);
            int k = step_row(x, s, "elo");
            if (k == 1) o->out[i] = sqlite3_column_int(s, 0);
            sqlite3_reset(s);
            if (k < 0) return k;
        }
    }
    return CB_OK;
}

static int sql_ratings_for(cb_store *s, const char *const *ids, int n, int32_t *out) {
    if (n < 0) return CB_E_INVALID;
    if (n == 0) return CB_OK;
    elo_op o = { ids, n, out };
    return cb_sql_read(s, elo_step, &o);
}

typedef struct {
    const char          *user, *name;
    const cb_rating_row *r;
    const cb_bot_row    *bots;
    int                  n;
} rating_op;

static int rating_name_step(cb_sql *x, void *ud) {
    rating_op *o = ud;
    stmt_t *s = cb_sql_stmt(x, S_RATING_NAME);
    if (!s) return CB_E_DB;
    cb_sql_bind_uuid(s, 1, o->user);
    cb_sql_bind_text_or_null(s, 2, o->name);
    return cb_sql_run(x, s, "rating name");   // no row: nothing to rename
}

static int sql_rating_set_username(cb_store *s, const char *user, const char *username) {
    rating_op o = { .user = user, .name = username };
    return cb_sql_write(s, rating_name_step, &o);
}

static int rating_put(cb_sql *x, const cb_rating_row *r) {
    stmt_t *s = cb_sql_stmt(x, S_RATING_PUT);
    if (!s) return CB_E_DB;
    if (!cb_sql_bind_uuid(s, R_USER, r->user_id)) return CB_E_INVALID;
    cb_sql_bind_text_or_null(s, R_USERNAME, r->username);
    b_i64(s, R_ELO, r->elo_rating);
    b_i64(s, R_PREVIOUS_ELO, r->previous_elo);
    b_i64(s, R_PLAYED, r->games_played);
    b_i64(s, R_UPDATED_AT, r->updated_at);
    return cb_sql_run(x, s, "rating put");
}

static int rating_import_step(cb_sql *x, void *ud) { return rating_put(x, ((rating_op *)ud)->r); }

static int sql_rating_import(cb_store *s, const cb_rating_row *r) {
    rating_op o = { .r = r };
    return cb_sql_write(s, rating_import_step, &o);
}

// The leaderboard's order is an index here, so every row already has it.
static int sql_rating_reindex(cb_store *s, const cb_rating_row *r) { return CB_E_CONFLICT; }

static int bots_replace_step(cb_sql *x, void *ud) {
    rating_op *o = ud;
    int rc = cb_sql_run(x, cb_sql_stmt(x, S_BOTS_CLEAR), "bots clear");
    for (int i = 0; rc == CB_OK && i < o->n; i++) {
        const cb_bot_row *b = &o->bots[i];
        stmt_t *s = cb_sql_stmt(x, S_BOT_PUT);
        if (!s) return CB_E_DB;
        if (!cb_sql_bind_uuid(s, B_ID, b->id)) return CB_E_INVALID;
        b_text(s, B_NICKNAME, b->nickname);
        b_text(s, B_STRATEGY, b->strategy_key);
        b_i64(s, B_ELO, b->elo_rating);
        b_i64(s, B_PREVIOUS_ELO, b->previous_elo);
        b_i64(s, B_PLAYED, b->games_played);
        b_i64(s, B_CREATED_AT, b->created_at);
        b_i64(s, B_UPDATED_AT, b->updated_at);
        rc = cb_sql_run(x, s, "bot put");
    }
    return rc;
}

static int sql_bots_replace(cb_store *s, const cb_bot_row *rows, int n) {
    if (n < 0) return CB_E_INVALID;
    rating_op o = { .bots = rows, .n = n };
    return cb_sql_write(s, bots_replace_step, &o);
}

// ---- snapshots ------------------------------------------------------------------------------

static int snapshot_put_step(cb_sql *x, void *ud) {
    const cb_snapshot_row *sn = ud;
    unsigned char players[CB_MAX_SEATS * CB_UUID_BYTES];
    for (int i = 0; i < sn->n_players; i++)
        if (!cb_uuid_pack(sn->player_ids[i], players + CB_UUID_BYTES * i)) return CB_E_INVALID;
    stmt_t *s = cb_sql_stmt(x, S_REPLAY_PUT);
    if (!s) return CB_E_DB;
    if (!cb_sql_bind_uuid(s, Y_ID, sn->id)) return CB_E_INVALID;
    cb_sql_bind_text_or_null(s, Y_GAME, sn->game_id);
    cb_sql_bind_bytes(s, Y_PLAYERS, players, (size_t)sn->n_players * CB_UUID_BYTES);
    cb_sql_bind_bytes(s, Y_MOVES, sn->moves, sn->moves_len);
    b_bytes_or_null(s, Y_EXTRAS, sn->extras, sn->extras_len);
    b_i64(s, Y_CREATED_AT, sn->created_at);
    if (sn->sum.has) {   // else NULL: no summary
        b_i64(s, Y_FOOL, sn->sum.fool);
        b_i64(s, Y_DUR, sn->sum.duration_s);
    }
    int rc = cb_sql_run(x, s, "replay put");
    for (int i = 0; rc == CB_OK && i < sn->n_players; i++) {
        if (!(s = cb_sql_stmt(x, S_REPLAY_PLAYER_PUT))) return CB_E_DB;
        cb_sql_bind_bytes(s, H_USER, players + CB_UUID_BYTES * i, CB_UUID_BYTES);
        b_i64(s, H_CREATED_AT, sn->created_at);
        cb_sql_bind_uuid(s, H_REPLAY, sn->id);
        b_i64(s, H_SEAT, i);   // a repeated id keeps its first seat (OR IGNORE)
        if (sn->sum.has) b_i64(s, H_RANK, cb_snapshot_rank(&sn->sum, sn->n_players, i));
        rc = cb_sql_run(x, s, "replay player");
    }
    return rc;
}

// The replay and a history row per player, in one transaction; a reused id is CB_E_CONFLICT.
static int sql_snapshot_put(cb_store *s, const cb_snapshot_row *sn) {
    if (sn->n_players < 0 || sn->n_players > CB_MAX_SEATS) return CB_E_INVALID;
    return cb_sql_write(s, snapshot_put_step, (void *)sn);
}

typedef struct {
    const cb_snapshot_query *q;
    vec                      v;   // cb_snapshot_row
} snaps_op;

static int snaps_step(cb_sql *x, void *ud) {
    snaps_op *o = ud;
    const cb_snapshot_query *q = o->q;
    stmt_t *s = cb_sql_stmt(x, q->newest_first ? S_REPLAYS_NEW : S_REPLAYS_OLD);
    if (!s) return CB_E_DB;
    cb_sql_bind_uuid(s, Q_VIEWER, q->viewer_id);
    // A filter that is not a UUID matches nothing: an empty blob, never NULL (which would match any).
    if (q->id && !cb_sql_bind_uuid(s, Q_ID, q->id)) sqlite3_bind_zeroblob(s, Q_ID, 0);
    if (q->game_id) b_text(s, Q_GAME, q->game_id);
    if (q->ref) {
        if (!cb_sql_bind_uuid(s, Q_REF_ID, q->ref)) sqlite3_bind_zeroblob(s, Q_REF_ID, 0);
        b_text(s, Q_REF_GAME, q->ref);
    }
    b_i64(s, Q_LIMIT, q->limit > 0 ? q->limit : NO_LIMIT);
    int k, rc = CB_OK;
    while (rc == CB_OK && (k = step_row(x, s, "replays")) == 1) {
        cb_snapshot_row *r = vec_push(&o->v);
        if (!r) { rc = CB_E_NOMEM; break; }
        cb_sql_col_uuid(s, COL(Y_ID), r->id);
        cb_sql_col_text(s, COL(Y_GAME), r->game_id, sizeof r->game_id);
        const unsigned char *p = sqlite3_column_blob(s, COL(Y_PLAYERS));
        int np = sqlite3_column_bytes(s, COL(Y_PLAYERS)) / CB_UUID_BYTES;
        r->n_players = np > CB_MAX_SEATS ? CB_MAX_SEATS : np;
        r->created_at = sqlite3_column_int64(s, COL(Y_CREATED_AT));
        bool has = sqlite3_column_type(s, COL(Y_DUR)) != SQLITE_NULL;
        bool want_moves = !q->summary_only || !has;
        if ((want_moves && !cb_sql_col_bytes(s, COL(Y_MOVES), &r->moves, &r->moves_len)) ||
            !cb_sql_col_bytes(s, COL(Y_EXTRAS), &r->extras, &r->extras_len))
            rc = CB_E_NOMEM;
        if (q->summary_only) {
            // The history row: numbers and the names (extras); no player ids.
            r->seat = sqlite3_column_int(s, COL(Y_SEAT));
            r->rank = sqlite3_column_type(s, COL(Y_RANK)) == SQLITE_NULL ? 0 : sqlite3_column_int(s, COL(Y_RANK));
            r->sum.has = has;
            r->sum.fool = (int8_t)(has ? sqlite3_column_int(s, COL(Y_FOOL)) : -1);
            r->sum.duration_s = has ? sqlite3_column_int(s, COL(Y_DUR)) : -1;
        } else {
            for (int i = 0; i < r->n_players; i++) cb_uuid_unpack(p + CB_UUID_BYTES * i, r->player_ids[i]);
        }
    }
    sqlite3_reset(s);
    return rc != CB_OK ? rc : k < 0 ? k : CB_OK;
}

// The viewer's replays (store.h snapshot query): one indexed read of the viewer's history rows joined to
// the replays, in the query's order.
static int sql_snapshots_query(cb_store *s, const cb_snapshot_query *q, cb_snapshot_fn fn, void *ud) {
    if (!q || !q->viewer_id || !*q->viewer_id) return CB_E_INVALID;
    snaps_op o = { q, { .size = sizeof(cb_snapshot_row) } };
    int rc = cb_sql_read(s, snaps_step, &o), n = 0;
    bool more = rc == CB_OK;
    for (int i = 0; i < o.v.n; i++) {
        cb_snapshot_row *r = &((cb_snapshot_row *)o.v.rows)[i];
        if (more) {
            n++;
            more = fn(r, ud);
        }
        cb_snapshot_row_free(r);
    }
    free(o.v.rows);
    return rc == CB_OK ? n : rc;
}

typedef struct {
    const char    *id;
    const uint8_t *extras;
    size_t         len;
} extras_op;

static int extras_step(cb_sql *x, void *ud) {
    extras_op *o = ud;
    stmt_t *s = cb_sql_stmt(x, S_REPLAY_EXTRAS);
    if (!s) return CB_E_DB;
    cb_sql_bind_uuid(s, 1, o->id);
    b_bytes_or_null(s, 2, o->extras, o->len);
    int rc = cb_sql_run(x, s, "replay extras");
    return rc == CB_OK && cb_sql_changes(x) == 0 ? CB_E_NOT_FOUND : rc;
}

// Account deletion: one replay's extras replaced (the names live nowhere else).
static int sql_snapshot_set_extras(cb_store *s, const char *id, const uint8_t *extras, size_t extras_len) {
    extras_op o = { id, extras, extras_len };
    return cb_sql_write(s, extras_step, &o);
}

// ---- chat -----------------------------------------------------------------------------------

static int chat_put_step(cb_sql *x, void *ud) {
    const cb_chat_row *c = ud;
    stmt_t *s = cb_sql_stmt(x, S_CHAT_PUT);
    if (!s) return CB_E_DB;
    b_text(s, C_GAME, c->game_id);
    b_i64(s, C_CREATED_AT, c->created_at);
    if (!cb_sql_bind_uuid(s, C_ID, c->id)) return CB_E_INVALID;
    cb_sql_bind_uuid(s, C_USER, c->user_id);
    b_text(s, C_MESSAGE, c->message);
    b_i64(s, C_SYSTEM, c->is_system);
    int rc = cb_sql_run(x, s, "chat put");
    return rc == CB_OK && cb_sql_changes(x) == 0 ? CB_E_NOT_FOUND : rc;
}

static int sql_chat_put(cb_store *s, const cb_chat_row *c) { return cb_sql_write(s, chat_put_step, (void *)c); }

typedef struct {
    const char *game;
    int         limit;
    vec         v;   // cb_chat_row
} chat_op;

static int chat_list_step(cb_sql *x, void *ud) {
    chat_op *o = ud;
    stmt_t *s = cb_sql_stmt(x, S_CHAT_LIST);
    if (!s) return CB_E_DB;
    b_text(s, C_GAME, o->game);
    b_i64(s, C_LIMIT, o->limit > 0 ? o->limit : NO_LIMIT);
    int k, rc = CB_OK;
    while (rc == CB_OK && (k = step_row(x, s, "chat list")) == 1) {
        cb_chat_row *c = vec_push(&o->v);
        if (!c) { rc = CB_E_NOMEM; break; }
        cb_sql_col_text(s, COL(C_GAME), c->game_id, sizeof c->game_id);
        c->created_at = sqlite3_column_int64(s, COL(C_CREATED_AT));
        cb_sql_col_uuid(s, COL(C_ID), c->id);
        cb_sql_col_uuid(s, COL(C_USER), c->user_id);
        const unsigned char *m = sqlite3_column_text(s, COL(C_MESSAGE));
        c->message = strdup(m ? (const char *)m : "");
        if (!c->message) rc = CB_E_NOMEM;
        c->is_system = sqlite3_column_int(s, COL(C_SYSTEM)) != 0;
    }
    sqlite3_reset(s);
    return rc != CB_OK ? rc : k < 0 ? k : CB_OK;
}

// The newest `limit`, handed out oldest first.
static int sql_chat_list(cb_store *s, const char *game, int limit, cb_chat_fn fn, void *ud) {
    chat_op o = { game, limit, { .size = sizeof(cb_chat_row) } };
    int rc = cb_sql_read(s, chat_list_step, &o), n = 0;
    bool more = rc == CB_OK;
    for (int i = o.v.n - 1; i >= 0; i--) {
        cb_chat_row *c = &((cb_chat_row *)o.v.rows)[i];
        if (more) {
            n++;
            more = fn(c, ud);
        }
        cb_chat_row_free(c);
    }
    free(o.v.rows);
    return rc == CB_OK ? n : rc;
}

// ---- an account's part of foolish -------------------------------------------------------------

// A signup's rating row (the start rating, previous_elo the start too), in the account's transaction.
static int sql_signup(cb_sql *x, const cb_user_row *u) {
    cb_rating_row rt = { .elo_rating = TABLE_RATING_START, .previous_elo = TABLE_RATING_START, .updated_at = u->created_at };
    memcpy(rt.user_id, u->id, sizeof rt.user_id);
    memcpy(rt.username, u->username, sizeof rt.username);
    return rating_put(x, &rt);
}

// What a deleted account leaves of foolish's: its seats, its chat, its rating row and its history (the
// list only it could read), in the delete's transaction. The replays stay for the other players, their
// names redacted first (functions_http.c); a replay's players keep the id, as a game's roster does.
static int sql_delete(cb_sql *x, const char *user) {
    static const int IDS[] = { S_DEL_MEMBERS, S_DEL_CHAT, S_DEL_RATING, S_DEL_HISTORY };
    int rc = CB_OK;
    for (size_t i = 0; rc == CB_OK && i < sizeof IDS / sizeof IDS[0]; i++) {
        stmt_t *s = cb_sql_stmt(x, IDS[i]);
        if (s) cb_sql_bind_uuid(s, 1, user);
        rc = cb_sql_run(x, s, STMTS[IDS[i]]);
    }
    return rc;
}

const cb_rows_ops cb_rows_sql = {
    .game_get = sql_game_get,
    .game_logs = sql_game_logs,
    .games_get_views = sql_games_get_views,
    .game_create = sql_game_create,
    .game_commit = sql_game_commit,
    .game_import = sql_game_import,
    .game_delete = sql_game_delete,
    .games_of_user = sql_games_of_user,
    .is_member = sql_is_member,
    .game_members = sql_game_members,
    .games_needing_bots = sql_games_needing_bots,
    .bots_list = sql_bots_list,
    .ratings_list = sql_ratings_list,
    .ratings_top = sql_ratings_top,
    .ratings_for = sql_ratings_for,
    .rating_set_username = sql_rating_set_username,
    .rating_import = sql_rating_import,
    .rating_reindex = sql_rating_reindex,
    .bots_replace = sql_bots_replace,
    .snapshot_put = sql_snapshot_put,
    .snapshots_query = sql_snapshots_query,
    .snapshot_set_extras = sql_snapshot_set_extras,
    .chat_put = sql_chat_put,
    .chat_list = sql_chat_list,
};

// ---- the migration's walk ---------------------------------------------------------------------

const char *const CB_ROWS_FAMILIES[CB_ROWS_N_FAMILIES] = {
    "users", "bots", "user_elo_ratings", "games", "player_hands", "bot_hands", "game_snapshots", "chat_messages",
};

typedef struct {
    vec ids;   // char[CB_ROWS_KEY_MAX]
    vec fam;   // int
} keys_op;

static int keys_step(cb_sql *x, void *ud) {
    keys_op *o = ud;
    for (int f = 0; f < CB_ROWS_N_FAMILIES; f++) {
        stmt_t *s = cb_sql_stmt(x, S_KEYS_USERS + f);
        if (!s) return CB_E_DB;
        int k;
        while ((k = step_row(x, s, "keys")) == 1) {
            char *id = vec_push(&o->ids);
            int *fam = vec_push(&o->fam);
            if (!id || !fam) { sqlite3_reset(s); return CB_E_NOMEM; }
            *fam = f;
            if (sqlite3_column_type(s, 0) == SQLITE_BLOB) cb_sql_col_uuid(s, 0, id);
            else cb_sql_col_text(s, 0, id, CB_ROWS_KEY_MAX);
        }
        sqlite3_reset(s);
        if (k < 0) return k;
    }
    return CB_OK;
}

int cb_rows_sql_keys(cb_store *s, cb_rows_key_fn fn, void *ud) {
    if (!cb_store_is_sql(s)) return CB_E_INVALID;
    keys_op o = { .ids = { .size = CB_ROWS_KEY_MAX }, .fam = { .size = sizeof(int) } };
    int rc = cb_sql_read(s, keys_step, &o);
    for (int i = 0; rc == CB_OK && i < o.ids.n; i++)
        if (!fn(CB_ROWS_FAMILIES[((int *)o.fam.rows)[i]], (char *)o.ids.rows + (size_t)i * CB_ROWS_KEY_MAX, ud)) break;
    free(o.ids.rows);
    free(o.fam.rows);
    return rc;
}

// foolish on every store: on the SQL store the rows above (and its tables and statements), on the item store
// rows_foolish.c's over the items; its part of an account on each.
const cb_store_app cb_foolish_store = {
    .rows = &cb_rows_foolish,
    .signup_txn = cb_rows_item_signup_txn,
    .delete_keys = cb_rows_item_delete_keys,
    .delete_txn = cb_rows_item_delete_txn,
    .seed = cb_import_seed_bots,
    .sql_rows = &cb_rows_sql,
    .sql_schema = cb_foolish_schema_sql,
    .sql_stmts = STMTS,
    .n_sql_stmts = S__COUNT,
    .sql_signup = sql_signup,
    .sql_delete = sql_delete,
};
