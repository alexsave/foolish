/* wire.h - c_chess's wire: the bodies of the requests a client sends (as SF_REQ over the /ws socket,
 * shared/c/stream_frame.h, or as plain HTTP), the answers, and the pushes. Fixed layouts,
 * little-endian, naturally aligned with the padding spelled out, so a frame is a struct written and
 * read with one memcpy on every target this runs on (arm64, x86-64, wasm32); the size of each is
 * pinned below. Both ends are this C: the server, and the client (client.h) in the load generator
 * and, as wasm, in the browser. TypeScript never reads these bytes.
 *
 * A push starts [CW_V][kind] like every frame of the socket (SF_V == CW_V), with kinds below
 * SF_HELLO so they read as pushes (sf_is_push).
 *
 *   requests (path: body -> answer body)
 *     POST /c/auth       cw_auth_req           -> cw_session + the access token (HTTP only)
 *     POST /c/refresh    cw_refresh_req        -> cw_session + the access token (HTTP only)
 *     POST /c/seek       cw_seek               -> cw_seek_ans (a game at once, or the seek waits)
 *     POST /c/unseek     (none)                -> (none)
 *     POST /c/challenge  cw_seek               -> cw_id (the challenge, for the link /c/<id>)
 *     POST /c/accept     cw_id (challenge)     -> cw_id (the game)
 *     POST /c/ai         cw_ai_req             -> cw_id (a casual game against the computer)
 *     POST /c/eval       cw_eval_req + moves   -> (none): the answer is pushed (CW_P_EVAL)
 *     POST /c/move       cw_move_req           -> (none): 200, or 409 with a cw_err
 *     POST /c/act        cw_act_req            -> (none)
 *     GET  /c/game/<id>  -                     -> cw_snap + its moves
 *     GET  /c/lobby      -                     -> cw_lobby
 *     GET  /c/user/<name>                      -> cw_profile + its rating points (the latest
 *                                                 CW_POINTS_SPEED of each speed)
 *     GET  /c/games/<name>                     -> cw_game_list
 *     GET  /c/pgn/<id>   (HTTP)                -> text/plain PGN
 *   pushes
 *     CW_P_STATE  cw_state  to the game's room (players and spectators): every change of a game
 *     CW_P_START  cw_start  to a user: a game of yours began (seek paired, challenge accepted, rematch)
 *     CW_P_LOBBY  cw_lobby  to the lobby room: the open seeks
 *     CW_P_EVAL   cw_eval   to a user: the engine's view of a position it asked about (POST /c/eval)
 *     CW_P_TV     cw_tv     to the lobby room, at most once a second when it changed: the games in play
 *                           with the highest ratings, each with its board
 *     GET /c/tv           -                    -> cw_tv
 *     GET /c/top/<speed>  -                    -> cw_top: the leaderboard of a speed (speed.h CC_BULLET..)
 *     GET /c/puzzle/next  -                    -> cw_puzzle: one near the asker's puzzle rating
 *     POST /c/puzzle/result cw_puzzle_req      -> cw_puzzle_done: both ratings after it */
#ifndef C_CHESS_WIRE_H
#define C_CHESS_WIRE_H

#include "puzzle.h"
#include "speed.h"

#include <stdint.h>

#define CW_V 1
#define CW_NAME 20          /* a username's bytes, NUL-padded (names are 3..20 of [A-Za-z0-9_-]) */
#define CW_LOBBY_MAX 64     /* seeks a lobby frame lists */
#define CW_POINTS_SPEED 150 /* rating points a profile lists, the latest of each speed */
#define CW_POINTS_MAX (CC_SPEEDS * CW_POINTS_SPEED)
#define CW_LIST_MAX 50      /* games a game list holds */

enum { CW_P_STATE = 1, CW_P_START = 2, CW_P_LOBBY = 3, CW_SNAP = 4, CW_P_TV = 5, CW_P_EVAL = 6 };
#define CW_PV_MAX 12
#define CW_TV_MAX 24
#define CW_TOP_MAX 50

/* cw_act_req.act */
enum { CW_A_RESIGN = 1, CW_A_ABORT = 2, CW_A_DRAW = 3, CW_A_DRAW_DECLINE = 4, CW_A_REMATCH = 5, CW_A_REMATCH_DECLINE = 6 };

/* cw_err.code: why a request was refused (the HTTP status says what kind) */
enum { CW_E_NONE = 0, CW_E_NO_GAME = 1, CW_E_NOT_PLAYER = 2, CW_E_RULES = 3, CW_E_BAD = 4, CW_E_SELF = 5, CW_E_BUSY = 6 };

#define CW_PASSWORD 72      /* bcrypt reads no more */
#define CW_REFRESH 64
#define CW_UUID 36
enum { CW_AUTH_SIGNUP = 1, CW_AUTH_SIGNIN = 2 };
/* cw_err.code of a refused sign-up or sign-in */
enum { CW_E_NAME_TAKEN = 20, CW_E_BAD_LOGIN = 21, CW_E_WEAK_PASSWORD = 22, CW_E_BAD_NAME = 23, CW_E_TOO_MANY = 24, CW_E_EXPIRED = 25 };

typedef struct { char name[CW_NAME]; char password[CW_PASSWORD]; uint8_t mode; uint8_t pad[3]; } cw_auth_req;
typedef struct { char refresh[CW_REFRESH]; } cw_refresh_req;
/* then access_len bytes of access token */
typedef struct {
    uint32_t expires_in;       /* seconds the access token lives */
    uint16_t access_len;
    uint16_t pad;
    char     user_id[CW_UUID];
    char     name[CW_NAME];
    char     refresh[CW_REFRESH];
} cw_session;

typedef struct { uint16_t base_s; uint8_t inc_s; uint8_t rated; } cw_seek;
typedef struct { uint32_t game; uint8_t color; uint8_t pad[3]; } cw_seek_ans;   /* game 0: waiting */
typedef struct { uint32_t id; } cw_id;
/* a position as the game that reached it: this, then n u16 moves from the start */
typedef struct { uint16_t n; uint8_t pad[2]; } cw_eval_req;
/* The engine's view of a position: white's view (score in centipawns, + good for white; mate in
 * moves, + white mates, 0 none), the depth it reached, and its best line. */
typedef struct {
    uint8_t  v, kind;          /* CW_V, CW_P_EVAL */
    uint16_t ply;              /* the position: the moves asked about */
    uint8_t  depth;
    int8_t   mate;
    int16_t  score;
    uint64_t key;              /* the position's Zobrist key */
    uint8_t  npv;
    uint8_t  pad[7];
    uint16_t pv[CW_PV_MAX];
} cw_eval;
/* color: 0 white, 1 black, 2 either (by lot); level 1..8 */
typedef struct { uint16_t base_s; uint8_t inc_s; uint8_t level; uint8_t color; uint8_t pad[3]; } cw_ai_req;
/* think_ms: the mover's own time on the move, by its clock (lag compensation, game.h); 0xffffffff unknown */
typedef struct { uint32_t game; uint16_t ply; uint16_t move; uint32_t think_ms; } cw_move_req;
typedef struct { uint32_t game; uint8_t act; uint8_t pad[3]; } cw_act_req;
typedef struct { uint8_t code; int8_t rc; uint8_t pad[2]; } cw_err;   /* rc: game.h's cc_move_rc */

/* Everything about a game that changes, after each change. The receiver that holds plies
 * 0..ply-2 applies `move` as ply - 1; one that is further behind asks for the snapshot. */
typedef struct {
    uint8_t  v, kind;          /* CW_V, CW_P_STATE */
    uint8_t  status, winner;   /* game.h */
    uint32_t game;
    uint16_t ply;              /* plies played */
    uint16_t move;             /* the last ply's move (0 before any) */
    uint32_t left_ms[2];       /* each side's time as of this frame */
    uint8_t  draw_offer;       /* bit 1 << color */
    uint8_t  rematch_offer;    /* bit 1 << color */
    int16_t  diff[2];          /* rating change, once a rated game is over */
    int16_t  rating[2];        /* ratings (after the game, once over) */
    uint8_t  clock_running;
    uint8_t  pad;
    uint32_t next_game;        /* the rematch, once both agreed */
} cw_state;

typedef struct {
    uint8_t  v, kind;          /* CW_V, CW_P_START */
    uint8_t  color;            /* yours */
    uint8_t  pad;
    uint32_t game;
} cw_start;

typedef struct {
    uint32_t id;
    uint16_t base_s;
    uint8_t  inc_s, rated;
    int16_t  rating;
    uint8_t  pad[2];
    char     name[CW_NAME];
} cw_seek_row;

typedef struct {
    uint8_t     v, kind;       /* CW_V, CW_P_LOBBY */
    uint16_t    n;
    uint32_t    games;         /* games in play */
    uint32_t    players;       /* sockets */
    cw_seek_row seeks[CW_LOBBY_MAX];   /* only n of them are sent */
} cw_lobby;

/* A game whole: this, then `ply` u16 moves. */
typedef struct {
    uint8_t  v, kind;          /* CW_V, CW_SNAP */
    uint8_t  status, winner;
    uint32_t game;
    uint16_t ply;
    uint16_t base_s;
    uint8_t  inc_s, rated, draw_offer, rematch_offer;
    uint32_t left_ms[2];
    uint32_t turn_ms;          /* how long the side to move has been thinking (its time runs from left_ms) */
    int16_t  rating[2];
    int16_t  diff[2];
    uint32_t next_game;
    uint8_t  clock_running;
    uint8_t  pad[7];
    int64_t  started_at;       /* ms since the epoch */
    char     name[2][CW_NAME];
} cw_snap;

/* A game in play as the lobby shows it: the board as 32 bytes, two squares a byte (low nibble the
 * even square), each square 0 empty or 1 + piece + 8 * color as the kernel numbers them. */
typedef struct {
    uint32_t game;
    uint16_t ply, base_s;
    uint8_t  inc_s, status, stm, clock_running;
    uint8_t  last_from, last_to;   /* 64: none */
    int16_t  rating[2];
    uint8_t  pad[2];
    uint32_t left_ms[2];
    char     name[2][CW_NAME];
    uint8_t  board[32];
} cw_tv_game;
typedef struct { uint8_t v, kind; uint16_t n; uint32_t pad; cw_tv_game games[CW_TV_MAX]; } cw_tv;

typedef struct { char name[CW_NAME]; int16_t rating; uint16_t rd; uint32_t games; } cw_top_row;
typedef struct { uint16_t n; uint8_t speed; uint8_t pad[5]; cw_top_row rows[CW_TOP_MAX]; } cw_top;   /* games: rated, of the speed */

typedef struct {
    char     name[CW_NAME];
    int16_t  rating[CC_SPEEDS];   /* by speed (speed.h) */
    uint16_t rd[CC_SPEEDS];
    uint32_t rated[CC_SPEEDS];    /* rated games of each speed */
    uint32_t games, wins, losses, draws;   /* every game, rated or not */
    int16_t  puzzle;           /* the puzzle rating */
    uint16_t puzzle_rd;
    uint32_t puzzles_tried, puzzles_solved;
    uint16_t n_points;
    uint8_t  pad[6];           /* the points start 8-aligned */
    /* then n_points cw_point, by speed, oldest first in each */
} cw_profile;
typedef struct { int64_t at; int16_t rating; uint8_t speed; uint8_t pad[5]; } cw_point;

typedef struct {
    uint32_t game;
    uint8_t  status, winner, color, rated;   /* color: the listed user's */
    uint16_t base_s, ply;
    uint8_t  inc_s, pad;
    int16_t  diff;
    int64_t  ended_at;
    char     opponent[CW_NAME];
    uint8_t  pad2[4];
} cw_game_row;
typedef struct { uint16_t n; uint8_t pad[6]; cw_game_row rows[CW_LIST_MAX]; } cw_game_list;

_Static_assert(sizeof(cw_auth_req) == 96, "cw_auth_req");
_Static_assert(sizeof(cw_session) == 128, "cw_session");
_Static_assert(sizeof(cw_seek) == 4, "cw_seek");
_Static_assert(sizeof(cw_ai_req) == 8, "cw_ai_req");
_Static_assert(sizeof(cw_eval) == 48, "cw_eval");
_Static_assert(sizeof(cw_tv_game) == 100, "cw_tv_game");
_Static_assert(sizeof(cw_top_row) == 28, "cw_top_row");
_Static_assert(sizeof(cw_seek_ans) == 8, "cw_seek_ans");
_Static_assert(sizeof(cw_move_req) == 12, "cw_move_req");
_Static_assert(sizeof(cw_act_req) == 8, "cw_act_req");
_Static_assert(sizeof(cw_state) == 36, "cw_state");
_Static_assert(sizeof(cw_start) == 8, "cw_start");
_Static_assert(sizeof(cw_seek_row) == 32, "cw_seek_row");
_Static_assert(sizeof(cw_lobby) == 12 + 32 * CW_LOBBY_MAX, "cw_lobby");
_Static_assert(sizeof(cw_snap) == 96, "cw_snap");
_Static_assert(sizeof(cw_profile) == 88, "cw_profile");
_Static_assert(sizeof(cw_point) == 16, "cw_point");
_Static_assert(sizeof(cw_game_row) == 48, "cw_game_row");

/* A puzzle for the asker, with where the asker stands: its puzzle rating and counts. Only the
 * puzzle handed out last may be answered (POST /c/puzzle/result), once. */
typedef struct {
    int16_t   rating;          /* the asker's puzzle rating */
    uint16_t  rd;
    uint32_t  tried, solved;
    uint32_t  plays;           /* the puzzle's */
    cp_puzzle p;               /* its rating and rd are the puzzle's now */
} cw_puzzle;
typedef struct { uint32_t id; uint8_t win; uint8_t pad[3]; } cw_puzzle_req;
typedef struct {
    int16_t  before, after;    /* the asker's puzzle rating */
    int16_t  puzzle;           /* the puzzle's after */
    uint16_t rd;
    uint32_t tried, solved;
} cw_puzzle_done;
_Static_assert(sizeof(cw_puzzle) == 144, "cw_puzzle");
_Static_assert(sizeof(cw_puzzle_req) == 8, "cw_puzzle_req");
_Static_assert(sizeof(cw_puzzle_done) == 16, "cw_puzzle_done");

#endif
