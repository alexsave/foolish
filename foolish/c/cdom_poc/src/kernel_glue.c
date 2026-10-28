/* kernel_glue.c - what the kernel needs from its host in this module.
 *
 * 1. ONE ALLOCATOR. cdom's allocator and the kernel's bump allocator
 *    (shared/c/wasm/bump_alloc.c) would both start at __heap_base and hand out
 *    the same bytes. The kernel's malloc is answered by cdom's instead.
 *
 * 2. ONE BRAIN. bot_drive asks the roster (bot_roster.c) for a seat's brain,
 *    and the roster's dispatch reaches every brain the server ladder ships:
 *    the Monte-Carlo searches, cordite's solver and its leaf book. The table
 *    here seats one handwritten bot, so the two roster calls bot_drive makes
 *    answer for that brain alone and the linker drops the rest. */
#include "../vendor/cdom.h"
#include "bot_roster.h"
#include "strategy.h"
#include <stddef.h>

void *malloc(size_t n) { return cd_alloc((uint32_t)n); }
void free(void *p) { cd_free(p); }
void *calloc(size_t n, size_t sz) {
  size_t t = n * sz;
  if (sz && t / sz != n) return 0;
  unsigned char *p = cd_alloc((uint32_t)t);
  for (size_t i = 0; i < t; i++) p[i] = 0;
  return p;
}

int bot_roster_find_by_strat(int strat) { return strat == STRAT_HANDWRITTEN_PROD ? 0 : -1; }

int bot_roster_choose(int idx, const Game *g, int seat, const LegalMoves *moves) {
  if (idx != 0 || !moves || moves->n <= 0) return -1;
  int chosen = handwritten_prod_strategy_choose(g, seat, moves, 0);
  return chosen >= 0 && chosen < moves->n ? chosen : -1;
}
