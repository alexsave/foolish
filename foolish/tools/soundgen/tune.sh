#!/bin/bash
# tune.sh - the tuner's rebuild, after tune (tune.c) has written a new tuning.tsv.
#
# Everything a page needs to play the new numbers, from ONE render: every take
# (only the WAVs whose bytes changed are rewritten and re-encoded), the take
# table every host plays by (c/build/gen/cue_takes.c), and the two kernel links
# that carry it (web.wasm for /sounds and the board, promo.wasm for the promo
# game, /ws), relinked only when the table changed. The same steps `npm run dev`
# runs, in the same order, so what the tuner plays is what the build ships.
set -euo pipefail
cd "$(dirname "$0")"
root=../..
t0=$(date +%s)
ct=$root/c/build/gen/cue_takes.c
mkdir -p build/wav "$root/public/audio/gen" "$(dirname "$ct")"
make -s build/soundgen
# Each take as rendered, so a change re-renders only the sets whose lines it
# touched (soundgen.c "THE TUNER'S CACHE"); emptied whenever soundgen is rebuilt.
bin=$(cksum < build/soundgen)
[ "$(cat build/cache/soundgen.cksum 2>/dev/null)" = "$bin" ] || { rm -rf build/cache; mkdir -p build/cache; echo "$bin" > build/cache/soundgen.cksum; }
log=$(SOUNDGEN_CACHE=build/cache ./build/soundgen write build/wav "$ct.tune" 2>&1) || { echo "$log"; exit 1; }
echo "$log" | grep -E "takes rendered|common gain|loudest card take"
SOUNDGEN_SAY_ENCODED=1 ./build/soundgen encode build/wav "$root/public/audio/gen"
./build/soundgen check build/wav "$root/public/audio/gen"
if cmp -s "$ct.tune" "$ct"; then
  rm -f "$ct.tune"
  # Newer than tuning.tsv, so c/Makefile does not render it all again.
  touch "$ct"
  echo "tune.sh: the take table is unchanged"
else
  mv "$ct.tune" "$ct"
  # scripts/wasm_build.sh's own targets, in one make and with no -j, as that script runs
  # them: each is a sub-make that regenerates build/layout/bots.hash (FORCE), and two
  # running side by side race on it (one rebuild in three failed with -j4).
  cc=${WASM_CC:-}
  [ -n "$cc" ] || [ "$(uname -s)" != "Darwin" ] || cc=/opt/homebrew/opt/llvm/bin/clang
  log=$(make -s -C "$root/c" ${cc:+WASM_CC="$cc"} wasm-web wasm-promo 2>&1) || { echo "$log"; exit 1; }
  echo "tune.sh: web.wasm and promo.wasm relinked with the new take table"
fi
# What the page checks after it reloads, so it knows the dev server is serving
# the new kernel link and not the last one: every take's length and anchor and
# every cue's mix, summed (src/components/SoundTuner.tsx sums cueTakes() and cueMix() alike).
fp=$(awk '/^    \{ [0-9]/ { gsub(/[^0-9 -]/, " "); s += $2 + $3 }
          /^    \[[0-9]+\] = \{/ { gsub(/[^0-9 -]/, " "); s += $2 + $3 + $4 + $5 }
          END { printf "%d", s }' "$ct")
echo "tune.sh: table $fp"
echo "tune.sh: done in $(( $(date +%s) - t0 )) s"
