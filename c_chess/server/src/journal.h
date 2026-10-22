/* journal.h - SQLite behind the in-memory state: a writer thread takes the records the state hands
 * it (under cc_lock, never waiting) and commits every record queued since its last commit in one
 * transaction (c_base's group commit, store_sql.h). A crash loses at most the records of the commit
 * in flight. At start the players and the games in play are read back (cc_journal_recover).
 *
 * The reads of what is no longer in memory (a finished game, a profile's history) block on the
 * store, so they run on the request pool only. */
#ifndef C_CHESS_JOURNAL_H
#define C_CHESS_JOURNAL_H

#include "cc_server.h"
#include "store/store.h"

enum {
    CC_SQL_PLAYER_PUT,
    CC_SQL_PLAYERS_ALL,
    CC_SQL_GAME_START,
    CC_SQL_MOVE_PUT,
    CC_SQL_GAME_END,
    CC_SQL_MOVES_DEL,
    CC_SQL_POINT_PUT,
    CC_SQL_GAMES_PLAYING,
    CC_SQL_MOVES_OF,
    CC_SQL_GAME_MAX,
    CC_SQL_GAME_GET,
    CC_SQL_GAMES_OF,
    CC_SQL_POINTS_OF,
    CC_SQL_N
};
extern const char *const cc_sql_stmts[CC_SQL_N];

int  cc_journal_start(cb_ctx *ctx);
/* Commits what is queued and stops the writer. */
void cc_journal_stop(void);
/* The records (under cc_lock; they copy what they need). */
void cc_journal_player(const cc_player *p);
void cc_journal_game_start(const cc_live *l);
void cc_journal_move(const cc_live *l);            /* the last ply of l */
void cc_journal_game_end(const cc_live *l, int64_t wall);
void cc_journal_point(const cc_player *p, int64_t wall);
/* Records queued and committed since start, for /c/stats and the tests. */
void cc_journal_counts(uint64_t *queued, uint64_t *committed, uint64_t *commits);
/* Blocks until everything queued before the call is committed (the tests, a clean stop). */
void cc_journal_sync(void);

/* At serve_open, before anything is served: the players and the games in play into the state. */
int  cc_journal_recover(cb_ctx *ctx, char *err, size_t cap);

/* ---- reads (request pool only) ---- */
/* A game from the store as a snapshot with its moves after it (out holds cap bytes): its length, 0
 * when there is no such game, or a CB_E_*. */
int  cc_db_game(cb_store *s, uint32_t id, uint8_t *out, size_t cap);
int  cc_db_games_of(cb_store *s, const cc_player *p, cw_game_list *out);
int  cc_db_points_of(cb_store *s, const cc_player *p, cw_point *out, int max);

#endif
