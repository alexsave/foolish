/* kernel_glue.c - what the kernel needs from its host in this module.
 *
 * The allocator is cdom's: it defines malloc, free, realloc and calloc (weak) over its own heap,
 * so the kernel's calls and cdom's land in one heap with nothing here.
 *
 * ONE BRAIN. bot_drive asks the roster (bot_roster.c) for a seat's brain,
 * and the roster's dispatch reaches every brain the server ladder ships:
 * the Monte-Carlo searches, cordite's solver and its leaf book. The table
 * here seats one handwritten bot, so the two roster calls bot_drive makes
 * answer for that brain alone and the linker drops the rest. */
#include "bot_roster.h"
#include "strategy.h"

int bot_roster_find_by_strat(int strat) { return strat == STRAT_HANDWRITTEN_PROD ? 0 : -1; }

int bot_roster_choose(int idx, const Game *g, int seat, const LegalMoves *moves) {
  if (idx != 0 || !moves || moves->n <= 0) return -1;
  int chosen = handwritten_prod_strategy_choose(g, seat, moves, 0);
  return chosen >= 0 && chosen < moves->n ? chosen : -1;
}
