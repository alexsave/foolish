/* game_keep.c - see game_keep.h. */
#include "game_keep.h"
#include "strategy.h"

void game_keep_canonical(Game *g) { state_canonical(g); }

int game_keep_put(const Game *g, unsigned char *out) { return state_blob_put(g, 0, out); }

static Game scratch; /* a Game is too big for the stack; the load is judged here first */

int game_keep_load(Game *g, const unsigned char *p, int len) {
  __builtin_memset(&scratch, 0, sizeof scratch);
  if (len <= STATE_BLOB_HEADER || state_blob_load(&scratch, p, len, 0) != 1) return 0;
  if (scratch.num_players != 2) return 0;
  if (scratch.status != GAME_STATUS_PLAYING && scratch.status != GAME_STATUS_GAME_OVER) return 0;
  scratch.players[0].strategy_key = STRATEGY_KEY_HUMAN;
  scratch.players[1].strategy_key = STRAT_HANDWRITTEN_PROD;
  __builtin_memcpy(g, &scratch, sizeof scratch);
  return 1;
}
