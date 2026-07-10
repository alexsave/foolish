#!/usr/bin/env bash
# Local workflow for the transposition-table divergence page.
#
#   Open docs/tt-divergence.html in a browser, then keep running `measure` to
#   add samples — each call APPENDS to the working-set histograms, re-fits the
#   curves + confidence bands, and rewrites the self-contained HTML. Refresh the
#   browser to see it update. Nothing is served; it is a plain local file.
#
# Usage:
#   tools/tt_divergence_viz/generate.sh                       # just rebuild HTML from data/
#   tools/tt_divergence_viz/generate.sh measure BOT PC GAMES  # add samples, rebuild
#   tools/tt_divergence_viz/generate.sh measure octogen 2 4000
#   tools/tt_divergence_viz/generate.sh sweep BOT "2 3 4" 1500 # measure several PCs
#
# Data (all under data/, checked in so the page renders without re-measuring):
#   data/W/<bot>_pc<pc>.hist   accumulating working-set histograms (append-only)
#   data/ccdf.json             aggregated CCDF curves (rebuilt from data/W)
#   data/measured.json         hand-entered direct-divergence points (tt_divergence.sh)
#
# Seeds are independent, so GAMES are sharded by seed across all cores; repeated
# `measure` calls auto-advance the seed range past what's already collected, so
# you never re-measure the same games. Portable to macOS /bin/bash 3.2.
set -euo pipefail
trap 'echo "[error] line $LINENO: \"$BASH_COMMAND\" exited $?" >&2' ERR
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CNITRO="$(cd "$HERE/../.." && pwd)"
CC=${CC:-cc}
SEED0=${SEED0:-42424}
J=${J:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )}
mkdir -p "$HERE/W" "$HERE/data/W"
throttle() { while [ "$(jobs -rp | wc -l)" -ge "$J" ]; do wait "$(jobs -rp | head -1)"; done; }

# Build the CD_TT_STATS evaluator once (TT22 = collision-free, so the histogram
# is the true distinct-key working set, not a collided undercount).
ensure_bin() {
  BIN="$HERE/eval_stats22"
  [ -x "$BIN" ] && [ "$BIN" -nt "$CNITRO/src/cordite_sim.c" ] && return
  echo "[build] compiling CD_TT_STATS evaluator (TT22)..."
  local CORE; CORE=$(cd "$CNITRO" && make -s print-core)
  ( cd "$CNITRO" && $CC -O2 -ffast-math -Isrc -Wno-deprecated-declarations \
      -DCD_TT_STATS -DCD_TT_BITS=22 $CORE src/main_eval.c -o "$BIN" -lm )
}

# distinct games already banked for a cell = distinct seeds in the .gw file
banked() { [ -s "$1" ] || { echo 0; return 0; }; awk '{print $1}' "$1" | sort -u | wc -l | tr -d ' '; }

# measure one (bot, pc) cell: shard GAMES by seed across cores, APPEND per-game
# seed-keyed records, then DEDUP on seed so re-measuring never double-counts.
measure_cell() {
  local bot=$1 pc=$2 games=$3
  local acc="$HERE/data/W/${bot}_pc${pc}.gw"
  local have; have=$(banked "$acc")
  local seed=$(( SEED0 + have ))                 # fresh seeds past what's banked
  local S=$J per rem i off g work
  S=$J; per=$(( games / S )); rem=$(( games - per*S ))
  work=$(mktemp -d)
  echo "[measure] $bot pc$pc  +$games games (had $have) seeds $seed.. across $S shards"
  local t0=$SECONDS off=$seed
  # heartbeat: deep (low player-count) cells take minutes; show progress
  ( while :; do sleep 15; echo "[measure]   ...still running ($(( SECONDS - t0 ))s)"; done ) &
  local hb=$!; disown 2>/dev/null || true
  for (( i=0; i<S; i++ )); do
    g=$per; [ "$i" -lt "$rem" ] && g=$(( per + 1 ))
    [ "$g" -gt 0 ] || continue
    throttle
    ( CD_GW=1 CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75 "$BIN" \
        --strategy="$bot" --opp=handwritten --players="$pc" \
        --games="$g" --seed-start="$off" 2>/dev/null | grep '^GW' \
        | awk '{print $2" "$3}' > "$work/s_$i.gw" ) &
    off=$(( off + g ))
  done
  wait
  kill "$hb" 2>/dev/null || true
  # merge new records into the accumulator, deduping on seed (deterministic games)
  touch "$acc"
  sort -k1,1n -u "$acc" "$work"/s_*.gw > "$work/merged.gw"
  mv "$work/merged.gw" "$acc"
  rm -rf "$work"
  echo "[measure] $bot pc$pc done: now $(banked "$acc") distinct games ($(( SECONDS - t0 ))s)"
}

rebuild() {
  node "$HERE/ccdf.mjs" "$HERE/data/W" > "$HERE/data/ccdf.json"
  node "$HERE/build.mjs"
  echo "[done] refresh docs/tt-divergence.html in your browser"
}

cmd=${1:-rebuild}
case "$cmd" in
  measure) ensure_bin; measure_cell "$2" "$3" "$4"; rebuild ;;
  sweep)   ensure_bin; for pc in $3; do measure_cell "$2" "$pc" "$4"; done; rebuild ;;
  rebuild) rebuild ;;
  *) echo "usage: generate.sh [rebuild | measure BOT PC GAMES | sweep BOT \"PCs\" GAMES]" >&2; exit 2 ;;
esac
