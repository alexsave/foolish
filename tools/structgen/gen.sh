#!/usr/bin/env bash
# Regenerate every structgen output with the libclang tool, one run per build.
# Layout flags come from c/Makefile itself, so a -D cap change there is picked up.
#
#   sdk/ts/gen/             the modules production TS imports (and the
#                           LAYOUT_HASH each wasm module is checked against)
#   tools/structgen/gen/    the generator's own genericity fixtures
#
#   gen.sh           write both
#   gen.sh --check   regenerate into a temp dir and fail if either is stale (the
#                    freshness gate, in the style of scripts/check_wasm_freshness.sh)
#
# game_layout.<build>.ts is ALSO written by the wasm make targets (c/Makefile,
# "Layout hash"), from the same specs/game_layout.args and the same flags, so
# `make -C c wasm-bots` after a header edit leaves the module and the wasm in
# agreement. This script is the full regeneration and the CI check.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
CLANG="${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}"
make -s -C "$here" build/structgen
SG="$here/build/structgen"
flags() { make -s -C "$root/c" -f Makefile -f "$here/print.mk" "sg-print-$1"; }
spec() { grep -v '^[[:space:]]*#' "$here/specs/$1.args"; }
RULES="$(flags WASM_RULES_CFLAGS)"
BOTS="$(flags WASM_BOT_CFLAGS)"
prod="$root/sdk/ts/gen"
fixtures="$here/gen"
check=0
if [ "${1:-}" = "--check" ]; then
  check=1
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  prod="$tmp/prod"; fixtures="$tmp/fixtures"
fi
mkdir -p "$prod" "$fixtures"

# The resident Game prefix the TS marshal reads and writes, per wasm build.
set -f   # the spec is split on whitespace, never globbed
# shellcheck disable=SC2207
GAME=(--cwd "$root/c" $(spec game_layout))
set +f
"$SG" "${GAME[@]}" --build "rules=$RULES" --ts "$prod/game_layout.rules.ts"
"$SG" "${GAME[@]}" --build "bots=$BOTS" --ts "$prod/game_layout.bots.ts"
if [ "$(sed -n 3p "$prod/game_layout.rules.ts")" = "$(sed -n 3p "$prod/game_layout.bots.ts")" ]; then
  echo "gen: rules and bots Game layouts are identical ($(sed -n 3p "$prod/game_layout.bots.ts"))"
else
  echo "gen: rules and bots Game layouts DIFFER - each host must load the module matching its wasm"
fi
"$SG" --cwd "$root/c" --header anim_plan.h --header legal.h --build "bots=$BOTS" \
  --root AnimPlan --root AnimBeats --root AnimEvent --root LegalMoves --ts "$prod/anim.bots.ts"

# Genericity fixture (test/verify.test.ts).
"$SG" --cwd "$here/test" --header kinds.h --root Kinds --build wasm= --const K_ --const KFLAG_ --ts "$fixtures/kinds.ts"

if [ "$check" = 1 ]; then
  stale=0
  diff -r "$root/sdk/ts/gen" "$prod" || stale=1
  diff -r -x verify.wasm "$here/gen" "$fixtures" || stale=1
  if [ "$stale" = 0 ]; then echo "gen: fresh"; else echo "gen: STALE - run tools/structgen/gen.sh"; exit 1; fi
  exit 0
fi
"$CLANG" --target=wasm32 -nostdlib -ffreestanding -O1 -I"$here/test" -I"$root/c/src" -isystem "$root/c/wasm/include" \
  -D_Thread_local= -DMAX_LOG_PAIRS=64 -DMAX_LEGAL_MOVES=4096 -DMAX_MOVE_CARDS=28 -DMAX_BATTLES=64 \
  -Wl,--no-entry -Wl,--export-all "$here/test/verify.c" -o "$fixtures/verify.wasm"
