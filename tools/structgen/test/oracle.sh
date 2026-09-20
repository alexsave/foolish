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
# The generator is shared (shared/tools/structgen); this test, and the specs
# and fixtures it points the generator at, are this product's.
sg="$root/shared/tools/structgen"
CLANG="${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}"
out="$here/build/oracle"
rm -rf "$out" && mkdir -p "$out/node" "$out/c"
make -s -C "$sg" build/structgen
# --no-print-directory: `-s` does not silence "Entering directory", and this
# output is CAPTURED into compiler flags. Harmless from a shell, five lines of
# English in $CFLAGS the day anything calls this from a make recipe.
BOTS="$(make -s --no-print-directory -C "$root/c" -f Makefile -f "$sg/print.mk" sg-print-WASM_BOT_CFLAGS)"
# WASM_MSG_CFLAGS, not WASM_RULES_CFLAGS. rules.wasm was retired in 81bd7715
# and that variable went with it, so this line had been expanding to the EMPTY
# STRING ever since: the run below compiled with no -Isrc, failed to find
# game.h, and took this whole test down with it. Nothing noticed, because
# oracle.sh runs in no workflow. The msg module is the live second flag set -
# a different -O, different caps - which is all this test wants from it: two
# builds that disagree enough to catch an emitter that only works for one.
MSG="$(make -s --no-print-directory -C "$root/c" -f Makefile -f "$sg/print.mk" sg-print-WASM_MSG_CFLAGS)"
[ -n "$MSG" ] || { echo "oracle.sh: WASM_MSG_CFLAGS came back empty - has the build been renamed again?" >&2; exit 1; }

run() { # name cwd build args...
    local name="$1" dir="$2" b="$3"; shift 3
    node "$sg/structgen.mjs" --clang "$CLANG" --cwd "$dir" --build "$b" "$@" --ts "$out/node/$name.ts"
    "$sg/build/structgen" --cwd "$dir" --build "$b" "$@" --ts "$out/c/$name.ts"
}
run game_msg "$root/c" "msg=$MSG" --header game.h --root Game
run game_bots "$root/c" "bots=$BOTS" --header game.h --root Game
run kinds "$sg/test" "wasm=" --header kinds.h --root Kinds
run anim "$root/c" "bots=$BOTS" --header anim_plan.h --header legal.h \
    --root AnimPlan --root AnimBeats --root AnimEvent --root LegalMoves
if diff -r "$out/node" "$out/c"; then
  echo "oracle: identical ($(ls "$out/c" | wc -l | tr -d ' ') files, $(cat "$out"/c/*.ts | wc -c | tr -d ' ') bytes)"
else
  echo "oracle: differs - expected since the design changes; check every hunk against the list above"
fi
