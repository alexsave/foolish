#!/usr/bin/env bash
# Regenerate every structgen output with the libclang tool, one run per build.
# Layout flags come from c/Makefile itself, so a -D cap change there is picked up.
#
#   sdk/ts/gen/             the modules production TS imports: accessors, and
#                           layout_hash.<build>.ts, the LAYOUT_HASH each wasm
#                           module is checked against (its own module, so the
#                           browser can check it without importing a reader of
#                           the unmasked Game - e2e/security_client_boundary)
#   tools/structgen/gen/    the generator's own genericity fixtures
#
#   gen.sh           write both
#   gen.sh --check   regenerate into a temp dir and fail if either is stale (the
#                    freshness gate, in the style of scripts/check_wasm_freshness.sh)
#
# game_layout.<build>.ts and layout_hash.<build>.ts are ALSO written by the wasm make targets (c/Makefile,
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
"$SG" "${GAME[@]}" --build "rules=$RULES" --ts "$prod/game_layout.rules.ts" --hash-ts "$prod/layout_hash.rules.ts"
"$SG" "${GAME[@]}" --build "bots=$BOTS" --ts "$prod/game_layout.bots.ts" --hash-ts "$prod/layout_hash.bots.ts"
if [ "$(sed -n 3p "$prod/layout_hash.rules.ts")" = "$(sed -n 3p "$prod/layout_hash.bots.ts")" ]; then
  echo "gen: rules and bots Game layouts are identical ($(sed -n 3p "$prod/layout_hash.bots.ts"))"
else
  echo "gen: rules and bots Game layouts DIFFER - each host must load the module matching its wasm"
fi
"$SG" --cwd "$root/c" --header anim_plan.h --header legal.h --build "bots=$BOTS" \
  --root AnimPlan --root AnimBeats --root AnimEvent --root LegalMoves --ts "$prod/anim.bots.ts"

# The web client's reader of its slot (c/src/client_table.h): snapshot readers
# only, no accessor over the struct (specs/view_layout.args).
set -f
# shellcheck disable=SC2207
VIEW=(--cwd "$root/c" $(spec view_layout))
set +f
"$SG" "${VIEW[@]}" --build "bots=$BOTS" --ts "$prod/view_layout.bots.ts"

# The Oracle's Mode B candidate table (c/src/oracle_mt.h), read back from
# oracle-mt.wasm (specs/oracle_layout.args).
set -f
# shellcheck disable=SC2207
ORACLE=(--cwd "$root/c" $(spec oracle_layout))
set +f
"$SG" "${ORACLE[@]}" --build "oracle_mt=$(flags WASM_ORACLE_MT_CFLAGS)" --ts "$prod/oracle_layout.oracle_mt.ts"

# Genericity fixtures (test/verify.test.ts).
"$SG" --cwd "$here/test" --header kinds.h --root Kinds --build wasm= --const K_ --const KFLAG_ --ts "$fixtures/kinds.ts"
"$SG" --cwd "$here/test" --header snap.h --root Snap --root SPtr --build wasm= --snapshot Snap --snapshot SPtr --snapshot-only \
  --count Snap.pairs=n_pairs --count Snap.items=n_items --count Snap.text=n_text --count SItem.text=len --writer Snap \
  --count SPtr.vals=n_vals --count SPtr.items=n_items --count SPtr.name=name_len --count SPtr.none=n_none --ts "$fixtures/snap.ts"

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
