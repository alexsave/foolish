#!/usr/bin/env bash
# Sizes: the generated modules, raw.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$here/build/sizes"
mkdir -p "$out"
make -s -C "$here" build/structgen
BOTS="$(make -s -C "$root/c" -f Makefile -f "$here/print.mk" sg-print-WASM_BOT_CFLAGS)"
"$here/build/structgen" --cwd "$root/c" --header game.h --root Game --build "bots=$BOTS" --ts "$out/game_full.bots.ts"
sz() { printf '  %-44s raw %6d\n' "$(basename "$1")" "$(wc -c < "$1")"; }
echo "generated modules:"
for f in "$out/game_full.bots.ts" "$root"/sdk/ts/gen/*.ts "$here"/gen/*.ts; do sz "$f"; done
[ -d "$here/build/oracle/node" ] && { echo "Node prototype (oracle phase) outputs:"; for f in "$here"/build/oracle/node/*.ts; do sz "$f"; done; }
