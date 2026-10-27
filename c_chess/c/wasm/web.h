/* web.h - what the page reads from the browser's kernel (web.c): the structs below, through readers
 * shared/tools/structgen writes from this header (web/src/gen/), checked by the layout's hash at
 * load. The page writes nothing but raw socket and HTTP bytes into the io buffer. */
#ifndef C_CHESS_WEB_H_
#define C_CHESS_WEB_H_

#include <stdint.h>

/* the constants the page reads too (structgen --const): events, actions, statuses, errors */
#include "client.h"

#define CV_NAME 24
#define CV_SEEKS 64
#define CV_PLY 600

enum { CV_NO_SQ = 64 };

/* Everything the play and watch pages draw. Pieces: 0 empty, 1..6 white P N B R Q K, 9..14 black. */
typedef struct {
    uint32_t events;          /* client.h CC_EV_*, since the last view */
    uint32_t game;
    int8_t   color;           /* ours, -1 watching */
    uint8_t  synced, ready, status;
    uint8_t  winner, stm, my_turn, clock_running;
    uint8_t  last_from, last_to, check_sq, draw_offer;
    uint8_t  rematch_offer, rated, inc_s, premove_from;   /* premove_from/to: CV_NO_SQ when none */
    uint8_t  premove_to, pad2[3];
    uint16_t ply, base_s;
    uint32_t clock_ms[2];
    int16_t  rating[2], diff[2];
    uint32_t next_game, start_game;
    uint8_t  start_color, auth_code, err_code;
    int8_t   err_rc;
    int32_t  auth_status;
    uint16_t err_status;
    uint16_t pad1;
    uint32_t challenge_id;
    uint32_t arena;           /* the arena that made the game shown; 0: none */
    uint32_t read_tag;        /* the last read's (CC_EV_READ) */
    uint16_t read_status;
    uint16_t pad3;
    uint8_t  board[64];
    char     white[CV_NAME];
    char     black[CV_NAME];
    char     me[CV_NAME];
} CvView;

typedef struct { char san[8]; } CvSan;
typedef struct { uint16_t n; uint16_t pad; CvSan moves[CV_PLY]; } CvMoves;

typedef struct {
    uint32_t id;
    uint16_t base_s;
    uint8_t  inc_s, rated;
    int16_t  rating;
    uint16_t pad;
    char     name[CV_NAME];
} CvSeek;
typedef struct { uint16_t n; uint16_t pad; uint32_t games, players; CvSeek seeks[CV_SEEKS]; } CvLobby;

/* A profile (GET /c/user/<name>), parsed: a rating per speed (speed.h CC_BULLET..CC_CLASSICAL, in
 * that order), the puzzle rating, and each speed's history: points[from[k] .. from[k] + count[k]),
 * oldest first. provisional: the deviation is at or above rating.h's CC_PROVISIONAL_RD (a "?"). */
#define CV_POINTS 600
typedef struct { double at; int16_t rating; uint8_t speed; uint8_t pad[5]; } CvPoint;
typedef struct {
    uint8_t  ok; uint8_t puzzle_provisional; uint8_t pad[2];
    int16_t  rating[CC_SPEEDS];
    uint16_t rd[CC_SPEEDS];
    uint32_t rated[CC_SPEEDS];          /* rated games of each speed */
    uint8_t  provisional[CC_SPEEDS];
    uint16_t from[CC_SPEEDS], count[CC_SPEEDS];
    uint32_t games, wins, losses, draws;
    int16_t  puzzle; uint16_t puzzle_rd;
    uint32_t puzzles_tried, puzzles_solved;
    uint16_t n; uint16_t pad2[5];        /* the points start 8-aligned */
    char     name[CV_NAME];
    CvPoint  points[CV_POINTS];
} CvProfile;

/* A user's recent games (GET /c/games/<name>), parsed. */
#define CV_ROWS 50
typedef struct {
    uint32_t game;
    uint8_t  status, winner, color, rated;
    uint16_t base_s, ply;
    uint8_t  inc_s, pad;
    int16_t  diff;
    double   ended_at;
    char     opponent[CV_NAME];
} CvGameRow;
typedef struct { uint16_t n; uint16_t pad[3]; CvGameRow rows[CV_ROWS]; } CvGames;

/* The lobby's live games, unpacked; clocks as of the view. */
#define CV_TV 24
typedef struct {
    uint32_t game;
    uint16_t ply, base_s;
    uint8_t  inc_s, status, stm, clock_running;
    uint8_t  last_from, last_to;
    int16_t  rating[2];
    uint8_t  pad[2];
    uint32_t clock_ms[2];
    char     white[CV_NAME];
    char     black[CV_NAME];
    uint8_t  board[64];
} CvTvGame;
typedef struct { uint16_t n; uint16_t pad; CvTvGame games[CV_TV]; } CvTv;

#define CV_TOP 50
typedef struct { char name[CV_NAME]; int16_t rating; uint16_t rd; uint32_t games; uint8_t provisional; uint8_t pad[3]; } CvTopRow;
typedef struct { uint16_t n; uint8_t speed; uint8_t pad; CvTopRow rows[CV_TOP]; } CvTop;   /* games: rated, of the speed */

/* The engine's view of a position of the game shown (white's view), its line in SAN. */
typedef struct {
    uint16_t ply;
    int16_t  score;            /* centipawns, + good for white */
    int8_t   mate;             /* moves, + white mates, 0 none */
    uint8_t  depth, best_from, best_to;   /* best_*: CV_NO_SQ when none */
    char     line[96];         /* "Nf3 Nc6 Bb5 a6", as far as it fits */
} CvEval;

/* A puzzle being solved (the kernel's cp_solve), and where the solver stands. */
typedef struct {
    uint32_t id;
    uint8_t  ok, state, color, themes;   /* ok: one is loaded; state: CP_SOLVING, CP_SOLVED, CP_FAILED */
    uint8_t  last_from, last_to, check_sq, hint_from;   /* hint_from: the next move's piece, CV_NO_SQ at the end */
    uint8_t  at, n, done, reply_pending; /* done: the result was answered (before and after hold it) */
    uint8_t  intro, wrong_sq, pad[2];    /* intro: the position before the opponent's move is drawn
                                          * (cv_puzzle_begin plays it); wrong_sq: where a wrong move went */
    int16_t  rating, puzzle_rating;      /* the solver's, the puzzle's */
    uint16_t rd, puzzle_rd;
    uint32_t tried, solved, plays;
    int16_t  before, after;
    uint8_t  board[64];
} CvPuzzle;

/* An arena (a push of its room, or the answer of a read, a join or a withdrawal) and its standings,
 * the top CV_ARENA_ROWS by score then rating. The countdowns are by the server's clock as of the
 * frame, run on by the page's clock since: starts_in_ms and ends_in_ms (negative once past). me_*:
 * the reader's own place (me_in CW_ARENA_OUT, _IN, _WITHDRAWN), known even below the rows shown. */
#define CV_ARENA_ROWS 100
#define CV_ARENA_NAME 32
typedef struct {
    char     name[CV_NAME];
    uint16_t score, games, rank;
    int16_t  rating;
    uint8_t  fire, withdrawn, playing, me;   /* fire: a win scores double; me: the reader's row */
} CvArenaRow;
typedef struct {
    uint8_t  ok, status, speed, rated;      /* status: arena.h CC_ARENA_UPCOMING.. */
    uint32_t id;
    uint16_t base_s;
    uint8_t  inc_s, me_in;
    uint16_t n_players, n, playing, me_rank;
    uint16_t me_score, me_games;
    uint8_t  me_fire, me_listed;             /* me_listed: the reader's row is among the rows */
    uint8_t  err;                            /* a refused answer's cw_err code (ok 0): CW_E_ARENA_.. */
    uint8_t  pad;
    uint32_t games;
    int32_t  starts_in_ms, ends_in_ms;
    uint32_t pad2;
    double   starts_at, ends_at;             /* ms since the epoch */
    char     name[CV_ARENA_NAME];
    char     by[CV_NAME];                    /* who made it; empty: the site */
    CvArenaRow rows[CV_ARENA_ROWS];
} CvArena;

/* GET /c/arenas: running first, then upcoming by start, then finished, latest first. */
#define CV_ARENAS 128
typedef struct {
    uint32_t id;
    uint8_t  status, speed, inc_s, rated;
    uint16_t base_s, n_players;
    int32_t  starts_in_ms, ends_in_ms;       /* by the server's clock when it answered */
    uint32_t pad;
    double   starts_at, ends_at;
    char     name[CV_ARENA_NAME];
    char     winner[CV_NAME];                /* a finished arena's first place */
} CvArenaItem;
typedef struct { uint16_t n; uint16_t pad; uint32_t pad2; CvArenaItem rows[CV_ARENAS]; } CvArenaList;

/* A guest's account, made at the first visit and kept by the page as these bytes (base64 in
 * localStorage): its name and password, NUL-padded. The page never reads inside it. */
#define CV_GUEST_PW 32
typedef struct { char name[CV_NAME]; char password[CV_GUEST_PW]; } CvGuest;

#endif
