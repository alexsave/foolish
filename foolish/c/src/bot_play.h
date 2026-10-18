// The offline "play a bot game to the end" loop, once.
//
// Every arena, analysis CLI, research tool and full-game test plays its games
// the same way: collect the seats should_bot_act says may act, shuffle them,
// and give each in turn its legal menu until one seat's chosen move applies;
// stop when the game is over, nobody may act, nobody's move applied, or a turn
// cap is reached. That body was written out sixteen times (docs: the 2026-10-17
// duplication audit, family 3), so a rules change to who may act had sixteen
// places to be missed.
//
// A STEP API, NOT A CALLBACK. Each caller does its own work around a move -
// prints it, records a training row, asks a second bot, hashes the state - and
// with a callback that work would need a chooser function plus a per-move
// function plus a context struct. docs/shared-consolidation/SUMMARY.md set the
// limit at one callback plus parameters. Here the caller keeps the loop and
// this file keeps the rules of it:
//
//     BotPlay bp; bot_play_init(&bp, &g, 4000, BOT_PLAY_SHUFFLE, NULL);
//     for (int seat; (seat = bot_play_next(&bp)) >= 0; ) {
//         int idx = choose(&g, seat, &bp.moves);
//         if (idx < 0 || idx >= bp.moves.n) continue;   // next seat this turn
//         ...per-move work...
//         bot_play_apply(&bp, idx);
//     }
//
// THE DRAWS ARE THE ONES EVERY COPY MADE. The shuffle is a Fisher-Yates from the
// top with game_random() (or, for the harnesses that keep the game streams
// untouched, rng_u32 on their own state), made at the start of each turn and
// nowhere else, so a seeded arena or test plays the game it played before.
//
// Host programs only: no shipped module links this file (the live cycle is
// bot_drive.c, which paces, bundles and leases, and robusta's rollouts keep
// their own loop over shared scratch with lite menus and no shuffle).
#ifndef CNITRO_BOT_PLAY_H
#define CNITRO_BOT_PLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "game.h"
#include "legal.h"

// How a turn orders the seats that may act.
enum {
    BOT_PLAY_SHUFFLE    = 0,  // Fisher-Yates, as the server's bot loop picks
    BOT_PLAY_SEAT_ORDER = 1,  // lowest seat first, no draw at all
};

typedef struct BotPlay {
    Game     *g;
    uint64_t *order_rng;      // NULL: shuffle with game_random(); else rng_u32(order_rng)
    int       order_mode;     // BOT_PLAY_SHUFFLE or BOT_PLAY_SEAT_ORDER
    int       max_turns;
    int       turns;          // turns begun so far
    int       order[MAX_PLAYERS];
    int       n_order;
    int       next;           // the next index into order[] this turn
    bool      in_turn;
    bool      acted;          // a move applied this turn
    bool      stopped;
    int       seat;           // the seat bot_play_next last returned
    LegalMoves moves;         // that seat's full menu
} BotPlay;

void bot_play_init(BotPlay *p, Game *g, int max_turns, int order_mode, uint64_t *order_rng);

// The next seat to choose for, with its menu in p->moves (never empty), or -1:
// the game is over, nobody may act, the last turn's seats all failed to move,
// or max_turns turns have begun. A seat skipped by the caller (an index out of
// range, a move it will not apply) simply falls through to the next seat.
int bot_play_next(BotPlay *p);

// Apply p->moves.moves[idx] for p->seat with legal_move_apply. True when it
// applied, which ends the turn; false (an index out of range included) leaves
// the turn to the next seat.
bool bot_play_apply(BotPlay *p, int idx);

// The same outcome for a move the caller applied itself (awire_apply, say):
// `applied` true ends the turn. Returns `applied`.
bool bot_play_took(BotPlay *p, bool applied);

#endif
