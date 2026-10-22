/* game.h - one game of chess with a clock: the authoritative state the server holds and the
 * browser mirrors (the same C, as wasm). Moves, the clock, every way a game ends (mate, stalemate,
 * resignation, the flag, agreement, threefold repetition, the fifty-move rule, insufficient
 * material, abort), and draw offers. No I/O and no allocation: a cc_game is a flat value.
 *
 * THE CLOCK. Times are milliseconds on the caller's clock (the server's monotonic clock; a test's
 * numbers). Nothing runs until Black's first move: before it each side has CC_FIRST_MOVE_MS to make
 * its first move or the game is aborted (cc_game_tick). From Black's first move on, the side to
 * move's time runs; a move deducts the time since the previous move and adds the increment. A side
 * whose time is gone loses on the next move it tries or the next tick, unless the other side
 * cannot mate (then it is a draw). */
#ifndef C_CHESS_GAME_H
#define C_CHESS_GAME_H

#include "chess.h"

#define CC_MAX_PLY 600            /* the game is drawn when it reaches this many plies */
#define CC_FIRST_MOVE_MS 30000    /* time to make one's first move before the game is aborted */
#define CC_REP_RING 128           /* repetition keys kept: more than the 100 plies a fifty-move draw allows */

/* LAG. A move reports how long its player thought (the client's own clock, from seeing the position
 * to sending the move); the rest of the time the server counts is the network's, and is given back
 * from a quota: it starts at CC_LAG_QUOTA_START, a move adds CC_LAG_QUOTA_GAIN, it never exceeds
 * CC_LAG_QUOTA_MAX, and what is given back is taken from it. A client that lies about its thinking
 * gains at most the quota. The flag waits for the same quota (at most CC_LAG_GRACE_MAX) so a move
 * in flight is not beaten by the server's own clock. */
#define CC_LAG_QUOTA_START 1000
#define CC_LAG_QUOTA_GAIN 150
#define CC_LAG_QUOTA_MAX 2000
#define CC_LAG_GRACE_MAX 1000

enum cc_status {
    CC_PLAYING = 1,
    CC_MATE = 2,
    CC_RESIGN = 3,
    CC_STALEMATE = 4,
    CC_FLAG = 5,              /* time ran out; winner set */
    CC_DRAW_AGREED = 6,
    CC_DRAW_REPETITION = 7,   /* the same position the third time (drawn automatically) */
    CC_DRAW_FIFTY = 8,        /* fifty moves by each side without a capture or a pawn move */
    CC_DRAW_MATERIAL = 9,     /* neither side can mate */
    CC_DRAW_FLAG_MATERIAL = 10, /* time ran out but the other side cannot mate */
    CC_DRAW_MAX_PLY = 11,
    CC_ABORTED = 12,
};
#define CC_NO_WINNER 2

/* What cc_game_move answers. */
enum cc_move_rc {
    CC_OK = 0,
    CC_E_OVER = -1,       /* the game has ended */
    CC_E_TURN = -2,       /* not this side's turn */
    CC_E_ILLEGAL = -3,    /* not a legal move here */
    CC_E_PLY = -4,        /* the move was made for another ply (a stale or repeated send) */
    CC_E_STATE = -5,      /* an offer or claim that does not apply now */
};

typedef struct {
    chess_pos pos;
    uint16_t  ply;                  /* plies played */
    uint8_t   status;               /* enum cc_status */
    uint8_t   winner;               /* CH_WHITE, CH_BLACK or CC_NO_WINNER */
    uint8_t   draw_offer;           /* bit 1 << color: that side offers a draw */
    uint8_t   pad[3];
    int32_t   base_ms, inc_ms;      /* the time control */
    int32_t   lag_quota[2];         /* ms of network lag each side may still be given back */
    int64_t   left_ms[2];           /* each side's remaining time as of turn_at */
    int64_t   turn_at;              /* when the side to move's turn began */
    int64_t   created_at;
    int64_t   ended_at;
    uint64_t  rep[CC_REP_RING];     /* position keys by ply (ring) */
    ch_move   moves[CC_MAX_PLY];
} cc_game;

void cc_game_new(cc_game *g, int32_t base_ms, int32_t inc_ms, int64_t now);
/* Whether the clock is running (from Black's first move until the end). */
bool cc_game_clock_running(const cc_game *g);
/* A side's time as of now (the side to move's runs down while the clock runs). */
int64_t cc_game_left(const cc_game *g, int color, int64_t now);
/* Plays the move for `color` at ply `ply` (which must be g->ply: a resend is refused, not
 * replayed). On CC_OK the game may have ended by it (g->status). A move whose mover's time has
 * already run out ends the game on the flag instead, and answers CC_E_OVER. */
int  cc_game_move(cc_game *g, int color, int ply, ch_move m, int64_t now);
/* The same, with the mover's own thinking time (ms; < 0: unknown, nothing given back). */
int  cc_game_move_lag(cc_game *g, int color, int ply, ch_move m, int64_t now, int32_t think_ms);
/* Ends the game when a clock has run out (past the side's grace) or a first move was not made in
 * time; true when the game ended by this call. The time until the next thing that could end the game is in *next_ms
 * (-1 when nothing runs). */
bool cc_game_tick(cc_game *g, int64_t now, int64_t *next_ms);
/* How long until the game could end by itself (a flag, past its grace, or the first-move abort):
 * ms, 0 when due, -1 when it is over. Reads only. */
int64_t cc_game_until(const cc_game *g, int64_t now);
int  cc_game_resign(cc_game *g, int color, int64_t now);
/* Abort: allowed before each side has made a move. */
int  cc_game_abort(cc_game *g, int color, int64_t now);
/* Offers a draw, or accepts the other side's offer (then the game is drawn). A move by the
 * offerer withdraws nothing; a move by the other side declines the offer. */
int  cc_game_draw(cc_game *g, int color, int64_t now);
int  cc_game_draw_decline(cc_game *g, int color);
bool cc_game_over(const cc_game *g);
/* 1, 0.5 or 0 for white as a score (the game must be over and not aborted). */
double cc_game_white_score(const cc_game *g);
/* Replays a game's moves from the start (the journal's recovery): CC_OK, or the error at the
 * first move that fails. Clocks are not replayed. */
int  cc_game_replay(cc_game *g, const ch_move *moves, int n);

#endif
