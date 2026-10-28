// botcup_adapter.h - the four Bot Cup contest bots (c/botcup/, copied unchanged from
// ~/Dev/botcup) as kernel brains: STRAT_BOTCUP_HAIKU .. STRAT_BOTCUP_KESTREL (strategy.h).
// Each choose is the referee's (~/Dev/botcup/kit/referee.c play_game) around the bot's own:
// the seat's masked view, the game's three random streams saved and restored around the call
// (a bot may run the engine on its own copies), and an index outside the menu clamped to 0
// and counted as a fault. Native builds only: no wasm module links the contest bots.
#ifndef CNITRO_BOTCUP_ADAPTER_H
#define CNITRO_BOTCUP_ADAPTER_H

#include "game.h"
#include "legal.h"

#include <stdbool.h>
#include <stdint.h>

// What `seat` may see of `g`, exactly referee.c's make_view: every other seat's hand cards, the
// deck's cards and every other seat's LOG_DRAW cards (but the face-up trump) become the
// placeholder {suit 0, value 1}; counts and everything face up stay real.
void botcup_make_view(Game *v, const Game *g, int seat);

// One choose as the referee asks it: `fn` sees the masked view of `seat`, the game's three random
// streams are put back after it, and an index outside the menu is 0 with `*fault` set.
typedef int (*BotcupChooseFn)(const Game *view, int seat, const LegalMoves *moves);
int botcup_run(BotcupChooseFn fn, const Game *g, int seat, const LegalMoves *moves, bool *fault);

// Run contest bot `strat` (a STRAT_BOTCUP_* id) for `seat`. -1 for another id or an empty menu.
int botcup_strategy_choose(int strat, const Game *g, int seat, const LegalMoves *moves);

// Per bot, since the process started: decisions, faults (an index outside the menu), and the
// time in its choose (total and the slowest), in microseconds.
typedef struct {
    long     decisions, faults;
    uint64_t total_us, max_us;
} BotcupStats;
void botcup_stats(int strat, BotcupStats *out);

#endif
