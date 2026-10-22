// league.h - THE BOT LEAGUE: every one of the game server's bots playing the others, at the server's
// own pace, at tables of two to eight, classic and podkidnoy, with nobody human seated; and the
// ratings the server gives them as each game is rated, kept as a history the site draws.
//
// HOW A TABLE COMES TO BE. No client request can deal a table of bots alone (the kernel deals only
// for a seated asker), so the league asks the game server's dev-only POST /dev/bot_table
// (foolish/server/src/app/dev_bot_table.h) with the admin token: the server seats the bots by its
// own lobby calls and deals, and its bot loop plays the game exactly as it plays a bot's turn at a
// human's table (CBASE_BOT_SPEEDUP=1: the production pace), then rates it through the commit.
//
// HOW THE LEAGUE SEES IT. One watcher account (client.h) hears every league table's room as a
// spectator (NW_WS_SPECT casts, applied to a client table per table), so the grid shows the bot
// games live; when a game is over it reads the game's envelope (GET /v1/games/<id>), whose rating
// stamp (view.h GameRatings: each seat's rating before and the change) is the result. The
// leaderboard (GET /v1/leaderboard) is read at start and every minute, and is the standing of a
// bot the league has not seen play. Nothing is computed here but counts.
//
// THE HISTORY survives a restart: every result is appended to a file (BE_LEAGUE_FILE) in its fixed
// layout (wire.h be_w_result), behind a head naming the bots in the order the indices mean.
#ifndef BE_LEAGUE_H
#define BE_LEAGUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char     server[128];      // host:port of the game server
    char     admin_token[128]; // the game server's CBASE_ADMIN_TOKEN (POST /dev/bot_table)
    char     file[512];        // the history file; "" keeps none
    int      tables;           // live league tables at once
    int      tables_per_bot;   // tables one bot may sit at at once (a server bot is an account, not a person)
    int      podkidnoy;        // percent of tables under podkidnoy rules
    uint64_t seed;
    bool     tournament;       // play the stored tournament (or draw one) instead of league tables
    char     tour_file[512];   // the tournament's file; "" keeps none
} be_league_opts;

// The mode birdseye runs in (wire.h BE_MODE_*), set by birdseye_app.c at open.
extern uint8_t be_mode;

int  be_league_start(const be_league_opts *o, char *err, size_t cap);
void be_league_stop(void);
// The league's gauges (be_metrics.h): bots seated.
void be_league_gauges(void);

// GET /be/league: be_w_league, the bots, the results (wire.h), in a buffer the caller frees.
// The bytes, or 0 when out of memory.
size_t be_league_snapshot(void **out, uint8_t mode);
// GET /be/tournament: the stored tournament (wire.h be_w_tour) in a buffer the caller frees.
size_t be_tour_snapshot(void **out);

// The seat count of a new table: 2 to 8, weighted toward two-player games, given how many bots
// are free (never more seats than that) and a random draw r in [0, 1000). 0 when fewer than two are free.
int be_league_pick_size(int free_bots, uint32_t r);

#endif
