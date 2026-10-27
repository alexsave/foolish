/* journal.c - see journal.h. */
#include "journal.h"
#include "puzzles.h"

#include "store/store_sql.h"
#include "util/err.h"
#include "util/log.h"
#include "util/clock.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

const char *const cc_sql_stmts[CC_SQL_N] = {
    [CC_SQL_PLAYER_PUT] = "INSERT OR REPLACE INTO cc_players (id, name, games, wins, losses, draws, last_at, pr, pd, pv, pz_tried, pz_solved) "
                          "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)",
    [CC_SQL_PLAYERS_ALL] = "SELECT id, name, games, wins, losses, draws, last_at, pr, pd, pv, pz_tried, pz_solved FROM cc_players",
    [CC_SQL_RATING_PUT] = "INSERT OR REPLACE INTO cc_ratings (player, speed, r, d, v, games) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
    [CC_SQL_RATINGS_ALL] = "SELECT player, speed, r, d, v, games FROM cc_ratings",
    [CC_SQL_GAME_START] = "INSERT OR REPLACE INTO cc_games (id, white, black, base_s, inc_s, rated, status, winner, started_at, ended_at, "
                          "r_w, r_b, d_w, d_b, moves) VALUES (?1, ?2, ?3, ?4, ?5, ?6, 1, 2, ?7, 0, ?8, ?9, 0, 0, NULL)",
    [CC_SQL_GAME_END] = "UPDATE cc_games SET status = ?2, winner = ?3, ended_at = ?4, d_w = ?5, d_b = ?6, moves = ?7, left_w = ?8, left_b = ?9 WHERE id = ?1",
    [CC_SQL_POINT_PUT] = "INSERT INTO cc_points (player, speed, at, rating) VALUES (?1, ?2, ?3, ?4)",
    [CC_SQL_GAMES_PLAYING] = "SELECT id, white, black, base_s, inc_s, rated, started_at, r_w, r_b FROM cc_games WHERE status = 1",
    [CC_SQL_GAME_MAX] = "SELECT COALESCE(MAX(id), 0) FROM cc_games",
    [CC_SQL_GAME_GET] = "SELECT white, black, base_s, inc_s, rated, status, winner, started_at, ended_at, r_w, r_b, d_w, d_b, moves, left_w, left_b "
                        "FROM cc_games WHERE id = ?1",
    [CC_SQL_GAMES_OF] = "SELECT id, white, black, base_s, inc_s, rated, status, winner, ended_at, d_w, d_b, length(moves) / 2 FROM cc_games "
                        "WHERE (white = ?1 OR black = ?1) AND status > 1 ORDER BY id DESC LIMIT 50",
    [CC_SQL_PUZZLE_PUT] = "INSERT OR REPLACE INTO cc_puzzles (id, r, d, v, plays) VALUES (?1, ?2, ?3, ?4, ?5)",
    [CC_SQL_PUZZLES_ALL] = "SELECT id, r, d, v, plays FROM cc_puzzles",
    /* the latest ?2 of each speed, by speed, oldest first in each (rowid breaks a tie of the same ms) */
    [CC_SQL_POINTS_OF] = "SELECT speed, at, rating FROM (SELECT speed, at, rating, rowid AS o, "
                         "ROW_NUMBER() OVER (PARTITION BY speed ORDER BY at DESC, rowid DESC) AS k FROM cc_points WHERE player = ?1) "
                         "WHERE k <= ?2 ORDER BY speed, at, o",
};

/* ---- the records ---- */

enum { J_PLAYER, J_RATING, J_GAME_START, J_GAME_END, J_POINT, J_PUZZLE };

typedef struct jrec {
    struct jrec *next;
    int          kind;
    union {
        cc_player player;   /* the fields that are written; the chains are not read */
        struct { char id[CB_UUID_LEN + 1]; int speed; uint32_t games; g2_player r; } rating;
        struct { uint32_t id; char w[CB_UUID_LEN + 1], b[CB_UUID_LEN + 1]; int base_s, inc_s, rated; int64_t at; int r0[2]; } start;
        struct { uint32_t id; int status, winner, d[2]; int64_t at, left[2]; int n; uint16_t moves[CC_MAX_PLY]; } end;
        struct { char id[CB_UUID_LEN + 1]; int speed; int64_t at; int rating; } point;
        struct { uint32_t id, plays; g2_player r; } puzzle;
    } u;
} jrec;

static struct {
    cb_ctx         *ctx;
    pthread_t       thread;
    pthread_mutex_t mu;
    pthread_cond_t  cv, done_cv;
    jrec           *head, *tail;
    bool            running, stop;
    uint64_t        queued, committed, commits;
    /* the moves file */
    int             mfd;
    char            mpath[1100];
    uint8_t        *mbuf;          /* records handed over and not yet written */
    size_t          mlen, mcap;
    uint64_t        mqueued, mwritten;   /* bytes */
    int             sync_ms, batch_us;
} J = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .done_cv = PTHREAD_COND_INITIALIZER, .mfd = -1 };

/* A move record: the game, the ply it was played at, the move, the mover's time after it. */
typedef struct { uint32_t game; uint16_t ply, move; int32_t left_ms; } mrec;
_Static_assert(sizeof(mrec) == 12, "mrec");

static void enqueue(jrec *r) {
    if (!J.running) { free(r); return; }   /* no store (a test without one): nothing to keep */
    r->next = NULL;
    pthread_mutex_lock(&J.mu);
    if (J.tail) J.tail->next = r; else J.head = r;
    J.tail = r;
    J.queued++;
    pthread_cond_signal(&J.cv);
    pthread_mutex_unlock(&J.mu);
}

static jrec *rec(int kind) {
    jrec *r = malloc(sizeof *r);
    if (r) r->kind = kind;
    else CB_ERROR("cc", "journal: out of memory, a record is lost");
    return r;
}

void cc_journal_player(const cc_player *p) {
    jrec *r = rec(J_PLAYER);
    if (!r) return;
    r->u.player = *p;
    enqueue(r);
}

void cc_journal_game_start(const cc_live *l) {
    jrec *r = rec(J_GAME_START);
    if (!r) return;
    r->u.start.id = l->id;
    memcpy(r->u.start.w, l->p[0]->id, sizeof r->u.start.w);
    memcpy(r->u.start.b, l->p[1]->id, sizeof r->u.start.b);
    r->u.start.base_s = l->g.base_ms / 1000;
    r->u.start.inc_s = l->g.inc_ms / 1000;
    r->u.start.rated = l->rated;
    r->u.start.at = l->started_at;
    r->u.start.r0[0] = l->rating0[0];
    r->u.start.r0[1] = l->rating0[1];
    enqueue(r);
}

void cc_journal_move(const cc_live *l) {
    if (J.mfd < 0) return;
    int ply = l->g.ply - 1;
    int64_t left = l->g.left_ms[ply & 1];
    mrec r = { l->id, (uint16_t)ply, l->g.moves[ply], (int32_t)(left > INT32_MAX ? INT32_MAX : left) };
    pthread_mutex_lock(&J.mu);
    if (J.mlen + sizeof r > J.mcap) {
        size_t c = J.mcap ? J.mcap * 2 : 1 << 16;
        uint8_t *nb = realloc(J.mbuf, c);
        if (!nb) { pthread_mutex_unlock(&J.mu); CB_ERROR("cc", "journal: out of memory, a move is lost"); return; }
        J.mbuf = nb;
        J.mcap = c;
    }
    bool first = J.mlen == 0;
    memcpy(J.mbuf + J.mlen, &r, sizeof r);
    J.mlen += sizeof r;
    J.mqueued += sizeof r;
    if (first) pthread_cond_signal(&J.cv);   /* the writer sleeps only on an empty buffer */
    pthread_mutex_unlock(&J.mu);
}

static void write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0) { if (errno == EINTR) continue; CB_ERROR("cc", "journal: the moves file refused a write: %s", strerror(errno)); return; }
        p += k;
        n -= (size_t)k;
    }
}

void cc_journal_game_end(const cc_live *l, int64_t wall) {
    jrec *r = rec(J_GAME_END);
    if (!r) return;
    r->u.end.id = l->id;
    r->u.end.status = l->g.status;
    r->u.end.winner = l->g.winner;
    r->u.end.d[0] = l->diff[0];
    r->u.end.d[1] = l->diff[1];
    r->u.end.at = wall;
    r->u.end.left[0] = l->g.left_ms[0];
    r->u.end.left[1] = l->g.left_ms[1];
    r->u.end.n = l->g.ply;
    for (int i = 0; i < l->g.ply; i++) r->u.end.moves[i] = l->g.moves[i];   /* stored little-endian (every target is) */
    enqueue(r);
}

void cc_journal_rating(const cc_player *p, int speed) {
    jrec *r = rec(J_RATING);
    if (!r) return;
    memcpy(r->u.rating.id, p->id, sizeof r->u.rating.id);
    r->u.rating.speed = speed;
    r->u.rating.games = p->rated[speed];
    r->u.rating.r = p->r[speed];
    enqueue(r);
}

void cc_journal_point(const cc_player *p, int speed, int64_t wall) {
    jrec *r = rec(J_POINT);
    if (!r) return;
    memcpy(r->u.point.id, p->id, sizeof r->u.point.id);
    r->u.point.speed = speed;
    r->u.point.at = wall;
    r->u.point.rating = cc_rating_int(&p->r[speed]);
    enqueue(r);
}

void cc_journal_puzzle(uint32_t id, const g2_player *g, uint32_t plays) {
    jrec *r = rec(J_PUZZLE);
    if (!r) return;
    r->u.puzzle.id = id;
    r->u.puzzle.plays = plays;
    r->u.puzzle.r = *g;
    enqueue(r);
}

/* ---- the writer ---- */

static int write_one(cb_sql *x, const jrec *r) {
    sqlite3_stmt *st;
    switch (r->kind) {
    case J_PLAYER: {
        const cc_player *p = &r->u.player;
        if (!(st = cb_sql_stmt(x, CC_SQL_PLAYER_PUT))) return CB_E_DB;
        cb_sql_bind_uuid(st, 1, p->id);
        sqlite3_bind_text(st, 2, p->name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, p->games);
        sqlite3_bind_int64(st, 4, p->wins);
        sqlite3_bind_int64(st, 5, p->losses);
        sqlite3_bind_int64(st, 6, p->draws);
        sqlite3_bind_int64(st, 7, p->last_at);
        sqlite3_bind_double(st, 8, p->pz.rating);
        sqlite3_bind_double(st, 9, p->pz.deviation);
        sqlite3_bind_double(st, 10, p->pz.volatility);
        sqlite3_bind_int64(st, 11, p->pz_tried);
        sqlite3_bind_int64(st, 12, p->pz_solved);
        return cb_sql_run(x, st, "cc player put");
    }
    case J_RATING:
        if (!(st = cb_sql_stmt(x, CC_SQL_RATING_PUT))) return CB_E_DB;
        cb_sql_bind_uuid(st, 1, r->u.rating.id);
        sqlite3_bind_int(st, 2, r->u.rating.speed);
        sqlite3_bind_double(st, 3, r->u.rating.r.rating);
        sqlite3_bind_double(st, 4, r->u.rating.r.deviation);
        sqlite3_bind_double(st, 5, r->u.rating.r.volatility);
        sqlite3_bind_int64(st, 6, r->u.rating.games);
        return cb_sql_run(x, st, "cc rating put");
    case J_GAME_START:
        if (!(st = cb_sql_stmt(x, CC_SQL_GAME_START))) return CB_E_DB;
        sqlite3_bind_int64(st, 1, r->u.start.id);
        cb_sql_bind_uuid(st, 2, r->u.start.w);
        cb_sql_bind_uuid(st, 3, r->u.start.b);
        sqlite3_bind_int(st, 4, r->u.start.base_s);
        sqlite3_bind_int(st, 5, r->u.start.inc_s);
        sqlite3_bind_int(st, 6, r->u.start.rated);
        sqlite3_bind_int64(st, 7, r->u.start.at);
        sqlite3_bind_int(st, 8, r->u.start.r0[0]);
        sqlite3_bind_int(st, 9, r->u.start.r0[1]);
        return cb_sql_run(x, st, "cc game start");
    case J_GAME_END:
        if (!(st = cb_sql_stmt(x, CC_SQL_GAME_END))) return CB_E_DB;
        sqlite3_bind_int64(st, 1, r->u.end.id);
        sqlite3_bind_int(st, 2, r->u.end.status);
        sqlite3_bind_int(st, 3, r->u.end.winner);
        sqlite3_bind_int64(st, 4, r->u.end.at);
        sqlite3_bind_int(st, 5, r->u.end.d[0]);
        sqlite3_bind_int(st, 6, r->u.end.d[1]);
        sqlite3_bind_blob(st, 7, r->u.end.moves, r->u.end.n * 2, SQLITE_STATIC);
        sqlite3_bind_int64(st, 8, r->u.end.left[0]);
        sqlite3_bind_int64(st, 9, r->u.end.left[1]);
        return cb_sql_run(x, st, "cc game end");
    case J_POINT:
        if (!(st = cb_sql_stmt(x, CC_SQL_POINT_PUT))) return CB_E_DB;
        cb_sql_bind_uuid(st, 1, r->u.point.id);
        sqlite3_bind_int(st, 2, r->u.point.speed);
        sqlite3_bind_int64(st, 3, r->u.point.at);
        sqlite3_bind_int(st, 4, r->u.point.rating);
        return cb_sql_run(x, st, "cc point");
    case J_PUZZLE:
        if (!(st = cb_sql_stmt(x, CC_SQL_PUZZLE_PUT))) return CB_E_DB;
        sqlite3_bind_int64(st, 1, r->u.puzzle.id);
        sqlite3_bind_double(st, 2, r->u.puzzle.r.rating);
        sqlite3_bind_double(st, 3, r->u.puzzle.r.deviation);
        sqlite3_bind_double(st, 4, r->u.puzzle.r.volatility);
        sqlite3_bind_int64(st, 5, r->u.puzzle.plays);
        return cb_sql_run(x, st, "cc puzzle");
    }
    return CB_E_INVALID;
}

static int write_batch(cb_sql *x, void *ud) {
    for (const jrec *r = ud; r; r = r->next) {
        int rc = write_one(x, r);
        if (rc != CB_OK) CB_ERROR("cc", "journal: a record of kind %d was refused: %s", r->kind, cb_strerror(rc));
    }
    return CB_OK;
}

static void *writer_main(void *ud) {
    (void)ud;
    int64_t last_sync = cb_mono_ms();
    bool dirty = false;
    uint8_t *spare = NULL;
    size_t spare_cap = 0;
    pthread_mutex_lock(&J.mu);
    for (;;) {
        while (!J.head && !J.mlen && !J.stop) {
            if (!dirty) { pthread_cond_wait(&J.cv, &J.mu); continue; }
            /* moves were written and not yet synced: wait at most until the sync is due */
            int64_t due = last_sync + J.sync_ms - cb_mono_ms();
            if (due <= 0) break;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (long)(due % 1000) * 1000000;
            ts.tv_sec += due / 1000 + ts.tv_nsec / 1000000000;
            ts.tv_nsec %= 1000000000;
            pthread_cond_timedwait(&J.cv, &J.mu, &ts);
        }
        if (!J.head && !J.mlen && J.stop && !dirty) break;
        if (!J.head && J.mlen && !J.stop && J.batch_us) {
            /* moves alone: let a few milliseconds of them gather into one write */
            pthread_mutex_unlock(&J.mu);
            usleep((useconds_t)J.batch_us);
            pthread_mutex_lock(&J.mu);
        }
        jrec *batch = J.head;
        uint64_t n = 0;
        for (jrec *r = batch; r; r = r->next) n++;
        J.head = J.tail = NULL;
        /* the moves: swap buffers, so the state keeps appending while this one is written */
        uint8_t *moves = J.mbuf;
        size_t mlen = J.mlen, mcap = J.mcap;
        J.mbuf = spare;
        J.mcap = spare_cap;
        J.mlen = 0;
        pthread_mutex_unlock(&J.mu);
        if (mlen && J.mfd >= 0) { write_all(J.mfd, moves, mlen); dirty = true; }
        spare = moves;
        spare_cap = mcap;
        if (batch) {
            int rc = cb_sql_write(J.ctx->store, write_batch, batch);
            if (rc != CB_OK) CB_ERROR("cc", "journal: a commit of %llu records failed: %s", (unsigned long long)n, cb_strerror(rc));
            while (batch) { jrec *nx = batch->next; free(batch); batch = nx; }
        }
        if (dirty && (cb_mono_ms() - last_sync >= J.sync_ms || J.stop)) {
            if (J.mfd >= 0) fsync(J.mfd);
            last_sync = cb_mono_ms();
            dirty = false;
        }
        pthread_mutex_lock(&J.mu);
        J.committed += n;
        J.mwritten += mlen;
        if (n) J.commits++;
        pthread_cond_broadcast(&J.done_cv);
    }
    pthread_mutex_unlock(&J.mu);
    free(spare);
    return NULL;
}

int cc_journal_start(cb_ctx *ctx) {
    J.ctx = ctx;
    J.stop = false;
    const char *v = getenv("CC_JOURNAL_SYNC_MS");
    J.sync_ms = v && *v ? atoi(v) : 100;
    v = getenv("CC_JOURNAL_BATCH_US");
    J.batch_us = v && *v ? atoi(v) : 2000;
    if (J.sync_ms < 0) J.sync_ms = 0;
    if (!ctx->store || !cb_store_is_sql(ctx->store)) return CB_OK;
    if (pthread_create(&J.thread, NULL, writer_main, NULL) != 0) return CB_E_NOMEM;
    J.running = true;
    return CB_OK;
}

void cc_journal_sync(void) {
    pthread_mutex_lock(&J.mu);
    uint64_t want = J.queued, mwant = J.mqueued;
    while (J.running && (J.committed < want || J.mwritten < mwant)) pthread_cond_wait(&J.done_cv, &J.mu);
    pthread_mutex_unlock(&J.mu);
}

void cc_journal_stop(void) {
    if (!J.running) return;
    pthread_mutex_lock(&J.mu);
    J.stop = true;
    pthread_cond_signal(&J.cv);
    pthread_mutex_unlock(&J.mu);
    pthread_join(J.thread, NULL);
    J.running = false;
    if (J.mfd >= 0) { fsync(J.mfd); close(J.mfd); J.mfd = -1; }
    free(J.mbuf);
    J.mbuf = NULL;
    J.mlen = J.mcap = 0;
}

void cc_journal_counts(uint64_t *queued, uint64_t *committed, uint64_t *commits) {
    pthread_mutex_lock(&J.mu);
    *queued = J.queued;
    *committed = J.committed;
    *commits = J.commits;
    pthread_mutex_unlock(&J.mu);
}

/* ---- recovery ---- */

cc_player *cc_player_recovered(const cc_player *in);
void cc_live_recovered(cc_live *l, int64_t now);
void cc_state_next_game_id(uint32_t id);

typedef struct { int rc; uint32_t players, ratings, games; char err[160]; } recover_ctx;

static void col_text(sqlite3_stmt *st, int c, char *out, size_t cap) {
    const unsigned char *s = sqlite3_column_text(st, c);
    snprintf(out, cap, "%s", s ? (const char *)s : "");
}

static int recover_fn(cb_sql *x, void *ud) {
    recover_ctx *c = ud;
    int64_t now = cc_mono();
    sqlite3_stmt *st = cb_sql_stmt(x, CC_SQL_PLAYERS_ALL);
    if (!st) return CB_E_DB;
    while (sqlite3_step(st) == SQLITE_ROW) {
        cc_player p;
        memset(&p, 0, sizeof p);
        cb_sql_col_uuid(st, 0, p.id);
        col_text(st, 1, p.name, sizeof p.name);
        for (int k = 0; k < CC_SPEEDS; k++) p.r[k] = cc_rating_new();   /* a speed never rated has no row */
        p.games = (uint32_t)sqlite3_column_int64(st, 2);
        p.wins = (uint32_t)sqlite3_column_int64(st, 3);
        p.losses = (uint32_t)sqlite3_column_int64(st, 4);
        p.draws = (uint32_t)sqlite3_column_int64(st, 5);
        p.last_at = sqlite3_column_int64(st, 6);
        p.pz = (g2_player){ sqlite3_column_double(st, 7), sqlite3_column_double(st, 8), sqlite3_column_double(st, 9) };
        p.pz_tried = (uint32_t)sqlite3_column_int64(st, 10);
        p.pz_solved = (uint32_t)sqlite3_column_int64(st, 11);
        if (!cc_player_recovered(&p)) { sqlite3_reset(st); return CB_E_NOMEM; }
        c->players++;
    }
    sqlite3_reset(st);
    if (!(st = cb_sql_stmt(x, CC_SQL_RATINGS_ALL))) return CB_E_DB;
    while (sqlite3_step(st) == SQLITE_ROW) {   /* onto the players just read */
        char id[CB_UUID_LEN + 1];
        cb_sql_col_uuid(st, 0, id);
        int k = sqlite3_column_int(st, 1);
        cc_player *p = cc_player_get(id);
        if (!p || k < 0 || k >= CC_SPEEDS) continue;
        p->r[k] = (g2_player){ sqlite3_column_double(st, 2), sqlite3_column_double(st, 3), sqlite3_column_double(st, 4) };
        p->rated[k] = (uint32_t)sqlite3_column_int64(st, 5);
        c->ratings++;
    }
    sqlite3_reset(st);
    if (!(st = cb_sql_stmt(x, CC_SQL_PUZZLES_ALL))) return CB_E_DB;
    while (sqlite3_step(st) == SQLITE_ROW) {   /* onto the puzzles loaded from the file; the rest are gone */
        g2_player r = { sqlite3_column_double(st, 1), sqlite3_column_double(st, 2), sqlite3_column_double(st, 3) };
        cc_puzzle_recovered((uint32_t)sqlite3_column_int64(st, 0), &r, (uint32_t)sqlite3_column_int64(st, 4));
    }
    sqlite3_reset(st);
    if (!(st = cb_sql_stmt(x, CC_SQL_GAME_MAX))) return CB_E_DB;
    if (sqlite3_step(st) == SQLITE_ROW) cc_state_next_game_id((uint32_t)sqlite3_column_int64(st, 0));
    sqlite3_reset(st);
    if (!(st = cb_sql_stmt(x, CC_SQL_GAMES_PLAYING))) return CB_E_DB;
    while (sqlite3_step(st) == SQLITE_ROW) {
        cc_live *l = calloc(1, sizeof *l);
        if (!l) { sqlite3_reset(st); return CB_E_NOMEM; }
        l->id = (uint32_t)sqlite3_column_int64(st, 0);
        char w[CB_UUID_LEN + 1], b[CB_UUID_LEN + 1];
        cb_sql_col_uuid(st, 1, w);
        cb_sql_col_uuid(st, 2, b);
        l->p[0] = cc_player_get(w);
        l->p[1] = cc_player_get(b);
        if (!l->p[0] || !l->p[1]) { free(l); continue; }   /* a player row that never committed: the game is lost */
        cc_game_new(&l->g, sqlite3_column_int(st, 3) * 1000, sqlite3_column_int(st, 4) * 1000, now);
        l->rated = (uint8_t)sqlite3_column_int(st, 5);
        l->speed = (uint8_t)cc_speed(sqlite3_column_int(st, 3), sqlite3_column_int(st, 4));
        l->started_at = sqlite3_column_int64(st, 6);
        l->rating0[0] = (int16_t)sqlite3_column_int(st, 7);
        l->rating0[1] = (int16_t)sqlite3_column_int(st, 8);
        l->g.turn_at = now;   /* its moves come from the moves file (replay_moves) */
        cc_live_recovered(l, now);
        c->games++;
    }
    sqlite3_reset(st);
    return CB_OK;
}

/* The moves file onto the games in play (each record at its ply; any other is stale: a game that
 * ended, or a ply already applied). The mover's time is the record's. */
static uint32_t replay_moves(int fd, int64_t now) {
    uint32_t applied = 0;
    mrec buf[4096];
    ssize_t k;
    size_t carry = 0;
    while ((k = read(fd, (uint8_t *)buf + carry, sizeof buf - carry)) > 0) {
        size_t n = (carry + (size_t)k) / sizeof(mrec);
        for (size_t i = 0; i < n; i++) {
            cc_live *l = cc_live_get(buf[i].game);
            if (!l || cc_game_over(&l->g) || buf[i].ply != l->g.ply) continue;
            int mover = l->g.pos.stm;
            if (cc_game_move(&l->g, mover, l->g.ply, (ch_move)buf[i].move, l->g.turn_at) != CC_OK) continue;
            l->g.left_ms[mover] = buf[i].left_ms;
            l->g.turn_at = now;
            applied++;
        }
        carry = (carry + (size_t)k) % sizeof(mrec);
        if (carry) memmove(buf, (uint8_t *)buf + n * sizeof(mrec), carry);
    }
    return applied;
}

void cc_live_each(void (*fn)(cc_live *l, void *ud), void *ud);

typedef struct { int fd; uint8_t buf[1 << 16]; size_t len; } rewrite_ctx;

static void rewrite_one(cc_live *l, void *ud) {
    rewrite_ctx *w = ud;
    if (cc_game_over(&l->g)) return;
    for (int p = 0; p < l->g.ply; p++) {
        /* only the last move of each side carries a time the replay keeps */
        int64_t left = p >= l->g.ply - 2 ? l->g.left_ms[p & 1] : 0;
        mrec r = { l->id, (uint16_t)p, l->g.moves[p], (int32_t)(left > INT32_MAX ? INT32_MAX : left) };
        if (w->len + sizeof r > sizeof w->buf) { write_all(w->fd, w->buf, w->len); w->len = 0; }
        memcpy(w->buf + w->len, &r, sizeof r);
        w->len += sizeof r;
    }
}

int cc_journal_recover(cb_ctx *ctx, char *err, size_t cap) {
    if (!ctx->store || !cb_store_is_sql(ctx->store)) return CB_OK;
    recover_ctx c = { 0 };
    pthread_mutex_lock(&cc_lock);
    int rc = cb_sql_read(ctx->store, recover_fn, &c);
    if (rc != CB_OK) {
        pthread_mutex_unlock(&cc_lock);
        snprintf(err, cap, "c_chess: the journal does not read back: %s", cb_strerror(rc));
        return rc;
    }
    /* the moves file: replayed onto the games just read, then rewritten with only their moves */
    snprintf(J.mpath, sizeof J.mpath, "%s-moves", ctx->cfg ? ctx->cfg->db_path : "c_chess.sqlite");
    uint32_t applied = 0;
    int fd = open(J.mpath, O_RDONLY);
    if (fd >= 0) { applied = replay_moves(fd, cc_mono()); close(fd); }
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.new", J.mpath);
    static rewrite_ctx w;
    w.fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (w.fd < 0) { pthread_mutex_unlock(&cc_lock); snprintf(err, cap, "c_chess: cannot write %s: %s", tmp, strerror(errno)); return CB_E_IO; }
    w.len = 0;
    cc_live_each(rewrite_one, &w);
    if (w.len) write_all(w.fd, w.buf, w.len);
    fsync(w.fd);
    close(w.fd);
    pthread_mutex_unlock(&cc_lock);
    if (rename(tmp, J.mpath) != 0) { snprintf(err, cap, "c_chess: cannot replace %s: %s", J.mpath, strerror(errno)); return CB_E_IO; }
    J.mfd = open(J.mpath, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (J.mfd < 0) { snprintf(err, cap, "c_chess: cannot open %s: %s", J.mpath, strerror(errno)); return CB_E_IO; }
    CB_INFO("cc", "journal: %u players (%u ratings), %u games in play recovered, %u moves replayed", c.players, c.ratings, c.games, applied);
    return CB_OK;
}

/* ---- reads ---- */

typedef struct { uint32_t id; uint8_t *out; size_t cap; int len; char w[CB_UUID_LEN + 1], b[CB_UUID_LEN + 1]; } game_read;

static int game_get_fn(cb_sql *x, void *ud) {
    game_read *g = ud;
    sqlite3_stmt *st = cb_sql_stmt(x, CC_SQL_GAME_GET);
    if (!st) return CB_E_DB;
    sqlite3_bind_int64(st, 1, g->id);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_reset(st); g->len = 0; return CB_OK; }
    cw_snap s;
    memset(&s, 0, sizeof s);
    s.v = CW_V;
    s.kind = CW_SNAP;
    s.game = g->id;
    cb_sql_col_uuid(st, 0, g->w);
    cb_sql_col_uuid(st, 1, g->b);
    s.base_s = (uint16_t)sqlite3_column_int(st, 2);
    s.inc_s = (uint8_t)sqlite3_column_int(st, 3);
    s.rated = (uint8_t)sqlite3_column_int(st, 4);
    s.status = (uint8_t)sqlite3_column_int(st, 5);
    s.winner = (uint8_t)sqlite3_column_int(st, 6);
    s.started_at = sqlite3_column_int64(st, 7);
    s.diff[0] = (int16_t)sqlite3_column_int(st, 11);
    s.diff[1] = (int16_t)sqlite3_column_int(st, 12);
    s.rating[0] = (int16_t)(sqlite3_column_int(st, 9) + s.diff[0]);   /* as a live snapshot: after the game */
    s.rating[1] = (int16_t)(sqlite3_column_int(st, 10) + s.diff[1]);
    const void *mv = sqlite3_column_blob(st, 13);
    int mn = sqlite3_column_bytes(st, 13) / 2;
    if (mn > CC_MAX_PLY) mn = CC_MAX_PLY;
    s.ply = (uint16_t)mn;
    s.left_ms[0] = (uint32_t)sqlite3_column_int64(st, 14);
    s.left_ms[1] = (uint32_t)sqlite3_column_int64(st, 15);
    size_t need = sizeof s + (size_t)mn * 2;
    if (need > g->cap) { sqlite3_reset(st); return CB_E_FULL; }
    memcpy(g->out, &s, sizeof s);
    if (mn) memcpy(g->out + sizeof s, mv, (size_t)mn * 2);
    g->len = (int)need;
    sqlite3_reset(st);
    return CB_OK;
}

int cc_db_game(cb_store *s, uint32_t id, uint8_t *out, size_t cap) {
    game_read g = { .id = id, .out = out, .cap = cap };
    int rc = cb_sql_read(s, game_get_fn, &g);
    if (rc != CB_OK) return rc;
    if (g.len > 0) {
        cw_snap *sn = (cw_snap *)(void *)out;
        pthread_mutex_lock(&cc_lock);
        const cc_player *pw = cc_player_get(g.w), *pb = cc_player_get(g.b);
        if (pw) memcpy(sn->name[0], pw->name, strnlen(pw->name, CW_NAME));
        if (pb) memcpy(sn->name[1], pb->name, strnlen(pb->name, CW_NAME));
        pthread_mutex_unlock(&cc_lock);
    }
    return g.len;
}

typedef struct { const char *id; cw_game_list *out; char opp[CW_LIST_MAX][CB_UUID_LEN + 1]; } list_read;

static int games_of_fn(cb_sql *x, void *ud) {
    list_read *r = ud;
    sqlite3_stmt *st = cb_sql_stmt(x, CC_SQL_GAMES_OF);
    if (!st) return CB_E_DB;
    cb_sql_bind_uuid(st, 1, r->id);
    int n = 0;
    while (n < CW_LIST_MAX && sqlite3_step(st) == SQLITE_ROW) {
        cw_game_row *row = &r->out->rows[n];
        memset(row, 0, sizeof *row);
        row->game = (uint32_t)sqlite3_column_int64(st, 0);
        char w[CB_UUID_LEN + 1], b[CB_UUID_LEN + 1];
        cb_sql_col_uuid(st, 1, w);
        cb_sql_col_uuid(st, 2, b);
        int color = strcmp(w, r->id) == 0 ? 0 : 1;
        row->color = (uint8_t)color;
        memcpy(r->opp[n], color == 0 ? b : w, CB_UUID_LEN + 1);
        row->base_s = (uint16_t)sqlite3_column_int(st, 3);
        row->inc_s = (uint8_t)sqlite3_column_int(st, 4);
        row->rated = (uint8_t)sqlite3_column_int(st, 5);
        row->status = (uint8_t)sqlite3_column_int(st, 6);
        row->winner = (uint8_t)sqlite3_column_int(st, 7);
        row->ended_at = sqlite3_column_int64(st, 8);
        row->diff = (int16_t)sqlite3_column_int(st, color == 0 ? 9 : 10);
        row->ply = (uint16_t)sqlite3_column_int(st, 11);
        n++;
    }
    sqlite3_reset(st);
    r->out->n = (uint16_t)n;
    return CB_OK;
}

int cc_db_games_of(cb_store *s, const cc_player *p, cw_game_list *out) {
    static __thread list_read r;
    memset(out, 0, 8);
    r.id = p->id;
    r.out = out;
    int rc = cb_sql_read(s, games_of_fn, &r);
    if (rc != CB_OK) return rc;
    pthread_mutex_lock(&cc_lock);
    for (int i = 0; i < out->n; i++) {
        const cc_player *o = cc_player_get(r.opp[i]);
        if (o) memcpy(out->rows[i].opponent, o->name, strnlen(o->name, CW_NAME));
    }
    pthread_mutex_unlock(&cc_lock);
    return CB_OK;
}

typedef struct { const char *id; cw_point *out; int per_speed, n; } points_read;

static int points_fn(cb_sql *x, void *ud) {
    points_read *r = ud;
    sqlite3_stmt *st = cb_sql_stmt(x, CC_SQL_POINTS_OF);
    if (!st) return CB_E_DB;
    cb_sql_bind_uuid(st, 1, r->id);
    sqlite3_bind_int(st, 2, r->per_speed);
    while (r->n < r->per_speed * CC_SPEEDS && sqlite3_step(st) == SQLITE_ROW) {
        cw_point *pt = &r->out[r->n++];
        memset(pt, 0, sizeof *pt);
        pt->speed = (uint8_t)sqlite3_column_int(st, 0);
        pt->at = sqlite3_column_int64(st, 1);
        pt->rating = (int16_t)sqlite3_column_int(st, 2);
    }
    sqlite3_reset(st);
    return CB_OK;
}

int cc_db_points_of(cb_store *s, const cc_player *p, cw_point *out, int per_speed) {
    points_read r = { p->id, out, per_speed, 0 };
    int rc = cb_sql_read(s, points_fn, &r);
    return rc == CB_OK ? r.n : rc;
}
