#!/usr/bin/env bash
# Builds the parity/bench harness into tools/structgen/build/harness:
#   head/          `git archive $KERNEL_REV` of c/src + c/wasm. Pinned to the commit
#                  legacy_marshal.ts was copied from (a844b2a1, the byte wire that
#                  shipped), so the comparison does not move under later kernel work.
#   game_layout.ts generated accessors for that Game under the bots.wasm flags
#   kernel.wasm    test/kernel.c + the HEAD state codec, compiled with the same
#                  flags and -DSG_LAYOUT_HASH from structgen --print-hash
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$here/build/harness"
CLANG="${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}"
make -s -C "$here" build/structgen
rm -rf "$out" && mkdir -p "$out/head"
git -C "$root" archive "${KERNEL_REV:-a844b2a1}" c/src c/wasm | tar -x -C "$out/head"
FLAGS="$(make -s -C "$root/c" -f Makefile -f "$here/print.mk" sg-print-WASM_BOT_CFLAGS)"

GEN=(--cwd "$out/head/c" --header game.h --root Game --build "bots=$FLAGS"
     --fields "Game=status,num_players,power_suit,first_attacker,defender,num_battles,deck_count,discard_pile_length,has_flipped,deterministic_deck,flipped,deck,table_battles,players,elimination_order,num_eliminated,good_players_mask,has_good_timestamp,logs"
     --fields "Player=status,hand_count,awaiting_attack,strategy_key,hand,name"
     --const GAME_STATUS_ --const PLAYER_STATUS_)
"$here/build/structgen" "${GEN[@]}" --ts "$out/game_layout.ts"
HASH="$("$here/build/structgen" "${GEN[@]}" --print-hash)"

cd "$out/head/c"
# shellcheck disable=SC2086 # FLAGS is a flag list
"$CLANG" $FLAGS -O2 -DSG_LAYOUT_HASH="$HASH" -Wno-unused-function \
  "$here/test/kernel.c" src/game.c src/deal_rng.c src/view.c \
  -Wl,--no-entry -Wl,--export-memory \
  -Wl,--export=k_game,--export=k_io,--export=k_layout_hash,--export=k_import,--export=k_import_keys \
  -Wl,--export=k_export,--export=k_set_deterministic_deck,--export=k_adopt,--export=k_human_mask \
  -Wl,--export=k_pickup,--export=k_transition,--export=k_refill \
  -o "$out/kernel.wasm"
echo "harness: layout $HASH, kernel.wasm $(wc -c < "$out/kernel.wasm" | tr -d ' ') bytes"
