#!/usr/bin/env bash
# Oracle: the libclang tool (structgen.c) against the Node prototype
# (structgen.mjs, which scrapes clang -fdump-record-layouts), same inputs,
# single-build runs, TypeScript diffed byte for byte.
#
# HISTORY. This was run BEFORE the design changes and was byte-identical on all
# four outputs (Game under rules and bots flags, Kinds, AnimPlan/AnimBeats/
# AnimEvent/LegalMoves: 4 files, 38,438 bytes); a deliberately broken bit
# position in the C tool turned it red. structgen.c has since changed its output
# on purpose, so a diff here is EXPECTED and each hunk should be one of:
#   header "// build:" and no LAYOUT_HASH (structgen.c writes it to --hash-ts
#   instead; the prototype has no such flag); enum fields typed by the enum's real
#   integer type (unsigned enums read with getUint32); "p" for "p + 0" and
#   "+ i0" for "+ i0 * 1"; raw_get/raw_set on 1/2/4-byte records; pack/unpack on
#   all-bitfield records; char[N] _str helpers; 2-byte bitfield windows
#   (getUint16) and a parenthesized wide-bitfield setter.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/../.." && pwd)"
CLANG="${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}"
out="$here/build/oracle"
rm -rf "$out" && mkdir -p "$out/node" "$out/c"
make -s -C "$here" build/structgen
BOTS="$(make -s -C "$root/c" -f Makefile -f "$here/print.mk" sg-print-WASM_BOT_CFLAGS)"
RULES="$(make -s -C "$root/c" -f Makefile -f "$here/print.mk" sg-print-WASM_RULES_CFLAGS)"

run() { # name cwd build args...
    local name="$1" dir="$2" b="$3"; shift 3
    node "$here/structgen.mjs" --clang "$CLANG" --cwd "$dir" --build "$b" "$@" --ts "$out/node/$name.ts"
    "$here/build/structgen" --cwd "$dir" --build "$b" "$@" --ts "$out/c/$name.ts"
}
run game_rules "$root/c" "rules=$RULES" --header game.h --root Game
run game_bots "$root/c" "bots=$BOTS" --header game.h --root Game
run kinds "$here/test" "wasm=" --header kinds.h --root Kinds
run anim "$root/c" "bots=$BOTS" --header anim_plan.h --header legal.h \
    --root AnimPlan --root AnimBeats --root AnimEvent --root LegalMoves
if diff -r "$out/node" "$out/c"; then
  echo "oracle: identical ($(ls "$out/c" | wc -l | tr -d ' ') files, $(cat "$out"/c/*.ts | wc -c | tr -d ' ') bytes)"
else
  echo "oracle: differs - expected since the design changes; check every hunk against the list above"
fi
