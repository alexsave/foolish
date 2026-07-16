#!/usr/bin/env bash
# Dedicated protagonist decision-latency pass for octogen pc2. For each candidate
# CD_TT_BITS, replays N games with CD_LAT=1 (times dispatch_choose with
# CLOCK_PROCESS_CPUTIME_ID) and ACCUMULATES total CPU-ns + decision count, so the
# average is stable and you can add more games later. Writes data/latency.json.
#
#   tools/tt_divergence_viz/lat_pass.sh [games-per-config] [BITS...]
#
# NOTE: pc2 games are ~1.5 s CPU each, so a literal 10k/config across every size
# is ~11 core-hours; the mean converges long before that. Run repeatedly to add
# samples toward whatever total you want.
set -euo pipefail
trap 'echo "[error] line $LINENO: \"$BASH_COMMAND\" exited $?" >&2' ERR
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CNITRO="$(cd "$HERE/../.." && pwd)"
CC=${CC:-cc}; BOT=${BOT:-octogen}; PC=${PC:-2}; SEED0=${SEED0:-800000}
J=${J:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )}
N=${1:-1000}; [ "$#" -gt 0 ] && shift || true
if [ "$#" -gt 0 ]; then BITS=("$@"); else BITS=(10 11 12 13 14 15 16 22); fi
LAT="$HERE/data/lat"; mkdir -p "$LAT"
throttle() { while [ "$(jobs -rp | wc -l)" -ge "$J" ]; do wait "$(jobs -rp | head -1)"; done; }

echo "[lat] ${BOT}_pc${PC}  bits=${BITS[*]}  +${N} games/config  jobs=$J"
build_bin() { local b=$1 out="$HERE/eval_sig${b}"
  [ -x "$out" ] && [ "$out" -nt "$CNITRO/src/main_eval.c" ] && return
  local CORE; CORE=$(cd "$CNITRO" && make -s print-core)
  ( cd "$CNITRO" && $CC -O2 -ffast-math -Isrc -Wno-deprecated-declarations \
      -DCD_TT_BITS="$b" $CORE src/main_eval.c -o "$out" -lm ); }
for b in "${BITS[@]}"; do build_bin "$b" & throttle; done; wait

per=$(( N / J )); rem=$(( N - per*J ))
for b in "${BITS[@]}"; do
  acc="$LAT/${BOT}_pc${PC}_tt${b}.lat"; touch "$acc"
  # fresh seeds past this config's banked games (each line = one batch's ns games)
  have=$(awk '{g+=$3}END{print g+0}' "$acc")
  start=$(( SEED0 + have ))
  work=$(mktemp -d); off=$start; t0=$SECONDS
  for (( i=0; i<J; i++ )); do
    g=$per; [ "$i" -lt "$rem" ] && g=$(( per+1 )); [ "$g" -gt 0 ] || continue
    CD_LAT=1 CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75 "$HERE/eval_sig${b}" \
      --strategy="$BOT" --opp=handwritten --players="$PC" --games="$g" --seed-start="$off" \
      >/dev/null 2>"$work/l_$i.err" &
    off=$(( off+g )); throttle
  done
  wait
  # one accumulator line per batch: "<total_ns> <decisions> <games>"
  cat "$work"/l_*.err | awk -v G="$N" '/^LAT/{ns+=$2;n+=$3} END{if(n)print ns" "n" "G}' >> "$acc"
  rm -rf "$work"
  tot=$(awk '{ns+=$1;n+=$2;g+=$3}END{printf "%d %d %d", ns, n, g}' "$acc")
  read tns tn tg <<<"$tot"
  printf "  TT%-3s  %6.2f ms/decision  (%d games, %d decisions, %ds)\n" \
    "$b" "$(awk -v ns=$tns -v n=$tn 'BEGIN{print ns/n/1e6}')" "$tg" "$tn" "$(( SECONDS-t0 ))"
done
node "$HERE/lat.mjs" > "$HERE/data/latency.json"
node "$HERE/build.mjs" >/dev/null
echo "[lat] wrote data/latency.json + rebuilt HTML"
