#!/bin/bash
# tune.sh - the tuner's rebuild, after tune (tune.c) has written a new tuning.tsv.
#
# Everything a page needs to play the new numbers, from ONE render: every take
# (only the WAVs whose bytes changed are rewritten and re-encoded), the take
# table every host plays by (c/build/gen/cue_takes.c), and the same table as bytes
# (build/cue_takes.bin) that an open page hands its kernel, so nothing reloads.
# The same render `npm run dev` runs, so what the tuner plays is what the build ships.
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
# The common gain is held while tuning (build/cache/gain, emptied with the cache), so
# a change re-renders and re-encodes only the takes that hold the part changed.
log=$(SOUNDGEN_CACHE=build/cache SOUNDGEN_GAIN_FILE=build/cache/gain ./build/soundgen write build/wav "$ct.tune" build/cue_takes.bin 2>&1) || { echo "$log"; exit 1; }
echo "$log" | grep -E "takes rendered|common gain|loudest card take"
SOUNDGEN_SAY_ENCODED=1 ./build/soundgen encode build/wav "$root/public/audio/gen"
./build/soundgen check build/wav "$root/public/audio/gen"
if cmp -s "$ct.tune" "$ct"; then
  rm -f "$ct.tune"
  # Newer than tuning.tsv, so c/Makefile does not render it all again.
  touch "$ct"
  echo "tune.sh: the take table is unchanged"
else
  # NOT relinked: an open page is handed the new table as bytes (build/cue_takes.bin,
  # GET /table, cue_table_load) and swaps the files it changed, with no reload. The
  # kernel links pick the table up at the next build (`npm run dev` or `make sounds`
  # relinks whenever cue_takes.c is newer), and until then a page loading in
  # development asks the endpoint for it (src/audio/cueAudio.ts).
  mv "$ct.tune" "$ct"
  echo "tune.sh: the take table changed"
fi
# What the page checks after it reloads, so it knows the dev server is serving
# the new kernel link and not the last one: every take's length and anchor and
# every cue's mix, summed (src/components/SoundTuner.tsx sums cueTakes() and cueMix() alike).
fp=$(awk '/^    \{ [0-9]/ { gsub(/[^0-9 -]/, " "); s += $2 + $3 }
          /^    \[[0-9]+\] = \{/ { gsub(/[^0-9 -]/, " "); s += $2 + $3 + $4 + $5 }
          END { printf "%d", s }' "$ct")
echo "tune.sh: table $fp"
echo "tune.sh: done in $(( $(date +%s) - t0 )) s"
