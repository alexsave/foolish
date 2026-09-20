#!/usr/bin/env bash
# Sizes: the generated modules, raw.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/../.." && pwd)"
# The generator is shared (shared/tools/structgen); this test, and the specs
# and fixtures it points the generator at, are this product's.
sg="$root/shared/tools/structgen"
out="$here/build/sizes"
mkdir -p "$out"
make -s -C "$sg" build/structgen
# --no-print-directory: `-s` does not silence "Entering directory", and this
# output is CAPTURED into compiler flags. Harmless from a shell, five lines of
# English in $CFLAGS the day anything calls this from a make recipe.
BOTS="$(make -s --no-print-directory -C "$root/c" -f Makefile -f "$sg/print.mk" sg-print-WASM_BOT_CFLAGS)"
"$sg/build/structgen" --cwd "$root/c" --header game.h --root Game --build "bots=$BOTS" --ts "$out/game_full.bots.ts"
sz() { printf '  %-44s raw %6d\n' "$(basename "$1")" "$(wc -c < "$1")"; }
echo "generated modules:"
for f in "$out/game_full.bots.ts" "$root"/sdk/ts/gen/*.ts "$here"/gen/*.ts; do sz "$f"; done
[ -d "$here/build/oracle/node" ] && { echo "Node prototype (oracle phase) outputs:"; for f in "$here"/build/oracle/node/*.ts; do sz "$f"; done; }
