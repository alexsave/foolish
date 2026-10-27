/* cc_server.h - the c_chess server's state: every live game, every player who has played, the open
 * seeks and challenges, in memory and authoritative; SQLite is the journal behind them (journal.h).
 *
 * ONE LOCK. Every handler runs on the loop thread when it can (CB_ROUTE_INLINE|HOME) and on a pool
 * thread when the loop's gate is shut, and the clock thread ends games on the flag, so the state is
 * behind cc_lock. A hold is short and never waits on I/O: pushes are handed to c_base's realtime
 * (which copies them when called off the loop) and journal records to the journal's queue. */
#ifndef C_CHESS_SERVER_H
#define C_CHESS_SERVER_H

#include "ctx.h"
#include "game.h"
#include "rating.h"
#include "wire.h"
#include "util/uuid.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct cc_player {
    char      id[CB_UUID_LEN + 1];
    char      name[CW_NAME + 1];
    g2_player r[CC_SPEEDS];            /* by speed (rating.h) */
    uint32_t  rated[CC_SPEEDS];        /* rated games of each speed */
    uint32_t  games, wins, losses, draws;
    int64_t   last_at;                 /* the last rated game's end, ms since the epoch */
    g2_player pz;                      /* the puzzle rating (puzzles.h) */
    uint32_t  pz_tried, pz_solved;
    uint32_t  pz_open;                 /* the puzzle handed out and not yet answered; 0: none */
    uint32_t  pz_seen[16];             /* the last puzzles handed out, not handed out again soon */
    uint8_t   pz_seen_at;
    uint16_t  playing;                 /* games of theirs in play (an arena pairs only the free) */
    int64_t   mute_until;              /* ms since the epoch: no chat before this (chats.h); journaled */
    int64_t   chat_at;                 /* mono ms: the last line said */
    int64_t   strike_at[3];            /* mono ms: the last banned words, oldest first (chats.h) */
    struct cc_player *next;            /* the id map's chain */
    struct cc_player *next_name;       /* the name map's chain */
} cc_player;

typedef struct cc_live {
    uint32_t   id;
    cc_game    g;
    cc_player *p[2];                   /* white, black */
    uint8_t    rated;
    uint8_t    rematch_offer;
    uint8_t    finished;               /* cc_finish ran (ratings, journal) */
    uint8_t    ai_pending;             /* the computer's search for this ply is in flight (ai.h) */
    uint8_t    speed;                  /* its rating's (rating.h), from the time control */
    uint32_t   arena;                  /* the arena that made it (arena.h); 0: none */
    int16_t    rating0[2];             /* the ratings when the game began */
    int16_t    diff[2];
    uint32_t   next_game;              /* the rematch */
    uint32_t   heap_idx;               /* 1 + its place in the clock thread's heap; 0: not there */
    int64_t    timer_at;               /* mono ms: when its clock could next end it */
    int64_t    started_at;             /* ms since the epoch */
    int64_t    evict_at;               /* mono ms: a finished game leaves memory after this */
    struct cc_live *next;              /* the id map's chain */
} cc_live;

typedef struct {
    uint32_t   id;
    cc_player *who;
    cw_seek    tc;
    int64_t    expires;                /* mono ms */
} cc_seek;

typedef struct cc_challenge {
    uint32_t   id;
    cc_player *who;
    cw_seek    tc;
    int64_t    expires;                /* mono ms */
    struct cc_challenge *next;
} cc_challenge;

#define CC_SEEK_TTL_MS 30000           /* a seek lives this long unless sent again */
#define CC_CHALLENGE_TTL_MS (30 * 60 * 1000)
#define CC_FINISHED_KEEP_MS (60 * 1000)   /* then a finished game is read from the journal */
#define CC_SEEKS_MAX 4096

extern pthread_mutex_t cc_lock;

/* The state; cc_state_init before anything. */
int  cc_state_init(cb_ctx *ctx);
void cc_state_free(void);

/* ---- under cc_lock ---- */
cc_player *cc_player_get(const char *id);
cc_player *cc_player_by_name(const char *name);
/* The player of a signed-in user, made (and journaled) on first sight. */
cc_player *cc_player_of(const char *id, const char *name);
cc_live   *cc_live_get(uint32_t id);
/* A new game between a and b (a white), its clock thread entry and journal record, and the START
 * pushes to both. */
cc_live   *cc_live_start(cc_player *white, cc_player *black, const cw_seek *tc, int64_t now_mono);
/* The same, for a game an arena made (its id rides in the game, the journal and the snapshot). */
cc_live   *cc_live_start_in(cc_player *white, cc_player *black, const cw_seek *tc, uint32_t arena, int64_t now_mono);
/* After any change to a live game: the state push to its room, its clock entry, and when it has
 * just ended, the finish (ratings, the journal). */
void       cc_live_changed(cc_live *l, int64_t now_mono);
void       cc_state_fill(const cc_live *l, cw_state *s, int64_t now_mono);
int        cc_color_of(const cc_live *l, const cc_player *p);   /* -1: not a player */
/* A time control the site plays: up to 3 hours plus 3 minutes a move, at least 15 s of play. */
bool       cc_tc_ok(const cw_seek *tc);
/* Seeks: pairs with a waiting seek of the same time control from someone else (*game set, the
 * seek gone) or waits; the lobby push follows. */
int        cc_seek_add(cc_player *p, const cw_seek *tc, int64_t now_mono, cc_live **game);
void       cc_seek_cancel(cc_player *p, int64_t now_mono);
int        cc_lobby_fill(cw_lobby *l, int64_t now_mono);   /* the frame's length */
cc_challenge *cc_challenge_add(cc_player *p, const cw_seek *tc, int64_t now_mono);
cc_challenge *cc_challenge_get(uint32_t id, int64_t now_mono);
void       cc_challenge_drop(uint32_t id);

/* The games in play with the highest ratings (at most max): the frame's length. */
int        cc_tv_fill(cw_tv *tv, int max, int64_t now_mono);
/* The leaderboard of a speed (0..CC_SPEEDS-1): players with a rated game of it, by its rating: the
 * frame's length. */
int        cc_top_fill(cw_top *top, int speed);

/* The clock thread's sweep, every 10 s: finished games past their keep leave memory, and their chat
 * with them; expired challenges and seeks go. */
void       cc_state_sweep(int64_t now_mono);

/* Counters for the lobby frame and /c/stats. */
uint32_t   cc_games_live(void);

/* The process's monotonic and wall clocks (util/clock.h), one place to stub. */
int64_t    cc_mono(void);
int64_t    cc_wall(void);

#endif
