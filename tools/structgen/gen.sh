#!/usr/bin/env bash
# Regenerate every structgen output with the libclang tool, one run per build.
# Layout flags come from c/Makefile itself, so a -D cap change there is picked up.
#   gen.sh           write gen/
#   gen.sh --check   regenerate into a temp dir and fail if gen/ is stale (the
#                    freshness gate, in the style of scripts/check_wasm_freshness.sh)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
CLANG="${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}"
make -s -C "$here" build/structgen
SG="$here/build/structgen"
flags() { make -s -C "$root/c" -f Makefile -f "$here/print.mk" "sg-print-$1"; }
RULES="$(flags WASM_RULES_CFLAGS)"
BOTS="$(flags WASM_BOT_CFLAGS)"
out="$here/gen"
check=0
if [ "${1:-}" = "--check" ]; then check=1; out="$(mktemp -d)"; trap 'rm -rf "$out"' EXIT; fi
mkdir -p "$out"

# The resident Game prefix the TS marshal reads and writes, per wasm build.
GAME=(--cwd "$root/c" --header game.h --root Game
      --fields "Game=status,num_players,power_suit,first_attacker,defender,num_battles,deck_count,discard_pile_length,has_flipped,deterministic_deck,flipped,deck,table_battles,players,elimination_order,num_eliminated,good_players_mask,has_good_timestamp"
      --fields "Player=status,hand_count,awaiting_attack,strategy_key,hand"
      --const GAME_STATUS_ --const PLAYER_STATUS_)
"$SG" "${GAME[@]}" --build "rules=$RULES" --ts "$out/game_layout.rules.ts"
"$SG" "${GAME[@]}" --build "bots=$BOTS" --ts "$out/game_layout.bots.ts"
tail -n +3 "$out/game_layout.rules.ts" > "$out/.rules.body"
tail -n +3 "$out/game_layout.bots.ts" > "$out/.bots.body"
if cmp -s "$out/.rules.body" "$out/.bots.body"; then
  echo "gen: rules and bots Game layouts are identical ($(head -n 1 "$out/.bots.body"))"
else
  echo "gen: rules and bots Game layouts DIFFER - each host must load the module matching its wasm"
fi
rm -f "$out/.rules.body" "$out/.bots.body"

# Genericity fixtures (test/verify.test.ts).
"$SG" --cwd "$here/test" --header kinds.h --root Kinds --build wasm= --const K_ --const KFLAG_ --ts "$out/kinds.ts"
"$SG" --cwd "$root/c" --header anim_plan.h --header legal.h --build "bots=$BOTS" \
  --root AnimPlan --root AnimBeats --root AnimEvent --root LegalMoves --ts "$out/anim.ts"

if [ "$check" = 1 ]; then
  if diff -r -x verify.wasm "$here/gen" "$out"; then echo "gen: fresh"; else echo "gen: STALE - run tools/structgen/gen.sh"; exit 1; fi
  exit 0
fi
"$CLANG" --target=wasm32 -nostdlib -ffreestanding -O1 -I"$here/test" -I"$root/c/src" -isystem "$root/c/wasm/include" \
  -D_Thread_local= -DMAX_LOG_PAIRS=64 -DMAX_LEGAL_MOVES=4096 -DMAX_MOVE_CARDS=28 -DMAX_BATTLES=64 \
  -Wl,--no-entry -Wl,--export-all "$here/test/verify.c" -o "$out/verify.wasm"
