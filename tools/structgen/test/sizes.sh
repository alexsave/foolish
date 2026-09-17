#!/usr/bin/env bash
# Sizes: generated modules (raw), and what a consumer ships (esbuild --bundle
# --minify, then gzip -9) for the hand-written marshal vs the generated one.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$here/build/sizes"
mkdir -p "$out"
make -s -C "$here" build/structgen
[ -f "$here/build/harness/game_layout.ts" ] || bash "$here/test/harness.sh"
BOTS="$(make -s -C "$root/c" -f Makefile -f "$here/print.mk" sg-print-WASM_BOT_CFLAGS)"
"$here/build/structgen" --cwd "$root/c" --header game.h --root Game --build "bots=$BOTS" --ts "$out/game_full.bots.ts"
sz() { printf '  %-44s raw %6d\n' "$(basename "$1")" "$(wc -c < "$1")"; }
echo "generated modules:"
for f in "$out/game_full.bots.ts" "$root"/sdk/ts/gen/*.ts "$here"/gen/*.ts; do sz "$f"; done
[ -d "$here/build/oracle/node" ] && { echo "Node prototype (oracle phase) outputs:"; for f in "$here"/build/oracle/node/*.ts; do sz "$f"; done; }
printf "export { marshal, readState } from '../../test/game_marshal.ts';\n" > "$out/entry_gen.ts"
printf "export { legacyMarshal, legacyParse } from '../../test/legacy_marshal.ts';\n" > "$out/entry_legacy.ts"
echo "consumer bundles (esbuild --bundle --minify, gzip -9):"
for e in legacy gen; do
  "$root/node_modules/.bin/esbuild" "$out/entry_$e.ts" --bundle --minify --format=esm --platform=neutral --outfile="$out/$e.min.js" --log-level=error
  printf '  %-44s min %6d  gz %6d\n' "$e" "$(wc -c < "$out/$e.min.js")" "$(gzip -9 -c "$out/$e.min.js" | wc -c)"
done
