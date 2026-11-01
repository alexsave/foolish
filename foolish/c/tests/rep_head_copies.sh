#!/bin/bash
# THE ONE WAY TO COPY A HEAD (game.h game_copy_head). The repetition ring lies
# past Game.logs, where a head-sized or short-log slot has no storage, and only
# a Game whose rep_live is set touches it. A raw memcpy of a Game prefix into
# other storage would carry rep_live along and let the engine write the ring
# past the slot. So no shipped C copies a Game prefix with memcpy except
# game_copy_head itself (game.c), which clears the flag. Run from c/.
# MUTATION-CHECKED: game_copy_head's call in cordite_sim.c solve_clone_prefix
# put back to `memcpy(dst, src, offsetof(Game, logs));` fails this script.
set -u
cd "$(dirname "$0")/.."
pat='memcpy\([^;]*(offsetof\(Game, *(logs|num_logs)\)|TABLE_HEAD_BYTES|TABLE_SNAP_BYTES|GAME_PREFIX_SIZE|RS_GAME_PREFIX|CLIENT_DELTA_SNAP_BYTES|SOLVE_CHILD_STRIDE)'
hits=$(grep -rnE "$pat" src wasm ios ../server/src 2>/dev/null | grep -v '^src/game\.c:' || true)
if [ -n "$hits" ]; then
  echo "rep_head_copies: a Game prefix copied without game_copy_head (game.h):"
  echo "$hits"
  exit 1
fi
echo "rep_head_copies: every Game prefix copy goes through game_copy_head"
