/* engine.h - Stockfish as child processes speaking UCI over pipes, for the server's own uses (play
 * against the computer; later analysis and puzzles). A bounded queue of searches in front of a
 * fixed set of engine processes, run by one manager thread with poll(); a search's answer comes back
 * through a callback on that thread. The engines run at background priority so the move path, on
 * the loop thread, is never the one that waits; the queue refuses past its bound instead of growing.
 *
 * A child process, not a library: an engine crash is a restarted process, not a server down, and the
 * NNUE net, embedded in the executable's read-only pages, is shared by every engine through the page
 * cache. DESIGN.md has the measurements behind the choice.
 *
 * Settings: CC_ENGINE (the stockfish executable; unset: no engine), CC_ENGINES (processes, default 2),
 * CC_ENGINE_HASH_MB (per process, default 16), CC_ENGINE_QUEUE (searches waiting, default 256). */
#ifndef C_CHESS_ENGINE_H
#define C_CHESS_ENGINE_H

#include "chess.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    ch_move  best;       /* CH_MOVE_NONE: no answer (no legal move, or the engine failed) */
    int      score_cp;   /* from the side to move's view; a mate is +-(30000 - plies) */
    int      depth;
    uint64_t nodes;
    int      time_ms;
    int      mate;       /* moves to mate from the side to move's view (+ it mates), 0 when none */
    int      npv;
    ch_move  pv[16];     /* the principal variation, best first */
    int      nmulti;     /* with ask.multipv > 1: the best lines' first moves and scores, best first */
    struct { ch_move move; int score_cp, mate; } multi[4];
} cc_engine_answer;

typedef void (*cc_engine_fn)(void *ud, const cc_engine_answer *a);

typedef struct {
    const ch_move *moves;   /* the game from the start position */
    int            n;
    int            skill;      /* UCI "Skill Level" 0..20 (20: full strength) */
    int            movetime_ms;
    int            depth;      /* 0: no depth limit */
    uint64_t       nodes;      /* 0: no node limit */
    int            multipv;    /* lines to keep (1..4; 0 means 1) */
    const char    *fen;        /* NULL: the start position; else the position the moves start from */
} cc_engine_ask;

int  cc_engine_start(void);    /* CB_OK, or CB_OK with no engine when CC_ENGINE is unset */
void cc_engine_stop(void);
bool cc_engine_available(void);
/* Queues a search (the moves are copied): CB_OK, or CB_E_FULL past the queue's bound, or
 * CB_E_STATE with no engine. fn runs on the engine thread, never under the caller's locks. */
int  cc_engine_search(const cc_engine_ask *ask, cc_engine_fn fn, void *ud);
/* Counters for /c/stats. */
void cc_engine_counts(uint64_t *searches, uint64_t *nodes, uint32_t *queued, uint32_t *busy);

#endif
