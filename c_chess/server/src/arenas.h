/* arenas.h - the server's arenas (the rules are the kernel's, c/src/arena.h): a fixed table of them
 * in memory under cc_lock, run by the clock thread once a second (cc_arena_tick): an upcoming arena
 * starts at its time, a running one pairs its free players and ends at its time, and the standings
 * of each are pushed to its room ("a<id>") when they changed. A game an arena made carries its id
 * (cc_live.arena) and reports its result here when it ends (cc_arena_game_over, from the finish).
 *
 * PAIRING, once a second, lichess's shape: the free players of a running arena (joined, not
 * withdrawn, in no game of any kind, rested CC_ARENA_REST_MS since their last arena game) sorted by
 * the rating of the arena's speed and paired with the nearest below them, skipping their last
 * arena opponent when more than two are free (two left who just met trade partners with the
 * nearest pair above them instead of waiting); colors go to whoever has had white less often. When
 * an odd number are free, the one who has played the most waits (not always the lowest rated).
 *
 * THE SCHEDULE: the site keeps one upcoming arena per speed on the hour (Hourly Bullet 1+0 for 27
 * minutes, Hourly Blitz 3+0 and Hourly Rapid 10+0 for 57), unless CC_ARENA_SCHEDULE=0.
 *
 * Everything is journaled (journal.c: cc_arenas, cc_arena_players) through the journal's queue; a
 * restart brings back the upcoming, running and recently finished arenas with their players. */
#ifndef C_CHESS_ARENAS_H
#define C_CHESS_ARENAS_H

#include "cc_server.h"

#define CC_ARENA_TICK_MS 1000
#define CC_ARENAS_MAX 64                     /* upcoming or running at once */
#define CC_ARENA_SLOTS CW_ARENAS_MAX         /* with the finished ones kept */
#define CC_ARENA_KEEP_MS (60 * 60 * 1000)    /* a finished arena stays in memory this long */
#define CC_ARENA_PER_CREATOR 4               /* upcoming arenas a player may have made */
#define CC_ARENA_PLAYERS_MAX 10000
#define CC_ARENA_REST_MS 3000                /* after an arena game, before the next pairing */

typedef struct {
    cc_player *p;
    cc_player *last;                         /* the last arena opponent */
    uint32_t   game;                         /* the arena game in play; 0: none */
    uint32_t   order;                        /* when it joined (a tie's last resort) */
    int64_t    rest_until;                   /* mono ms */
    uint16_t   score, games, white, black, rank;
    uint8_t    streak, withdrawn;
} cc_arena_player;

typedef struct cc_arena {
    uint32_t   id;
    uint8_t    status, speed;                /* arena.h CC_ARENA_UPCOMING..; speed.h */
    uint8_t    dirty;                        /* the standings changed since the last push */
    uint8_t    unsorted;                     /* ... since the last ranking */
    cw_seek    tc;
    char       name[CW_ARENA_NAME + 1];
    char       creator[CB_UUID_LEN + 1];     /* "": the site's schedule */
    int64_t    starts_at, ends_at;           /* wall ms */
    cc_arena_player *pl;
    uint32_t   n, cap, joins;
    uint32_t  *rank;                         /* indices into pl by rank (cc_arena_rank) */
    uint32_t   games;
    uint16_t   playing;
    uint8_t    final_written;                /* the end's rows were journaled */
    uint8_t   *sent;                         /* the last push, its `now` zeroed */
    uint32_t   sent_len;
} cc_arena;

/* ---- under cc_lock ---- */
cc_arena *cc_arena_get(uint32_t id);
/* A new arena (by: its maker; NULL the site's schedule): CW_E_NONE with *out, or a CW_E_ code. */
int  cc_arena_create(cc_player *by, const cw_arena_req *q, int64_t wall, cc_arena **out);
int  cc_arena_join(cc_arena *a, cc_player *p);
int  cc_arena_withdraw(cc_arena *a, cc_player *p);
/* The clock thread's pass: the schedule, starts and ends, the pairing, the standings pushes. */
void cc_arena_tick(int64_t now_mono, int64_t wall);
/* A game the arena made has ended (state.c's finish, after the ratings). */
void cc_arena_game_over(const cc_live *l, int64_t now_mono);
/* The arena's frame (cw_arena_hdr + its top rows) with `me`'s own place (NULL: none) into out (at
 * least CW_ARENA_FRAME_MAX): its length. */
size_t cc_arena_fill(cc_arena *a, const cc_player *me, int64_t wall, uint8_t *out);
/* The arenas in memory, running first, then upcoming by start, then finished: the frame's length. */
size_t cc_arena_list_fill(cw_arena_list *out, int64_t wall);
/* Ranks the players by score, then rating, then who joined first (a->rank), when they changed. */
void cc_arena_rank(cc_arena *a);
void cc_arena_schedule(bool on);
void cc_arena_free(void);
/* For /c/stats: pairing passes, the time they took (us, all and the longest), pushes and their bytes. */
void cc_arena_counts(uint64_t *ticks, uint64_t *tick_us, uint64_t *tick_max_us, uint64_t *pushes, uint64_t *push_bytes);

/* ---- recovery (journal.c, under cc_lock) ---- */
cc_arena *cc_arena_recovered(const cc_arena *in);   /* copies the arena; its players come next */
void cc_arena_player_recovered(cc_arena *a, const cc_arena_player *in);
/* After the games in play are back: each arena game's players are in it again. */
void cc_arena_games_recovered(void);
/* Ids go on after the journal's largest (an arena long gone from memory keeps its id). */
void cc_arena_next_id(uint32_t max);

#endif
